#include "draw_trace_diag.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"
#include "alternating_eye.h"
#include "companion_bridge.h"
#include "camera_matrix_test_hook.h"
#include "projection_aspect_hook.h"
#include "aiming_controller_hook.h"
#include "boat_seat.h"
#include "driving_camera_hook.h"
#include "ui_pass.h"
#include "native_stereo.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/d3d_util.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace mohw {
namespace {

// A/B TEST (2026-09-22, CONFIRMED): kEnabled=false skipped InstallOnce entirely and fixed a return of the
// true-frustum ghosting after a long session -- not the UI-snapshot logic itself, but the raw overhead of
// intercepting every draw call and render-target bind in the WHOLE frame (not just the tail UI bind) via all six
// vtable hooks below. The position-filter fix itself (hooks/projection_aspect_hook.cpp) was never touched and isn't
// the issue. Re-enabled now with a LEAN footprint instead of all-or-nothing: only OMSetRenderTargets and Draw are
// hooked by default (see kFullTraceHooksEnabled for the other four). Draw is kept (not dropped along with the rest)
// because the tonemap/UI composite's own first draw is very likely a non-indexed fullscreen-triangle Draw call --
// missing it would break the "snapshot after draw N" counting the UI-snapshot mechanism depends on. DrawIndexed
// itself is never hooked here regardless -- constantbuffer_hook.cpp already owns that vtable slot and bridges into
// this file via DrawTraceNoteDraw, so counting still sees indexed UI-element draws with zero additional hooks.
constexpr bool kEnabled = true;
// Diagnostic-only hooks (2026-09-22): DrawIndexedInstanced, DrawInstanced, ClearRenderTargetView, CopyResource.
// Only needed for the RICH Scroll-Lock draw-by-draw trace log (annotating instanced draws, clears and copies) --
// not needed for the UI-snapshot mechanism itself, and instanced/clear/copy calls are common throughout WORLD
// rendering (shadow maps, G-buffers, foliage/particles), so leaving them unhooked by default cuts real overhead.
// Flip to true only while actively re-investigating draw-index boundaries; the trace log is just less detailed
// without it (missing those four event types) rather than broken.
constexpr bool kFullTraceHooksEnabled = false;

constexpr const char* kLogFile = "mohwvr_drawtrace.log";

// ID3D11DeviceContext vtable indices (d3d11.h order).
constexpr int kIdxDrawIndexed = 12, kIdxDraw = 13, kIdxDrawIndexedInstanced = 20, kIdxDrawInstanced = 21, kIdxOMSetRenderTargets = 33,
              kIdxClearRenderTargetView = 50, kIdxCopyResource = 47;

using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using ClearRTVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);

OMSetRenderTargetsFn g_origOM = nullptr;
DrawIndexedFn g_origDrawIndexed = nullptr;
DrawFn g_origDraw = nullptr;
DrawIndexedInstancedFn g_origDrawIndexedInstanced = nullptr;
DrawInstancedFn g_origDrawInstanced = nullptr;
ClearRTVFn g_origClear = nullptr;
CopyResourceFn g_origCopy = nullptr;

bool g_installed = false;
std::atomic<bool> g_armed{false};
std::atomic<bool> g_tracing{false};
std::mutex g_mutex;
std::vector<std::string> g_lines;
ID3D11Texture2D* g_backbuffer = nullptr;
DWORD g_traceThread = 0;

// UI-snapshot capture -- UNCONDITIONAL, every frame, not gated by Scroll Lock tracing. See draw_trace_diag.h's
// GetUiSnapshotTexture comment and this file's top for the plan. Single-threaded-context assumption is deliberate,
// same as the rest of this file (the game's real rendering happens on one D3D11 immediate context).
bool g_curTargetIsBackbuffer = false;
int g_backbufferDrawCount = 0;
constexpr int kUiSnapshotAfterDraw = 1; // snapshot after this many draws land under a backbuffer bind -- see the .h comment
ID3D11Texture2D* g_uiSnapshotTex = nullptr;
UINT g_uiSnapshotW = 0, g_uiSnapshotH = 0;
DXGI_FORMAT g_uiSnapshotFmt = DXGI_FORMAT_UNKNOWN;

// current-target aggregation
std::string g_curTargetDesc;
long long g_curDraws = 0;
long long g_curVerts = 0;

void FlushCurrent()
{
    if (g_curTargetDesc.empty())
        return;
    char buf[256];
    snprintf(buf, sizeof(buf), "  -> %lld draws (%lld vertices/indices)", g_curDraws, g_curVerts);
    g_lines.push_back(g_curTargetDesc + buf);
    g_curDraws = 0;
    g_curVerts = 0;
}

std::string DescribeTarget(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    char buf[256];
    if (n == 0 || !rtvs || !rtvs[0])
    {
        snprintf(buf, sizeof(buf), "RT: (none) depth=%s", dsv ? "yes" : "no");
        return buf;
    }
    ID3D11Resource* res = nullptr;
    rtvs[0]->GetResource(&res);
    std::string s = "RT: ";
    if (res)
    {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
        {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            snprintf(buf, sizeof(buf), "%ux%u fmt=%d samples=%u %s numRTV=%u depth=%s", d.Width, d.Height, static_cast<int>(d.Format),
                     d.SampleDesc.Count, (tex == g_backbuffer) ? "**BACKBUFFER**" : "", n, dsv ? "yes" : "no");
            s += buf;
            tex->Release();
        }
        res->Release();
    }
    return s;
}

bool OnTraceThread()
{
    return g_tracing.load(std::memory_order_relaxed) && GetCurrentThreadId() == g_traceThread;
}

void EnsureUiSnapshotTex(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& bbDesc)
{
    if (g_uiSnapshotTex && g_uiSnapshotW == bbDesc.Width && g_uiSnapshotH == bbDesc.Height && g_uiSnapshotFmt == bbDesc.Format &&
        TextureOnDevice(g_uiSnapshotTex, device))
        return;
    if (g_uiSnapshotTex)
    {
        g_uiSnapshotTex->Release();
        g_uiSnapshotTex = nullptr;
    }
    D3D11_TEXTURE2D_DESC d = bbDesc;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE; // for the eventual companion diff pass; unused by the BMP dump (it stages its own copy)
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g_uiSnapshotTex)))
    {
        g_uiSnapshotW = bbDesc.Width;
        g_uiSnapshotH = bbDesc.Height;
        g_uiSnapshotFmt = bbDesc.Format;
    }
    else
    {
        MOHW_LOG(kLogFile, "EnsureUiSnapshotTex: CreateTexture2D(%ux%u fmt=%d) FAILED", bbDesc.Width, bbDesc.Height,
                  static_cast<int>(bbDesc.Format));
    }
}

// HUD PLACEMENT (2026-09-26): the game draws its HUD over the whole backbuffer, laid out for a flat screen, so in
// true-frustum mode each eye showed it centred on that eye's (off-centre) image rather than on where the eye looks,
// and the two copies didn't fuse. After draw 1 (the world composite, per draw_trace_diag.h) the rest of the frame is
// the HUD, so one viewport placed here moves all of it. The target is a single head-locked rectangle, centred on head
// forward (moved by HudOffsetX/Y) and sized HudScale of the eye's real field of view, so the HUD stays out towards the
// corners. Projected into this eye with its real tangents (the eye image maps linearly onto them) and its eye offset:
// image x = (t - eyeOffset/HudDepth - centre) / halfWidth, eyeOffset = the rendering eye offset * HudIpdScale (so the
// HUD's IPD follows the world's). Vertically it is centred on head forward too (2026-09-30): the vertical frustum is
// as lopsided as the horizontal one (Quest 3: 44 deg up, 55 down), so the image's middle is ~13 deg below forward --
// centred there, the reticle (the rectangle's centre) sat that far below the game's aim.
// A frame whose whole final pass is 2D: a pre-rendered movie, a menu or a loading screen. In gameplay draw 1 of the
// final pass is the 3D world composite (kept full-frame, its IPD comes from the camera) and only the draws after it are
// placed; in a 2D frame draw 1 is itself 2D (the movie frame, the menu background), so it's placed too and the whole
// frame becomes one head-locked screen whose eye copies fuse. Told apart by whether the world camera got the true
// per-eye frustum recently (hooks/projection_aspect_hook.cpp).
// The in-game (pause) menu: drawn over a still-rendering world, in the same final-pass draw numbers the reticle and the
// objective markers use in play, so those special cases must switch off while it is up. Told apart by the simulation:
// AimingController::Update runs every ~33 ms tick in play and (expected, being verified live 2026-09-30 -- every change
// is logged to mohwvr_hudlayers.log) stops while paused. Also true wherever else the simulation stops, which is fine:
// the reticle and markers aren't meaningful then either.
// DISABLED 2026-09-30 (kPauseDetectionEnabled): the user saw HUD items briefly show unplaced in each eye before
// correcting, and asked to switch it off for now. Still computed and logged (so its transitions can be matched against
// such moments), but always reports "not paused".
constexpr bool kPauseDetectionEnabled = true;

bool IsSimulationPaused()
{
    constexpr unsigned long long kPausedAfterMs = 300;
    // Driving missions (game\MOHWD.exe): no aim updates there; the game's committed view stopping changing instead
    // (hooks/driving_camera_hook.h DrivingSimulationPaused).
    unsigned long long last = g_drivingExe ? GetDrivingViewChangedMs() : GetLastAimingUpdateMs();
    unsigned long long now = GetTickCount64();
    bool paused = last != 0 && now - last > kPausedAfterMs;
    static bool lastPaused = false; // render thread only
    if (paused != lastPaused)
    {
        lastPaused = paused;
        MOHW_LOG("mohwvr_hudlayers.log", "simulation %s (%s %llu ms ago, final-pass draw %d)%s",
                  paused ? "PAUSED" : "RUNNING again", g_drivingExe ? "view change" : "aim update", now - last,
                  g_backbufferDrawCount + 1,
                  kPauseDetectionEnabled ? "" : " -- detection disabled, no effect");
    }
    return kPauseDetectionEnabled && paused;
}

// The main menu (2026-09-30): it renders a real 3D scene behind the menu, at gameplay-like camera rates (4,857/s), so
// the rate test calls it 3D -- but it doesn't get the per-eye camera (the frustum match jumps between many camera
// objects), so its background had no IPD. There is no player then: no aim update and no first-person body for a while,
// which in play only happens in cutscenes and loads. Then the whole frame is placed like a movie.
// IN A LEVEL (2026-10-16): the robot-control section has no aim update and no first-person body either (the robot has
// its own controls), and after 3 s it turned into a floating screen per eye. It isn't the main menu: the main menu is
// only reached before any play or through a loading screen. So the player having aimed since the last stretch of
// 2D-only frames (over 1 s: a loading screen, a movie) counts as being in a level, and that is never the main menu.
// world = this frame's WorldRenderedRecently, passed in so the 2D stretches are seen every frame.
bool NoPlayerForAWhile(bool world)
{
    constexpr unsigned long long kNoPlayerMs = 3000;
    constexpr unsigned long long kLevelEndsAfter2DMs = 1000;
    unsigned long long now = GetTickCount64();
    static bool inLevel = false;              // render thread only
    static unsigned long long twoDSinceMs = 0; // render thread only
    // A boat seat is in a level too: a checkpoint there loads straight into it, with no aim to have seen.
    if (IsPlayerAimActive() || GetBoatSeatRaw() != BoatSeat::None)
        inLevel = true;
    if (world)
        twoDSinceMs = 0;
    else if (twoDSinceMs == 0)
        twoDSinceMs = now;
    else if (inLevel && now - twoDSinceMs > kLevelEndsAfter2DMs)
    {
        inLevel = false;
        MOHW_LOG("mohwvr_hudlayers.log", "left the level (2D-only frames for %llu ms)", now - twoDSinceMs);
    }
    unsigned long long lastAim = GetLastAimingUpdateMs();
    bool noAim = lastAim == 0 || now - lastAim > kNoPlayerMs;
    static unsigned long long bodyLastSeenMs = 0; // render thread only
    if (IsPlayerSkeletonLoaded())
        bodyLastSeenMs = now;
    bool noBody = bodyLastSeenMs == 0 || now - bodyLastSeenMs > kNoPlayerMs;
    return noAim && noBody && !inLevel;
}

bool Is2DFrame()
{
    constexpr unsigned kWorldRecentMs = 200; // rate window, see WorldRenderedRecently
    // Driving missions (game\MOHWD.exe): no world-camera rate is measured there; a frame is a real per-eye view while
    // the driving camera hook gives the main view its stereo, and the whole-frame floating screen otherwise.
    bool twoD = false;
    if (g_drivingExe)
        twoD = !DrivingStereoActive();
    else
    {
        bool world = WorldRenderedRecently(kWorldRecentMs);
        bool noPlayer = NoPlayerForAWhile(world); // every frame, so its level tracking sees the 2D stretches
        twoD = !world || noPlayer;
    }
    static bool lastTwoD = false; // render thread only
    if (twoD != lastTwoD)
    {
        lastTwoD = twoD;
        MOHW_LOG("mohwvr_hudlayers.log", "whole-frame 2D placement %s%s", twoD ? "ON" : "off",
                  twoD && WorldRenderedRecently(kWorldRecentMs) ? " (no player: main menu)" : "");
    }
    return twoD;
}

// The last Is2DFrame decision, for other threads (IsWhole2DFrameNow), and when it last changed. Render thread writes.
std::atomic<bool> g_whole2DNow{false};
std::atomic<unsigned long long> g_whole2DSinceMs{0};

void PublishWhole2DFrame()
{
    const bool twoD = Is2DFrame();
    if (g_whole2DSinceMs.load(std::memory_order_relaxed) == 0 || twoD != g_whole2DNow.load(std::memory_order_relaxed))
    {
        g_whole2DNow.store(twoD, std::memory_order_relaxed);
        g_whole2DSinceMs.store(GetTickCount64(), std::memory_order_relaxed);
    }
}

void PlaceHudForThisEye(ID3D11DeviceContext* ctx, UINT width, UINT height, float s, bool keepAspect);

// Before the first draw of the final pass in a 2D frame: place it (and, the viewport being state, everything after).
void MaybePlaceWhole2DFrame(ID3D11DeviceContext* ctx)
{
    // Native two-view: each half is an eye; the UI is placed per half inside its own pass instead (PlaceNativeHud).
    if (!g_curTargetIsBackbuffer || g_backbufferDrawCount != 0 || !g_backbuffer || IsNativeStereoActive() || !Is2DFrame())
        return;
    D3D11_TEXTURE2D_DESC bbDesc{};
    g_backbuffer->GetDesc(&bbDesc);
    PlaceHudForThisEye(ctx, bbDesc.Width, bbDesc.Height, GetMenuScreenScale(), true);
}

// keepAspect (whole 2D frames): the image fills the eye's whole field of view, which isn't the image's shape (Quest 3:
// 2.22 x 2.39 in tangents for a 1280x720 image, so a pixel shows at about half its width), so a rectangle that is s of
// the view both ways squashed the movie horizontally. With keepAspect it has the image's own width:height instead,
// as large as fits inside s of the view. Off (the HUD over the world), it stays s of the view both ways, out towards
// the corners.
// The placed rectangle for the eye being rendered, as fractions of the frame: left, top, width, height (y down).
// Any thread (the marker projection hook asks from the game thread). False when there is no placement.
bool ComputeHudRectForEye(int eye, float s, bool keepAspect, UINT width, UINT height, float rect[4])
{
    if (!GetHudPlacementEnabled() || eye < 0 || eye > 1)
        return false;
    mohwvr::ipc::HmdViewBlock hmd{};
    if (!GetHmdView(&hmd) || hmd.viewMode != mohwvr::ipc::kViewModeTrueFrustum)
        return false;
    float tL = tanf(hmd.angleLeft[eye]), tR = tanf(hmd.angleRight[eye]);
    float tU = tanf(hmd.angleUp[eye]), tD = tanf(hmd.angleDown[eye]);
    float centreX = (tL + tR) * 0.5f, halfW = (tR - tL) * 0.5f, halfH = (tU - tD) * 0.5f;
    if (halfW < 0.05f || halfH < 0.05f)
        return false;
    // s: HudScale for the HUD over the 3D world, MenuScreenScale for a whole 2D frame (see Is2DFrame).
    if (!(s >= 0.2f && s <= 1.5f))
        s = 0.75f;
    constexpr float kDegToRad = 3.14159265f / 180.0f;
    float depth = GetHudDepth(); // meters, 0.1 minimum; HudIpdScale 0 puts the HUD at infinity
    if (!(depth >= 0.1f))
        depth = 0.1f;
    float ipdScale = GetHudIpdScale();
    if (!(ipdScale >= 0.0f && ipdScale <= 10.0f))
        ipdScale = 1.0f;
    // A whole 2D frame (keepAspect: a movie, a menu, a loading screen) is a screen at its own distance, fused with the
    // rendering eye offset like a real one (2026-11-08): the HUD's 0.1 m and x0.2 put it at 0.5 m, cross-eyed.
    if (keepAspect)
    {
        depth = GetMenuScreenDepth();
        ipdScale = 1.0f;
    }
    float offsetXDeg = 0.0f, offsetYDeg = 0.0f;
    GetHudOffsetDeg(&offsetXDeg, &offsetYDeg);
    float eyeX = (eye == 1 ? 1.0f : -1.0f) * fabsf(GetEyeOffsetAlongRow0(eye == 1)) * ipdScale; // right eye sits at +x
    // Half extents of the placed rectangle, in tangents.
    float rectHalfW = s * halfW, rectHalfH = s * halfH;
    if (keepAspect && width > 0 && height > 0)
    {
        float imageAspect = static_cast<float>(width) / static_cast<float>(height);
        if (rectHalfH * imageAspect <= rectHalfW)
            rectHalfW = rectHalfH * imageAspect;
        else
            rectHalfH = rectHalfW / imageAspect;
    }
    float centreTan = tanf(offsetXDeg * kDegToRad) - eyeX / depth;
    float ndcLeft = (centreTan - rectHalfW - centreX) / halfW;
    float ndcRight = (centreTan + rectHalfW - centreX) / halfW;
    float centreY = (tU + tD) * 0.5f; // the image middle's tangent (tD is negative); 0 = head forward
    float ndcUpShift = (tanf(offsetYDeg * kDegToRad) - centreY) / halfH;
    float heightFraction = rectHalfH / halfH;

    rect[0] = (ndcLeft + 1.0f) * 0.5f;
    rect[1] = (1.0f - heightFraction - ndcUpShift) * 0.5f;
    rect[2] = (ndcRight - ndcLeft) * 0.5f;
    rect[3] = heightFraction;
    return rect[2] > 0.01f && rect[3] > 0.01f;
}

// The same for the eye this frame renders (one eye per frame).
bool ComputeHudRect(float s, bool keepAspect, UINT width, UINT height, float rect[4])
{
    return ComputeHudRectForEye(IsRightEyeActive() ? 1 : 0, s, keepAspect, width, height, rect);
}

// When the HUD (not a whole 2D frame) was last placed: the marker projection only maps into the rectangle then.
std::atomic<unsigned long long> g_hudPlacedMs{0};

// KEEP THE PLACEMENT (2026-11-12): the viewport the UI was placed into under this backbuffer bind (render thread),
// put back before each later draw under the bind if the game has set another since. The placement used to be set once
// (at the first draw, or after the world's) and left as state. With the UI layout size off 1 (hooks/ui_pass.cpp) the
// user saw "double images, which become further separated the greater the value", anchored to one side: the UI's
// rectangle is no longer the whole screen then, and the game presumably sets it as the viewport per movie, which undid
// the placement for every movie after the first: drawn at the same spot in both eyes, so at another depth than the
// placed screen. Each such viewport is logged (the first few), to confirm. Two-view places every HUD draw already.
bool g_placedVpValid = false;
D3D11_VIEWPORT g_placedVp{};

void KeepPlacedViewport(ID3D11DeviceContext* ctx)
{
    if (!g_placedVpValid || !g_curTargetIsBackbuffer || IsNativeStereoActive())
        return;
    D3D11_VIEWPORT current{};
    UINT count = 1;
    ctx->RSGetViewports(&count, &current);
    if (count == 1 && current.TopLeftX == g_placedVp.TopLeftX && current.TopLeftY == g_placedVp.TopLeftY &&
        current.Width == g_placedVp.Width && current.Height == g_placedVp.Height)
        return;
    static int lines = 0; // render thread
    if (lines < 16)
    {
        ++lines;
        MOHW_LOG(kLogFile,
                 "the game set the viewport x=%.1f y=%.1f w=%.1f h=%.1f under the placed UI (draw %d of the bind, %s): "
                 "placed again at x=%.1f y=%.1f w=%.1f h=%.1f",
                 current.TopLeftX, current.TopLeftY, current.Width, current.Height, g_backbufferDrawCount + 1,
                 Is2DFrame() ? "whole 2D frame" : "HUD over the world", g_placedVp.TopLeftX, g_placedVp.TopLeftY,
                 g_placedVp.Width, g_placedVp.Height);
    }
    ctx->RSSetViewports(1, &g_placedVp);
}

// THE DRIVING MISSIONS' HUD IN TWO-VIEW (2026-11-18): MOHWD.exe has no UI pass hook (hooks/ui_pass.cpp is MOHW.exe's),
// so its HUD would be drawn once across the whole side-by-side frame -- half of it in each eye. Instead each HUD draw
// into the backbuffer is drawn twice, once into each eye's HUD rectangle (ComputeHudRectForEye, the whole half with the
// placement off), its viewport and scissor rectangle mapped from the frame into that rectangle; the game's are put back
// after. A HUD draw is told apart from the views' own by its viewport, wider than half the frame (a view draws into its
// half), and by not being the bind's first draw (where a whole-frame composite would be). The first decisions are logged.
template <typename DrawCall>
bool DrawIntoBothHalves(ID3D11DeviceContext* ctx, DrawCall draw)
{
    if (!g_drivingExe || !g_curTargetIsBackbuffer || !g_backbuffer || !IsNativeStereoActive())
        return false;
    D3D11_TEXTURE2D_DESC bb{};
    g_backbuffer->GetDesc(&bb);
    const float W = static_cast<float>(bb.Width), H = static_cast<float>(bb.Height), half = W * 0.5f;
    UINT vpCount = 0;
    ctx->RSGetViewports(&vpCount, nullptr);
    D3D11_VIEWPORT vp{};
    if (vpCount >= 1)
    {
        vpCount = 1;
        ctx->RSGetViewports(&vpCount, &vp);
    }
    const bool hud = vpCount == 1 && vp.Width > half + 1.0f && g_backbufferDrawCount >= 1 && W >= 2.0f && H >= 1.0f;
    static int lines = 0; // render thread
    if (lines < 40)
    {
        ++lines;
        MOHW_LOG(kLogFile, "driving two-view: backbuffer draw %d, viewport x=%.0f y=%.0f w=%.0f h=%.0f of %.0fx%.0f: %s",
                 g_backbufferDrawCount + 1, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, W, H,
                 hud ? "HUD, drawn into both eyes' HUD rectangles" : "a view's own, left as it is");
    }
    if (!hud)
        return false;
    UINT scCount = 0;
    ctx->RSGetScissorRects(&scCount, nullptr);
    D3D11_RECT sc{};
    if (scCount >= 1)
    {
        scCount = 1;
        ctx->RSGetScissorRects(&scCount, &sc);
    }
    for (int eye = 0; eye < 2; ++eye)
    {
        float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        float placed[4];
        if (ComputeHudRectForEye(eye, GetHudScale(), false, bb.Width, bb.Height, placed))
            memcpy(rect, placed, sizeof(rect));
        const float ex = static_cast<float>(eye) * half + rect[0] * half, ew = rect[2] * half;
        const float ey = rect[1] * H, eh = rect[3] * H;
        D3D11_VIEWPORT mapped = vp;
        mapped.TopLeftX = ex + vp.TopLeftX / W * ew;
        mapped.Width = vp.Width / W * ew;
        mapped.TopLeftY = ey + vp.TopLeftY / H * eh;
        mapped.Height = vp.Height / H * eh;
        ctx->RSSetViewports(1, &mapped);
        if (scCount == 1)
        {
            D3D11_RECT r{};
            r.left = static_cast<LONG>(ex + static_cast<float>(sc.left) / W * ew);
            r.right = static_cast<LONG>(ex + static_cast<float>(sc.right) / W * ew + 0.5f);
            r.top = static_cast<LONG>(ey + static_cast<float>(sc.top) / H * eh);
            r.bottom = static_cast<LONG>(ey + static_cast<float>(sc.bottom) / H * eh + 0.5f);
            ctx->RSSetScissorRects(1, &r);
        }
        draw();
    }
    ctx->RSSetViewports(1, &vp);
    if (scCount == 1)
        ctx->RSSetScissorRects(1, &sc);
    return true;
}

void PlaceHudForThisEye(ID3D11DeviceContext* ctx, UINT width, UINT height, float s, bool keepAspect)
{
    float rect[4];
    if (!ComputeHudRect(s, keepAspect, width, height, rect))
        return;
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = rect[0] * static_cast<float>(width);
    vp.Width = rect[2] * static_cast<float>(width);
    vp.TopLeftY = rect[1] * static_cast<float>(height);
    vp.Height = rect[3] * static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    g_placedVp = vp;
    g_placedVpValid = true;
    if (!keepAspect)
        g_hudPlacedMs.store(GetTickCount64(), std::memory_order_relaxed);

    static int lines = 0;
    if (lines < 6)
    {
        ++lines;
        MOHW_LOG(kLogFile, "HUD placed for eye %d: viewport x=%.1f y=%.1f w=%.1f h=%.1f of %ux%u (scale %.2f)",
                  IsRightEyeActive() ? 1 : 0, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, width, height, s);
    }
}

// Unconditional (every frame, not gated by tracing) -- see g_curTargetIsBackbuffer's declaration comment.
void MaybeSnapshotUi(ID3D11DeviceContext* ctx)
{
    if (!g_curTargetIsBackbuffer || !g_backbuffer)
        return;
    ++g_backbufferDrawCount;
    if (g_backbufferDrawCount != kUiSnapshotAfterDraw)
        return;
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (!device)
        return;
    D3D11_TEXTURE2D_DESC bbDesc{};
    g_backbuffer->GetDesc(&bbDesc);
    EnsureUiSnapshotTex(device, bbDesc);
    if (g_uiSnapshotTex)
        ctx->CopyResource(g_uiSnapshotTex, g_backbuffer); // recorded right after this draw's commands -- correct GPU ordering, no sync needed
    device->Release();
    // Native two-view: draw 1 is a view's composite and the HUD is placed per half inside its own pass (PlaceNativeHud).
    if (IsNativeStereoActive())
        return;
    bool whole2D = Is2DFrame();
    PlaceHudForThisEye(ctx, bbDesc.Width, bbDesc.Height, whole2D ? GetMenuScreenScale() : GetHudScale(), whole2D);
}

bool g_capturingPass = false; // render thread: this final pass is being logged (markers pass capture, below)
void MaybeStartPassCapture();

void STDMETHODCALLTYPE Hook_OM(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    if (OnTraceThread())
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        FlushCurrent();
        g_curTargetDesc = DescribeTarget(n, rtvs, dsv);
    }
    // Unconditional: every OMSetRenderTargets call starts a fresh "which draw number is this under this bind" count.
    g_curTargetIsBackbuffer = (n > 0 && rtvs && rtvs[0]) ? [&] {
        ID3D11Resource* res = nullptr;
        rtvs[0]->GetResource(&res);
        bool isBb = false;
        if (res)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
            {
                isBb = (tex == g_backbuffer);
                tex->Release();
            }
            res->Release();
        }
        return isBb;
    }() : false;
    g_backbufferDrawCount = 0;
    g_placedVpValid = false; // a new bind places again (KeepPlacedViewport)
    if (g_curTargetIsBackbuffer)
    {
        IsSimulationPaused(); // logs each change, whether or not anything below asks
        PublishWhole2DFrame(); // every frame, for the view count (hooks/native_stereo.cpp)
        MaybeStartPassCapture();
    }
    else
        g_capturingPass = false;
    g_origOM(ctx, n, rtvs, dsv);
}

// Whether this frame's final pass into the backbuffer has drawn anything yet -- the last thing in a frame, so a camera
// rebuild after it belongs to the NEXT frame (hooks/driving_camera_hook.cpp, 2026-10-14). Reset at Present.
std::atomic<bool> g_finalPassStarted{false};

// For the traced frame (2026-11-11, the two-view cutscene overlay hunt): what the first draw under a bind reads -- the
// pixel shader, the textures in slots 0-3 (size and format) and the viewport -- so a pass that samples a whole-frame
// texture into one view's half shows itself.
std::string DescribeDrawInputs(ID3D11DeviceContext* ctx)
{
    char buf[512];
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ID3D11ShaderResourceView* srvs[4] = {};
    ctx->PSGetShaderResources(0, 4, srvs);
    D3D11_VIEWPORT vp{};
    UINT vpCount = 1;
    ctx->RSGetViewports(&vpCount, &vp);
    int used = snprintf(buf, sizeof(buf), "      first draw: PS=%p viewport=(%.0f,%.0f %.0fx%.0f) tex:", static_cast<void*>(ps),
                        vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
    for (int i = 0; i < 4; ++i)
    {
        char t[64] = " -";
        if (srvs[i])
        {
            ID3D11Resource* res = nullptr;
            srvs[i]->GetResource(&res);
            ID3D11Texture2D* tex = nullptr;
            if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
            {
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                snprintf(t, sizeof(t), " %ux%u/%d%s", d.Width, d.Height, static_cast<int>(d.Format),
                         tex == g_backbuffer ? "(BB)" : "");
                tex->Release();
            }
            else
                snprintf(t, sizeof(t), " (not 2D)");
            if (res)
                res->Release();
            srvs[i]->Release();
        }
        if (used > 0 && used < static_cast<int>(sizeof(buf)))
            used += snprintf(buf + used, sizeof(buf) - used, "%s", t);
    }
    if (ps)
        ps->Release();
    return buf;
}

void CountDraw(ID3D11DeviceContext* ctx, UINT verts)
{
    if (g_curTargetIsBackbuffer)
        g_finalPassStarted.store(true, std::memory_order_relaxed);
    if (OnTraceThread())
    {
        const bool first = g_curDraws == 0;
        const std::string inputs = first ? DescribeDrawInputs(ctx) : std::string();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (first)
            g_lines.push_back(g_curTargetDesc + "\n" + inputs);
        ++g_curDraws;
        g_curVerts += verts;
    }
    MaybeSnapshotUi(ctx);
}

// RETICLE HIDE (2026-09-28): the reticle is draws 2..7 under the tail backbuffer bind in gameplay (walked and confirmed
// 2026-09-22, see project memory). Menus draw into the same bind, so the same draw numbers can be menu elements; the
// hide only applies while the first-person viewmodel is being drawn (IsPlayerSkeletonLoaded: gameplay), which keeps
// the main menu and loading screens intact, but can still clip parts of an in-game (pause) menu -- hence the setting.
// Skipped draws are still counted, so later draw numbers (and the HUD placement after draw 1) are unaffected.
constexpr int kReticleFirstDraw = 2;
constexpr int kReticleLastDraw = 7;

// The texture a final-pass draw samples (PS slot 0): its size and format identify the element type (captured
// 2026-09-30, mohwvr_hudlayers.log): reticle 128x64, weapon display 256x64, D-pad display 32x32, objective markers
// 512x128, friendly markers 16x32 -- the icons BC3 (format 77); text is the 1024x1024 R8 font atlas. Draw numbers
// aren't usable: they shift with what the HUD shows, and the reticle isn't always drawn.
constexpr UINT kFormatBC3 = 77; // DXGI_FORMAT_BC3_UNORM

bool GetDrawTexture(ID3D11DeviceContext* ctx, UINT* width, UINT* height, UINT* format)
{
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv)
        return false;
    bool ok = false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (res)
    {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
        {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            *width = d.Width;
            *height = d.Height;
            *format = d.Format;
            ok = true;
            tex->Release();
        }
        res->Release();
    }
    srv->Release();
    return ok;
}

// ---- Native two-view HUD (2026-11-08, hooks/native_stereo.h) -------------------------------------------------------
// The HUD list is replayed once per half (hooks/ui_pass.cpp). Before each of its draws into the backbuffer the viewport
// is that eye's HUD rectangle inside that eye's half: the halves Present hands over (hooks/present_hook.cpp). The game's
// own viewport isn't kept: it's the flat-screen frame either way, which the one-eye placement replaces too. Not for a
// draw sampling a frame-sized texture (a world composite), in case one is ever drawn inside the pass.
thread_local int t_nativeHudReplay = -1;
thread_local int t_nativeHudDraws = 0;

// For a backbuffer draw inside a per-half HUD replay: counts it and returns its number in the replay (1 = the first).
// 0 for every other draw.
int NativeHudDrawNumber()
{
    if (!g_curTargetIsBackbuffer || !g_backbuffer || CurrentUiPass() != 1 || CurrentHudEye() < 0)
        return 0;
    const int replay = CurrentHudReplay();
    if (replay != t_nativeHudReplay)
    {
        t_nativeHudReplay = replay;
        t_nativeHudDraws = 0;
    }
    return ++t_nativeHudDraws;
}

void PlaceNativeHud(ID3D11DeviceContext* ctx)
{
    D3D11_TEXTURE2D_DESC bb{};
    g_backbuffer->GetDesc(&bb);
    UINT texW = 0, texH = 0, texFormat = 0;
    if (GetDrawTexture(ctx, &texW, &texH, &texFormat) && texW * 2 >= bb.Width && texH >= bb.Height)
    {
        // A frame-sized source: a world composite, not HUD. Logged (2026-11-08, the menu looked different per eye):
        // such a draw keeps the pass's own viewport, so it may cover both halves.
        static int skippedLines = 0; // render thread
        if (skippedLines < 12)
        {
            ++skippedLines;
            D3D11_VIEWPORT current{};
            UINT count = 1;
            ctx->RSGetViewports(&count, &current);
            MOHW_LOG(kLogFile,
                     "native two-view: HUD replay draw %d for the %s half samples a %ux%u texture: not placed (taken for "
                     "a world composite), drawn at the pass's viewport x=%.1f w=%.1f of %u (%s)",
                     t_nativeHudDraws, CurrentHudEye() == 1 ? "right" : "left", texW, texH, current.TopLeftX,
                     current.Width, bb.Width, Is2DFrame() ? "2D frame" : "over the world");
        }
        return;
    }
    const int eye = CurrentHudEye();
    const UINT halfWidth = bb.Width / 2;
    const bool whole2D = Is2DFrame();
    float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // placement off or no headset data: the flat HUD fills the half
    // The aspect a whole 2D frame keeps is the full frame's: the game lays its UI out for the whole backbuffer, not a
    // half (with the half's, menus came out half as wide as they should).
    const bool placed = ComputeHudRectForEye(eye, whole2D ? GetMenuScreenScale() : GetHudScale(), whole2D, bb.Width,
                                             bb.Height, rect);
    if (!placed)
    {
        rect[0] = 0.0f;
        rect[1] = 0.0f;
        rect[2] = 1.0f;
        rect[3] = 1.0f;
    }
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(eye * halfWidth) + rect[0] * static_cast<float>(halfWidth);
    vp.Width = rect[2] * static_cast<float>(halfWidth);
    vp.TopLeftY = rect[1] * static_cast<float>(bb.Height);
    vp.Height = rect[3] * static_cast<float>(bb.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    if (placed && !whole2D)
        g_hudPlacedMs.store(GetTickCount64(), std::memory_order_relaxed);

    static int lines = 0; // render thread
    if (lines < 8 && t_nativeHudDraws == 1)
    {
        ++lines;
        MOHW_LOG(kLogFile,
                 "native two-view: HUD replay into the %s half: viewport x=%.1f y=%.1f w=%.1f h=%.1f of %ux%u (%s, %s)",
                 eye == 1 ? "right" : "left", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, bb.Width, bb.Height,
                 whole2D ? "whole 2D frame" : "HUD over the world", placed ? "placed" : "placement off: the half");
    }
}

bool ShouldSkipAsReticle(ID3D11DeviceContext* ctx, int nativeDraw)
{
    // Only while the controller aims (the weapon drive): with it off, shots follow the game's own aim again and the
    // reticle is needed to see it. And not while paused: the in-game menu uses the same draw numbers.
    // Nor in a scripted stretch: the gun is the game's then, and shots go where the game aims.
    // And on the RHIB's Mk19 aimed by hand: the head turns apart from the gun, so a head-locked crosshair isn't its aim.
    const bool boatGun = GetBoatSeat() == BoatSeat::Gunner && GetBoatGunAim();
    if (!g_curTargetIsBackbuffer || !GetHideReticle() ||
        (!boatGun && (!GetWeaponDriveEnabled() || GetPlayerAimDriveWeight() < 0.5f)))
        return false;
    int next = g_backbufferDrawCount + 1; // the number this draw will get
    int first = kReticleFirstDraw, last = kReticleLastDraw;
    if (nativeDraw > 0)
    {
        // In a per-half HUD replay: the world composite isn't one of its draws, so the reticle starts at 1 (7 kept as
        // the end, in case it ever is). The texture check below decides.
        next = nativeDraw;
        first = 1;
    }
    if (next < first || next > last || (boatGun ? IsBoatPaused() : !IsPlayerSkeletonLoaded() || IsSimulationPaused()))
        return false;
    // The reticle's own texture: when it isn't drawn (live: a pass starting with the 256x64 weapon display), these
    // draw numbers are other HUD elements and must stay.
    UINT w = 0, h = 0, format = 0;
    return GetDrawTexture(ctx, &w, &h, &format) && w == 128 && h == 64 && format == kFormatBC3;
}

// ---- Final-pass capture (2026-09-29) -------------------------------------------------------------------------------
// A capture of every final-pass draw (both eyes) every kPassCaptureEveryMs in gameplay, to mohwvr_hudlayers.log:
// number, count, shaders, first texture and size, viewport. (Built to find a signature for the in-world markers'
// draws; they are now projected into the HUD rectangle at the source instead, hooks/marker_projection_hook.h.)
constexpr const char* kHudLayerLog = "mohwvr_hudlayers.log";
constexpr unsigned long long kPassCaptureEveryMs = 15000;
constexpr int kPassCaptureMaxPasses = 20; // captures, each kPassCaptureDraws draws
int g_passesCaptured = 0;

// Called from Hook_OM on every backbuffer bind. The game binds the backbuffer several times a frame and the final 2D
// pass comes in a later bind (live: a per-bind capture started twice a frame and logged no draws), so a capture runs for
// the next kPassCaptureDraws backbuffer draws across however many binds, marking each bind with the eye it's for.
constexpr int kPassCaptureDraws = 300;
int g_captureDrawsLeft = 0; // render thread

void MaybeStartPassCapture()
{
    static unsigned long long nextCaptureMs = 0;
    if (g_captureDrawsLeft > 0)
    {
        MOHW_LOG(kHudLayerLog, " BIND (eye %d)", IsRightEyeActive() ? 1 : 0);
        g_capturingPass = true;
        return;
    }
    g_capturingPass = false;
    // Only with the log files on (sdk/logging.h): the capture is nothing but log lines, thousands of them at a level's
    // start in two-view (every backbuffer bind).
    if (g_passesCaptured >= kPassCaptureMaxPasses || !IsPlayerSkeletonLoaded() || !LogsEnabled())
        return;
    unsigned long long now = GetTickCount64();
    if (now < nextCaptureMs)
        return;
    nextCaptureMs = now + kPassCaptureEveryMs;
    ++g_passesCaptured;
    g_captureDrawsLeft = kPassCaptureDraws;
    g_capturingPass = true;
    MOHW_LOG(kHudLayerLog, "CAPTURE %d: next %d backbuffer draws; HUD placement %s", g_passesCaptured,
              kPassCaptureDraws, GetHudPlacementEnabled() ? "on" : "off");
    MOHW_LOG(kHudLayerLog, " BIND (eye %d)", IsRightEyeActive() ? 1 : 0);
}

void LogPassDraw(ID3D11DeviceContext* ctx, UINT count, bool indexed, bool skipped)
{
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 1, &srv);
    UINT texW = 0, texH = 0, texFormat = 0;
    void* texPtr = nullptr;
    if (srv)
    {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        if (res)
        {
            texPtr = res;
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
            {
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                texW = d.Width;
                texH = d.Height;
                texFormat = d.Format;
                tex->Release();
            }
            res->Release();
        }
        srv->Release();
    }
    D3D11_VIEWPORT vp{};
    UINT vpCount = 1;
    ctx->RSGetViewports(&vpCount, &vp);
    MOHW_LOG(kHudLayerLog, "  draw %d %s count=%u VS=%p PS=%p tex=%p %ux%u fmt=%u viewport=(%.0f,%.0f %.0fx%.0f)%s",
              g_backbufferDrawCount + 1, indexed ? "indexed" : "plain", count, static_cast<void*>(vs),
              static_cast<void*>(ps), texPtr, texW, texH, texFormat, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
              skipped ? " (skipped)" : "");
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
}

// Live debugging (DebugHideDraw): one draw number of the final pass is skipped, in gameplay only, to see which element
// it is. Still counted, like the reticle skip. Not while paused: everything hidden comes back for the in-game menu.
bool IsDebugHiddenDraw()
{
    int hide = GetDebugHideDraw();
    return hide > 0 && g_curTargetIsBackbuffer && g_backbufferDrawCount + 1 == hide && !Is2DFrame() &&
           !IsSimulationPaused();
}

// Before each final-pass draw: the capture. (The in-world markers used to get the full frame back here, by draw
// number or by texture; replaced 2026-10-16 by projecting them into the HUD rectangle at the source,
// hooks/marker_projection_hook.h, so the whole UI keeps one rectangle.)
void BeforeBackbufferDraw(ID3D11DeviceContext* ctx, UINT count, bool indexed, bool skipped)
{
    if (!g_curTargetIsBackbuffer || !g_backbuffer)
        return;
    if (g_capturingPass && g_captureDrawsLeft > 0)
    {
        --g_captureDrawsLeft;
        LogPassDraw(ctx, count, indexed, skipped);
    }
}

void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b)
{
    const int nativeDraw = NativeHudDrawNumber();
    if (nativeDraw > 0)
        PlaceNativeHud(c);
    else
    {
        MaybePlaceWhole2DFrame(c);
        KeepPlacedViewport(c);
    }
    bool skip = ShouldSkipAsReticle(c, nativeDraw) || IsDebugHiddenDraw();
    BeforeBackbufferDraw(c, n, true, skip);
    if (!skip && !DrawIntoBothHalves(c, [&] { g_origDrawIndexed(c, n, s, b); }))
        g_origDrawIndexed(c, n, s, b);
    CountDraw(c, n);
    UiPassNoteDraw();
}
void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* c, UINT n, UINT s)
{
    const int nativeDraw = NativeHudDrawNumber();
    if (nativeDraw > 0)
        PlaceNativeHud(c);
    else
    {
        MaybePlaceWhole2DFrame(c);
        KeepPlacedViewport(c);
    }
    bool skip = ShouldSkipAsReticle(c, nativeDraw) || IsDebugHiddenDraw();
    BeforeBackbufferDraw(c, n, false, skip);
    if (!skip && !DrawIntoBothHalves(c, [&] { g_origDraw(c, n, s); }))
        g_origDraw(c, n, s);
    CountDraw(c, n);
    UiPassNoteDraw();
}
void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, INT b, UINT si)
{
    KeepPlacedViewport(c);
    if (!DrawIntoBothHalves(c, [&] { g_origDrawIndexedInstanced(c, n, i, s, b, si); }))
        g_origDrawIndexedInstanced(c, n, i, s, b, si);
    CountDraw(c, n * i);
}
void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, UINT si)
{
    KeepPlacedViewport(c);
    if (!DrawIntoBothHalves(c, [&] { g_origDrawInstanced(c, n, i, s, si); }))
        g_origDrawInstanced(c, n, i, s, si);
    CountDraw(c, n * i);
}
void STDMETHODCALLTYPE Hook_Clear(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, const FLOAT color[4])
{
    if (OnTraceThread())
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        FlushCurrent();
        char buf[160];
        snprintf(buf, sizeof(buf), "  CLEAR RTV color=(%.2f,%.2f,%.2f,%.2f)", color[0], color[1], color[2], color[3]);
        g_lines.push_back(buf);
    }
    g_origClear(c, rtv, color);
}
void STDMETHODCALLTYPE Hook_Copy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src)
{
    if (OnTraceThread())
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        FlushCurrent();
        char buf[160];
        snprintf(buf, sizeof(buf), "  COPYRESOURCE dst=%p src=%p%s", static_cast<void*>(dst), static_cast<void*>(src),
                 (dst == g_backbuffer) ? " (dst is BACKBUFFER)" : ((src == g_backbuffer) ? " (src is BACKBUFFER)" : ""));
        g_lines.push_back(buf);
    }
    g_origCopy(c, dst, src);
}

bool HookOne(void** vtable, int idx, void* detour, void** original, const char* name)
{
    void* target = vtable[idx];
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(%s @ %p) FAILED: %s", name, target, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook(%s) FAILED: %s", name, MH_StatusToString(s));
        return false;
    }
    return true;
}

void InstallOnce(ID3D11Device* device)
{
    if (!kEnabled)
        return;
    if (g_installed)
        return;
    g_installed = true; // one attempt only
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx)
        return;
    void** vtable = *reinterpret_cast<void***>(ctx);
    MH_Initialize();
    // LEAN footprint (2026-09-22, see kEnabled/kFullTraceHooksEnabled's declaration comments): OMSetRenderTargets,
    // Draw and DrawIndexed. DrawIndexed used to be counted through constantbuffer_hook.cpp's own hook (via
    // DrawTraceNoteDraw); that hook was retired 2026-09-28, so it's hooked here now -- a bool check and the original
    // per call, far cheaper than the retired hook. The remaining four (instanced draws, clears, copies) only matter for
    // the rich Scroll-Lock trace log and are gated separately.
    bool ok = true;
    ok &= HookOne(vtable, kIdxOMSetRenderTargets, reinterpret_cast<void*>(&Hook_OM), reinterpret_cast<void**>(&g_origOM), "OMSetRenderTargets");
    ok &= HookOne(vtable, kIdxDraw, reinterpret_cast<void*>(&Hook_Draw), reinterpret_cast<void**>(&g_origDraw), "Draw");
    ok &= HookOne(vtable, kIdxDrawIndexed, reinterpret_cast<void*>(&Hook_DrawIndexed),
                  reinterpret_cast<void**>(&g_origDrawIndexed), "DrawIndexed");
    if (kFullTraceHooksEnabled)
    {
        ok &= HookOne(vtable, kIdxDrawIndexedInstanced, reinterpret_cast<void*>(&Hook_DrawIndexedInstanced),
                      reinterpret_cast<void**>(&g_origDrawIndexedInstanced), "DrawIndexedInstanced");
        ok &= HookOne(vtable, kIdxDrawInstanced, reinterpret_cast<void*>(&Hook_DrawInstanced), reinterpret_cast<void**>(&g_origDrawInstanced),
                      "DrawInstanced");
        ok &= HookOne(vtable, kIdxClearRenderTargetView, reinterpret_cast<void*>(&Hook_Clear), reinterpret_cast<void**>(&g_origClear),
                      "ClearRenderTargetView");
        ok &= HookOne(vtable, kIdxCopyResource, reinterpret_cast<void*>(&Hook_Copy), reinterpret_cast<void**>(&g_origCopy), "CopyResource");
    }
    ctx->Release();
    MOHW_LOG(kLogFile, "draw trace hooks %s (lean=%s, full-trace=%s; press Scroll Lock to trace the next frame)", ok ? "installed" : "PARTIALLY FAILED",
              "OM+Draw", kFullTraceHooksEnabled ? "on" : "off");
}

// Saves g_uiSnapshotTex to a BMP next to the exe, same format helper shape as companion/main.cpp's DumpSourceIfRequested
// (staging copy -> Map -> write BGRA/RGBA rows). Game-side equivalent, since this texture never leaves this process.
void DumpUiSnapshotBmp(ID3D11DeviceContext* ctx, ID3D11Device* device)
{
    if (!LogsEnabled())
        return; // a diagnostic file: only with the log files on (sdk/logging.h)
    if (!g_uiSnapshotTex)
    {
        MOHW_LOG(kLogFile, "DumpUiSnapshotBmp: no snapshot captured yet");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    g_uiSnapshotTex->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) || !staging)
    {
        MOHW_LOG(kLogFile, "DumpUiSnapshotBmp: staging CreateTexture2D FAILED");
        return;
    }
    ctx->CopyResource(staging, g_uiSnapshotTex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
    {
        char path[MAX_PATH] = {};
        DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        while (n > 0 && path[n - 1] != '\\')
            --n;
        path[n] = 0;
        strcat_s(path, "mohwvr_ui_snapshot_dump.bmp");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") == 0 && f)
        {
            const UINT w = desc.Width, h = desc.Height;
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            unsigned imgSize = w * h * 4;
            unsigned char hdr[54] = {'B', 'M'};
            unsigned fileSize = 54 + imgSize, off = 54, dib = 40;
            int iw = static_cast<int>(w), ih = -static_cast<int>(h); // top-down
            unsigned short planes = 1, bpp = 32;
            memcpy(hdr + 2, &fileSize, 4);
            memcpy(hdr + 10, &off, 4);
            memcpy(hdr + 14, &dib, 4);
            memcpy(hdr + 18, &iw, 4);
            memcpy(hdr + 22, &ih, 4);
            memcpy(hdr + 26, &planes, 2);
            memcpy(hdr + 28, &bpp, 2);
            memcpy(hdr + 34, &imgSize, 4);
            fwrite(hdr, 1, 54, f);
            for (UINT y = 0; y < h; ++y)
            {
                const unsigned char* row = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                for (UINT x = 0; x < w; ++x)
                {
                    const unsigned char* p = row + x * 4;
                    unsigned char px[4] = {bgra ? p[0] : p[2], p[1], bgra ? p[2] : p[0], 255};
                    fwrite(px, 1, 4, f);
                }
            }
            fclose(f);
            MOHW_LOG(kLogFile, "UI snapshot dump saved: %ux%u format=%d -> %s", w, h, static_cast<int>(desc.Format), path);
        }
        ctx->Unmap(staging, 0);
    }
    staging->Release();
}

} // namespace

void DrawTraceNoteDraw(ID3D11DeviceContext* context, unsigned indexCount)
{
    CountDraw(context, indexCount);
}

bool FinalPassStartedThisFrame()
{
    return g_finalPassStarted.load(std::memory_order_relaxed);
}

bool GetHudRectForMarkers(float rect[4])
{
    // Native two-view: the markers are drawn into both halves from one projection: GetNativeHudRectsForMarkers.
    if (IsNativeStereoActive())
        return false;
    unsigned long long placed = g_hudPlacedMs.load(std::memory_order_relaxed);
    if (placed == 0 || GetTickCount64() - placed > 250) // the HUD isn't being placed (a 2D frame, placement off)
        return false;
    return ComputeHudRect(GetHudScale(), false, 0, 0, rect);
}

bool GetNativeHudRectsForMarkers(float rects[2][4])
{
    if (!IsNativeStereoActive())
        return false;
    // PlaceNativeHud's rectangles: the placed one while placement is on, else the whole half.
    for (int eye = 0; eye < 2; ++eye)
        if (!ComputeHudRectForEye(eye, GetHudScale(), false, 0, 0, rects[eye]))
        {
            rects[eye][0] = 0.0f;
            rects[eye][1] = 0.0f;
            rects[eye][2] = 1.0f;
            rects[eye][3] = 1.0f;
        }
    return true;
}

bool IsWhole2DFrameNow(unsigned long long* forMs)
{
    const unsigned long long since = g_whole2DSinceMs.load(std::memory_order_relaxed);
    if (forMs)
        *forMs = since == 0 ? 0 : GetTickCount64() - since;
    return since != 0 && g_whole2DNow.load(std::memory_order_relaxed);
}

ID3D11Texture2D* GetUiSnapshotTexture()
{
    return g_uiSnapshotTex;
}

void DrawTraceOnPresent(ID3D11Device* device, ID3D11Texture2D* backbuffer)
{
    if (!kEnabled)
        return; // A/B test -- see kEnabled's declaration comment
    InstallOnce(device);
    g_backbuffer = backbuffer;
    g_finalPassStarted.store(false, std::memory_order_relaxed); // the next frame starts

    // POLLING TABLE DISABLED (2026-09-25), PERFORMANCE TEST -- see present_hook.cpp's identical comment. This
    // disables the Scroll Lock arm-a-trace trigger; g_armed can be hardcoded to true directly if this capture is
    // needed again before it's restored. The rest of this function's per-frame work (g_tracing dump-if-armed
    // below) is untouched.
    // static bool wasDown = false;
    // bool down = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
    // if (down && !wasDown)
    // {
    //     g_armed.store(true, std::memory_order_relaxed);
    //     // Same keypress dumps THIS (about-to-end) frame's UI snapshot -- it was captured during this frame's draws,
    //     // which already happened by the time Present runs.
    //     ID3D11DeviceContext* ctx = nullptr;
    //     device->GetImmediateContext(&ctx);
    //     if (ctx)
    //     {
    //         DumpUiSnapshotBmp(ctx, device);
    //         ctx->Release();
    //     }
    // }
    // wasDown = down;

    if (g_tracing.load(std::memory_order_relaxed))
    {
        // This Present ends the traced frame -- dump it.
        g_tracing.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_mutex);
        FlushCurrent();
        // All traces in one file (2026-11-11), each headed with the state it was taken in, so a moment the user names
        // can be matched by its time.
        const char* fileName = "mohwvr_frametrace.log";
        MOHW_LOG(fileName, "==== traced frame (%zu events): two-view %s, soldier's aim %s, first-person body %s ====",
                 g_lines.size(), IsNativeStereoActive() ? "on" : "off", IsPlayerAimActive() ? "running" : "not running",
                 IsPlayerSkeletonLoaded() ? "drawn" : "not drawn");
        for (const std::string& l : g_lines)
            MOHW_LOG(fileName, "%s", l.c_str());
        MOHW_LOG(fileName, "==== end of traced frame ====");
        MOHW_LOG(kLogFile, "trace saved to %s (%zu events)", fileName, g_lines.size());
        g_lines.clear();
        g_curTargetDesc.clear();
        return;
    }
    // (2026-11-11: automatic traces, a frame every 4 s in two-view 3D frames, found the depth-of-field passes that
    // doubled each eye's image -- hooks/native_stereo.cpp. Removed; arm g_armed to take a trace again.)
    if (g_armed.exchange(false, std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lines.clear();
        g_curTargetDesc.clear();
        g_curDraws = g_curVerts = 0;
        g_traceThread = GetCurrentThreadId();
        g_tracing.store(true, std::memory_order_relaxed); // the frame that starts after this Present is the traced one
    }
}

} // namespace mohw
