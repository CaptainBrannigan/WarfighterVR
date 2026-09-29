#include "draw_trace_diag.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"
#include "alternating_eye.h"
#include "companion_bridge.h"
#include "camera_matrix_test_hook.h"
#include "projection_aspect_hook.h"

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
    if (g_uiSnapshotTex && g_uiSnapshotW == bbDesc.Width && g_uiSnapshotH == bbDesc.Height && g_uiSnapshotFmt == bbDesc.Format)
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
// HUD's IPD follows the world's). Vertically the rectangle is centred on the image (both eyes share the vertical
// frustum).
// A frame whose whole final pass is 2D: a pre-rendered movie, a menu or a loading screen. In gameplay draw 1 of the
// final pass is the 3D world composite (kept full-frame, its IPD comes from the camera) and only the draws after it are
// placed; in a 2D frame draw 1 is itself 2D (the movie frame, the menu background), so it's placed too and the whole
// frame becomes one head-locked screen whose eye copies fuse. Told apart by whether the world camera got the true
// per-eye frustum recently (hooks/projection_aspect_hook.cpp).
bool Is2DFrame()
{
    constexpr unsigned kWorldRecentMs = 200; // rate window, see WorldRenderedRecently
    return !WorldRenderedRecently(kWorldRecentMs);
}

void PlaceHudForThisEye(ID3D11DeviceContext* ctx, UINT width, UINT height, float s, bool keepAspect);

// Before the first draw of the final pass in a 2D frame: place it (and, the viewport being state, everything after).
void MaybePlaceWhole2DFrame(ID3D11DeviceContext* ctx)
{
    if (!g_curTargetIsBackbuffer || g_backbufferDrawCount != 0 || !g_backbuffer || !Is2DFrame())
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
void PlaceHudForThisEye(ID3D11DeviceContext* ctx, UINT width, UINT height, float s, bool keepAspect)
{
    if (!GetHudPlacementEnabled())
        return;
    mohwvr::ipc::HmdViewBlock hmd{};
    if (!GetHmdView(&hmd) || hmd.viewMode != mohwvr::ipc::kViewModeTrueFrustum)
        return;
    int eye = IsRightEyeActive() ? 1 : 0;
    float tL = tanf(hmd.angleLeft[eye]), tR = tanf(hmd.angleRight[eye]);
    float tU = tanf(hmd.angleUp[eye]), tD = tanf(hmd.angleDown[eye]);
    float centreX = (tL + tR) * 0.5f, halfW = (tR - tL) * 0.5f, halfH = (tU - tD) * 0.5f;
    if (halfW < 0.05f || halfH < 0.05f)
        return;
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
    float offsetXDeg = 0.0f, offsetYDeg = 0.0f;
    GetHudOffsetDeg(&offsetXDeg, &offsetYDeg);
    float eyeX = (eye == 1 ? 1.0f : -1.0f) * fabsf(GetActiveEyeOffsetAlongRow0()) * ipdScale; // right eye sits at +x
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
    float ndcUpShift = tanf(offsetYDeg * kDegToRad) / halfH;
    float heightFraction = rectHalfH / halfH;

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = (ndcLeft + 1.0f) * 0.5f * static_cast<float>(width);
    vp.Width = (ndcRight - ndcLeft) * 0.5f * static_cast<float>(width);
    vp.TopLeftY = (1.0f - heightFraction - ndcUpShift) * 0.5f * static_cast<float>(height);
    vp.Height = heightFraction * static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    static int lines = 0;
    if (lines < 6)
    {
        ++lines;
        MOHW_LOG(kLogFile,
                  "HUD placed for eye %d: viewport x=%.1f y=%.1f w=%.1f h=%.1f of %ux%u (scale %.2f, depth %.2f m, IPD x%.2f, "
                  "offset %.1f/%.1f deg, tan L/R %.3f/%.3f)",
                  eye, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, width, height, s, depth, ipdScale, offsetXDeg,
                  offsetYDeg, tL, tR);
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
    if (g_curTargetIsBackbuffer)
        MaybeStartPassCapture();
    else
        g_capturingPass = false;
    g_origOM(ctx, n, rtvs, dsv);
}

void CountDraw(ID3D11DeviceContext* ctx, UINT verts)
{
    if (OnTraceThread())
    {
        std::lock_guard<std::mutex> lock(g_mutex);
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

bool ShouldSkipAsReticle()
{
    // Only while the controller aims (the weapon drive): with it off, shots follow the game's own aim again and the
    // reticle is needed to see it.
    if (!g_curTargetIsBackbuffer || !GetHideReticle() || !GetWeaponDriveEnabled())
        return false;
    int next = g_backbufferDrawCount + 1; // the number this draw will get
    return next >= kReticleFirstDraw && next <= kReticleLastDraw && IsPlayerSkeletonLoaded();
}

// ---- In-world objective markers (2026-09-29) -----------------------------------------------------------------------
// The markers are 2D draws in the final pass like the HUD, so they get the HUD rectangle (scaled, shifted to HUD depth)
// and leave their targets. Two tools to find their treatment:
//  - FullFrameFromDraw (dashboard, 0 = off): from that draw number on, the full frame is restored. If a marker then
//    sits on its target at its depth, the game already places markers per eye and they only need exempting.
//  - A capture of every final-pass draw (both eyes) every kPassCaptureEveryMs in gameplay, to mohwvr_hudlayers.log:
//    number, count, shaders, first texture and size -- to find a signature for marker draws, as the numbering shifts
//    with HUD content.
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
    if (g_passesCaptured >= kPassCaptureMaxPasses || !IsPlayerSkeletonLoaded())
        return;
    unsigned long long now = GetTickCount64();
    if (now < nextCaptureMs)
        return;
    nextCaptureMs = now + kPassCaptureEveryMs;
    ++g_passesCaptured;
    g_captureDrawsLeft = kPassCaptureDraws;
    g_capturingPass = true;
    MOHW_LOG(kHudLayerLog, "CAPTURE %d: next %d backbuffer draws; HUD placement %s, full frame from draw %d",
              g_passesCaptured, kPassCaptureDraws, GetHudPlacementEnabled() ? "on" : "off", GetFullFrameFromDraw());
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
              skipped ? " (skipped: reticle)" : "");
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
}

// Before each final-pass draw: the full-frame test, then the capture.
void BeforeBackbufferDraw(ID3D11DeviceContext* ctx, UINT count, bool indexed, bool skipped)
{
    if (!g_curTargetIsBackbuffer || !g_backbuffer)
        return;
    int from = GetFullFrameFromDraw();
    if (from > 0 && g_backbufferDrawCount + 1 == from && !Is2DFrame())
    {
        D3D11_TEXTURE2D_DESC bbDesc{};
        g_backbuffer->GetDesc(&bbDesc);
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(bbDesc.Width);
        vp.Height = static_cast<float>(bbDesc.Height);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
    }
    if (g_capturingPass && g_captureDrawsLeft > 0)
    {
        --g_captureDrawsLeft;
        LogPassDraw(ctx, count, indexed, skipped);
    }
}

void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b)
{
    MaybePlaceWhole2DFrame(c);
    bool skip = ShouldSkipAsReticle();
    BeforeBackbufferDraw(c, n, true, skip);
    if (!skip)
        g_origDrawIndexed(c, n, s, b);
    CountDraw(c, n);
}
void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* c, UINT n, UINT s)
{
    MaybePlaceWhole2DFrame(c);
    bool skip = ShouldSkipAsReticle();
    BeforeBackbufferDraw(c, n, false, skip);
    if (!skip)
        g_origDraw(c, n, s);
    CountDraw(c, n);
}
void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, INT b, UINT si)
{
    g_origDrawIndexedInstanced(c, n, i, s, b, si);
    CountDraw(c, n * i);
}
void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, UINT si)
{
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
        // A NEW file per trace (date-time in the name) so several traces (level, pause menu, ...) can be kept and compared.
        SYSTEMTIME st;
        GetLocalTime(&st);
        char fileName[96];
        snprintf(fileName, sizeof(fileName), "mohwvr_drawtrace_%04d%02d%02d_%02d%02d%02d.log", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
        MOHW_LOG(fileName, "==== traced frame (%zu events) ====", g_lines.size());
        for (const std::string& l : g_lines)
            MOHW_LOG(fileName, "%s", l.c_str());
        MOHW_LOG(fileName, "==== end of traced frame ====");
        MOHW_LOG(kLogFile, "trace saved to %s (%zu events)", fileName, g_lines.size());
        g_lines.clear();
        g_curTargetDesc.clear();
        return;
    }
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
