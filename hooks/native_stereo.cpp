#include "native_stereo.h"

#include "alternating_eye.h"
#include "companion_bridge.h"
#include "draw_trace_diag.h"
#include "perf_probe.h"
#include "scope_view.h"
#include "../openvr_direct/openvr_direct.h"
#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_nativestereo.log";
constexpr unsigned long long kRecentMs = 250;
constexpr float kMaxFovYRad = 3.05f; // hooks/eye_matched_fov.cpp's clamp: a fovY near pi breaks the projection

using BuildViewsFn = void(__fastcall*)(void* self, void* edx, void* params, uint32_t* count, unsigned char* views);
BuildViewsFn g_originalBuildViews = nullptr;
void* g_buildViewsAddress = nullptr;

// WHERE THE VIEW LIST'S FIELDS ARE (2026-11-18, the user: "implement our in engine render to the driving exe"): MOHW.exe's
// layout (sdk/mohw_offsets.h VIEWDESC_* / RENDERVIEW_*) or the driving missions' game\MOHWD.exe's, an older build of
// the same builder with a bigger view and a RenderView four floats longer before the aspect (MOHWD_*). A field at 0
// isn't used in that build.
struct ViewLayout
{
    const char* exe;
    uint32_t descSize;
    uint32_t viewport;
    uint32_t renderView, renderViewCopy;
    uint32_t renderViewPrev; // MOHWD.exe: never written by its builder (a stack array), so left alone
    uint32_t rawCamera;      // the first-person batches' camera (MOHW.exe), the same place in MOHWD.exe (unconfirmed)
    bool rawCameraConfirmed; // false: written only while its position is the view's own
    uint32_t fovY, aspect, viewportOffset, dirty; // RenderView fields
};
constexpr ViewLayout kMohwLayout{"MOHW.exe", VIEWDESC_SIZE, VIEWDESC_VIEWPORT, VIEWDESC_RENDERVIEW,
                                 VIEWDESC_RENDERVIEW_COPY, VIEWDESC_RENDERVIEW_PREV, VIEWDESC_VIEWMODEL_CAMERA, true,
                                 RENDERVIEW_FOVY, RENDERVIEW_ASPECT, RENDERVIEW_VIEWPORTOFFSET, RENDERVIEW_DIRTY};
constexpr ViewLayout kMohwdLayout{"MOHWD.exe", MOHWD_VIEWDESC_SIZE, MOHWD_VIEWDESC_VIEWPORT, MOHWD_VIEWDESC_RENDERVIEW,
                                  MOHWD_VIEWDESC_RENDERVIEW_COPY, 0, MOHWD_VIEWDESC_RAW_CAMERA, false, RENDERVIEW_FOVY,
                                  MOHWD_RENDERVIEW_ASPECT, MOHWD_RENDERVIEW_VIEWPORTOFFSET, MOHWD_RENDERVIEW_DIRTY};
const ViewLayout* g_layout = &kMohwLayout;

// MOHWD.exe's static addresses in this process (g_baseDelta is MOHW.exe's, 0 in the driving executable).
uintptr_t InThisExe(uintptr_t staticAddress)
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) - 0x400000u + staticAddress;
}

// A raw camera this far from the view's own position isn't the view's camera: MOHWD.exe's is left alone then.
constexpr float kRawCameraMatchMetres = 0.5f;

std::atomic<unsigned long long> g_twoViewsMs{0};

// The eye cameras written into the views' raw camera (VIEWDESC_VIEWMODEL_CAMERA) for the last few frames: a frame's
// render jobs can still be running while the next frame's views are built. And this frame's eye projections (fovY,
// aspect, viewport offset x, y), for the first-person batches' camera.
constexpr int kEyeCameraFrames = 4;
constexpr float kEyeCameraMatchMetres = 0.005f;
std::mutex g_eyeMutex;
float g_eyeCameraPos[kEyeCameraFrames][2][3] = {};
unsigned g_eyeCameraFrame[kEyeCameraFrames] = {}; // which built frame each entry is (g_framesBuilt at the time)
unsigned g_framesBuilt = 0;
int g_eyeCameraNext = 0;
int g_eyeCameraCount = 0;
float g_eyeProjection[2][4] = {};
bool g_haveEyeProjection = false;
// The last two-view frame's head centre, its right axis, and the world cameras written into the two views (RenderView
// +0x40), for GetNativeEyeCameras (diagnostics: where the engine really draws each eye from).
float g_centrePos[3] = {};
float g_centreRight[3] = {};
float g_writtenEye[2][3] = {};
bool g_haveWrittenEyes = false;
// The same for the last few frames (render jobs lag the build): [frame][0 left, 1 right, 2 head centre][xyz], for
// NativeStereoClassifyWorldCamera.
float g_worldRing[kEyeCameraFrames][3][3] = {};
int g_worldRingNext = 0;
int g_worldRingCount = 0;

// The game's own SplitScreenTestViewCount, before this module first changed it.
bool g_haveGameViewCount = false;
uint32_t g_gameViewCount = 0;
bool g_viewCountChanged = false;

// WHOLE 2D FRAMES GET ONE VIEW (2026-11-08, first headset test): a movie, a menu, a loading screen or a cutscene before
// the player has control (hooks/draw_trace_diag.h IsWhole2DFrameNow) runs with the game's own view count. Split between
// the halves, the intro movie showed a different half of itself to each eye (drawn across the whole double-wide frame),
// and the main menu's background filled each eye while its items floated in front; the floating screen those get in
// one-view mode can't be made from two eye views. The world renderer resizes its targets when the views' size changes
// (FUN_0101CFA0), so the count can change at run time; each switch waits for the new state to hold, and comes at most
// every 2 s, so a stray frame can't flip it back and forth. Present thread.
constexpr unsigned long long kOneViewAfterMs = 300;
constexpr unsigned long long kTwoViewsAfterMs = 300;
constexpr unsigned long long kMinSwitchGapMs = 2000;
std::atomic<bool> g_wantTwoViews{false};
unsigned long long g_lastSwitchMs = 0;

bool ReadU32(uintptr_t address, uint32_t* out)
{
    __try
    {
        *out = *reinterpret_cast<const uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteU32(uintptr_t address, uint32_t value)
{
    __try
    {
        *reinterpret_cast<uint32_t*>(address) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadBytes(const void* from, void* to, size_t n)
{
    __try
    {
        memcpy(to, from, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteBytes(void* to, const void* from, size_t n)
{
    __try
    {
        memcpy(to, from, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// GameRenderSettings::SplitScreenTestViewCount's address, 0 while there's no GameRenderer. MOHWD.exe: its renderer's
// vtable is checked first (its settings field is found through a getter, not a reflected record).
uintptr_t ViewCountAddress()
{
    uint32_t renderer = 0, settings = 0, vtable = 0;
    if (g_drivingExe)
    {
        if (!ReadU32(InThisExe(MOHWD_GAMERENDERER), &renderer) || !renderer || !ReadU32(renderer, &vtable) ||
            vtable != static_cast<uint32_t>(InThisExe(MOHWD_GAMERENDERER_VTABLE)) ||
            !ReadU32(renderer + MOHWD_GAMERENDERER_SETTINGS, &settings) || !settings)
            return 0;
        return settings + MOHWD_GAMERENDERSETTINGS_VIEWCOUNT;
    }
    if (!ReadU32(g_baseDelta + OFFSET_GAMERENDERER, &renderer) || !renderer ||
        !ReadU32(renderer + GAMERENDERER_SETTINGS, &settings) || !settings)
        return 0;
    return settings + GAMERENDERSETTINGS_SPLITSCREENTESTVIEWCOUNT;
}

// One eye's frustum as the projection builder takes it (hooks/eye_matched_fov.cpp's TRUE PER-EYE FRUSTUM, and
// projection_aspect_hook.h for the parameterization): vertical FOV, aspect, viewport offset.
struct EyeProjection
{
    float fovY, aspect, offX, offY;
};

bool EyeFrustum(const mohwvr::ipc::HmdViewBlock& hmd, int eye, EyeProjection* out)
{
    float k = GetFrustumScale();
    if (!(k > 0.2f && k < 4.0f))
        k = 1.0f;
    const float tL = tanf(hmd.angleLeft[eye]) * k, tR = tanf(hmd.angleRight[eye]) * k;
    const float tU = tanf(hmd.angleUp[eye]) * k, tD = tanf(hmd.angleDown[eye]) * k;
    const float T = (tU - tD) * 0.5f; // half-height tan
    const float width = tR - tL;
    if (!(T > 0.05f && width > 0.05f))
        return false;
    out->aspect = width / (2.0f * T);
    out->offX = ((tL + tR) * 0.5f) / (out->aspect * T * 2.0f);
    out->offY = ((tU + tD) * 0.5f) / (T * 2.0f);
    out->fovY = 2.0f * atanf(T);
    if (out->fovY > kMaxFovYRad)
        out->fovY = kMaxFovYRad;
    return true;
}

// One RenderView made an eye's: its camera moved by `offset` along its own row 0 (right), the eye's frustum, the
// matrices marked for a rebuild. `transform` is the 16 floats to start from (the head-centre camera).
bool MakeEye(unsigned char* renderView, const float transform[16], float offset, const EyeProjection* projection)
{
    float m[16];
    memcpy(m, transform, sizeof(m));
    for (int i = 0; i < 3; ++i)
        m[12 + i] += m[i] * offset;
    if (!WriteBytes(renderView, m, sizeof(m)))
        return false;
    if (projection)
    {
        const float offsets[2] = {projection->offX, projection->offY};
        if (!WriteBytes(renderView + g_layout->fovY, &projection->fovY, 4) ||
            !WriteBytes(renderView + g_layout->aspect, &projection->aspect, 4) ||
            !WriteBytes(renderView + g_layout->viewportOffset, offsets, sizeof(offsets)))
            return false;
    }
    uint32_t dirty = 0;
    return ReadU32(reinterpret_cast<uintptr_t>(renderView + g_layout->dirty), &dirty) &&
           WriteU32(reinterpret_cast<uintptr_t>(renderView + g_layout->dirty), dirty | 0xD);
}

bool SaneCamera(const float m[16])
{
    for (int r = 0; r < 3; ++r)
    {
        const float len = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (!(len > 0.5f && len < 2.0f))
            return false;
    }
    for (int i = 12; i < 15; ++i)
        if (!std::isfinite(m[i]))
            return false;
    return true;
}

// The two split-screen views made the two eyes: view 0 (left half) the left eye, view 1 (right half) the right. View 1
// was built as a test camera (view 0 turned and moved), so both start from view 0's camera, the head centre.
void MakeEyes(unsigned char* views)
{
    PerfScope perf(kPerfMakeEyes); // hooks/perf_probe.h
    const ViewLayout& L = *g_layout;
    float centre[16], centrePrev[16];
    if (!ReadBytes(views + L.renderView, centre, sizeof(centre)) || !SaneCamera(centre))
        return;
    const bool havePrev = L.renderViewPrev != 0 && ReadBytes(views + L.renderViewPrev, centrePrev, sizeof(centrePrev)) &&
                          SaneCamera(centrePrev);
    // The first-person batches' camera: the views' raw camera, the head centre while this is active (CommitViewTransform
    // adds no eye offset). Both views' copies start from view 0's. MOHWD.exe's (unconfirmed): only while it sits where
    // the view does.
    float rawCentre[16];
    bool haveRaw = L.rawCamera != 0 && ReadBytes(views + L.rawCamera, rawCentre, sizeof(rawCentre)) && SaneCamera(rawCentre);
    float rawDistance = -1.0f;
    if (haveRaw && !L.rawCameraConfirmed)
    {
        const float dx = rawCentre[12] - centre[12], dy = rawCentre[13] - centre[13], dz = rawCentre[14] - centre[14];
        rawDistance = sqrtf(dx * dx + dy * dy + dz * dz);
        haveRaw = rawDistance < kRawCameraMatchMetres;
    }
    float rawEye[2][3] = {};

    mohwvr::ipc::HmdViewBlock hmd{};
    EyeProjection eyes[2];
    bool haveFrustum = GetHmdView(&hmd) && EyeFrustum(hmd, 0, &eyes[0]) && EyeFrustum(hmd, 1, &eyes[1]);

    for (int eye = 0; eye < 2; ++eye)
    {
        unsigned char* view = views + eye * L.descSize;
        const float offset = GetEyeOffsetAlongRow0(eye == 1);
        const EyeProjection* projection = haveFrustum ? &eyes[eye] : nullptr;
        MakeEye(view + L.renderView, centre, offset, projection);
        MakeEye(view + L.renderViewCopy, centre, offset, projection);
        if (havePrev) // the previous frame's camera, for motion vectors: the same eye of it
            MakeEye(view + L.renderViewPrev, centrePrev, offset, projection);
        if (haveRaw)
        {
            float m[16];
            memcpy(m, rawCentre, sizeof(m));
            for (int i = 0; i < 3; ++i)
                m[12 + i] += m[i] * offset;
            if (WriteBytes(view + L.rawCamera, m, sizeof(m)))
                memcpy(rawEye[eye], m + 12, sizeof(rawEye[eye]));
        }
    }
    // SCOPED EYE (2026-11-14, hooks/scope_view.h): an eye behind the gun's scope sees through it -- its world view (not
    // its raw camera: the first-person batches still find their eye by it, and aren't drawn in this one) becomes the
    // scope's camera and magnified frustum. Not in the driving missions (no guns; the weapon reads are MOHW.exe's).
    if (haveFrustum && !g_drivingExe)
    {
        float k = GetFrustumScale();
        if (!(k > 0.2f && k < 4.0f))
            k = 1.0f;
        float tangents[2][4];
        for (int eye = 0; eye < 2; ++eye)
        {
            tangents[eye][0] = tanf(hmd.angleLeft[eye]) * k;
            tangents[eye][1] = tanf(hmd.angleRight[eye]) * k;
            tangents[eye][2] = tanf(hmd.angleUp[eye]) * k;
            tangents[eye][3] = tanf(hmd.angleDown[eye]) * k;
        }
        ScopeEyeView scope{};
        if (ComputeScopedEye(centre, tangents, &scope) && scope.eye >= 0 && scope.eye < 2)
        {
            unsigned char* view = views + scope.eye * L.descSize;
            const EyeProjection projection{scope.fovY, scope.aspect, scope.offX, scope.offY};
            MakeEye(view + L.renderView, scope.camera, 0.0f, &projection);
            MakeEye(view + L.renderViewCopy, scope.camera, 0.0f, &projection);
            if (L.renderViewPrev != 0)
                MakeEye(view + L.renderViewPrev, scope.camera, 0.0f, &projection); // no motion blur through it
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_eyeMutex);
        for (int i = 0; i < 3; ++i)
        {
            g_centrePos[i] = centre[12 + i];
            g_centreRight[i] = centre[i];
            g_writtenEye[0][i] = centre[12 + i] + centre[i] * GetEyeOffsetAlongRow0(false);
            g_writtenEye[1][i] = centre[12 + i] + centre[i] * GetEyeOffsetAlongRow0(true);
            g_worldRing[g_worldRingNext][0][i] = g_writtenEye[0][i];
            g_worldRing[g_worldRingNext][1][i] = g_writtenEye[1][i];
            g_worldRing[g_worldRingNext][2][i] = centre[12 + i];
        }
        g_worldRingNext = (g_worldRingNext + 1) % kEyeCameraFrames;
        if (g_worldRingCount < kEyeCameraFrames)
            ++g_worldRingCount;
        g_haveWrittenEyes = true;
        if (haveRaw)
        {
            memcpy(g_eyeCameraPos[g_eyeCameraNext], rawEye, sizeof(rawEye));
            g_eyeCameraFrame[g_eyeCameraNext] = ++g_framesBuilt;
            g_eyeCameraNext = (g_eyeCameraNext + 1) % kEyeCameraFrames;
            if (g_eyeCameraCount < kEyeCameraFrames)
                ++g_eyeCameraCount;
        }
        if (haveFrustum)
        {
            for (int eye = 0; eye < 2; ++eye)
            {
                g_eyeProjection[eye][0] = eyes[eye].fovY;
                g_eyeProjection[eye][1] = eyes[eye].aspect;
                g_eyeProjection[eye][2] = eyes[eye].offX;
                g_eyeProjection[eye][3] = eyes[eye].offY;
            }
            g_haveEyeProjection = true;
        }
    }

    static unsigned long long nextLogMs = 0;
    const unsigned long long now = GetTickCount64();
    if (now >= nextLogMs)
    {
        nextLogMs = now + 5000;
        uint16_t vp[2][4] = {};
        ReadBytes(views + L.viewport, vp[0], sizeof(vp[0]));
        ReadBytes(views + L.descSize + L.viewport, vp[1], sizeof(vp[1]));
        MOHW_LOG(kLogFile,
                 "two views (%s): left viewport (%u,%u %ux%u), right (%u,%u %ux%u); eye offsets %.4f / %.4f m; frustum %s "
                 "(left fov %.1f deg aspect %.3f offset %.3f,%.3f; right offset %.3f,%.3f); head centre (%.2f, %.2f, "
                 "%.2f); first-person camera %s (%.3f m from the view%s)",
                 L.exe, vp[0][0], vp[0][1], vp[0][2], vp[0][3], vp[1][0], vp[1][1], vp[1][2], vp[1][3],
                 GetEyeOffsetAlongRow0(false), GetEyeOffsetAlongRow0(true), haveFrustum ? "per eye" : "the game's",
                 haveFrustum ? eyes[0].fovY * 57.29578f : 0.0f, haveFrustum ? eyes[0].aspect : 0.0f,
                 haveFrustum ? eyes[0].offX : 0.0f, haveFrustum ? eyes[0].offY : 0.0f,
                 haveFrustum ? eyes[1].offX : 0.0f, haveFrustum ? eyes[1].offY : 0.0f, centre[12], centre[13],
                 centre[14], haveRaw ? "per eye" : "NOT FOUND (the gun stays at the head centre)", rawDistance,
                 L.rawCameraConfirmed ? "" : ", used only under 0.5 m");
    }
}

void __fastcall HookedBuildViews(void* self, void* edx, void* params, uint32_t* count, unsigned char* views)
{
    g_originalBuildViews(self, edx, params, count, views);
    if (!GetNativeStereoEnabled())
        return;
    uint32_t n = 0;
    ReadU32(reinterpret_cast<uintptr_t>(count), &n);
    static uint32_t lastCount = ~0u;
    if (n != lastCount)
    {
        lastCount = n;
        MOHW_LOG(kLogFile, "the engine built %u view(s) this frame%s", n,
                 n == 2 ? ""
                        : (g_wantTwoViews.load(std::memory_order_relaxed)
                               ? " (two asked for)"
                               : " (a whole 2D frame: one view, the floating screen in both eyes)"));
    }
    if (n != 2)
        return;
    MakeEyes(views);
    g_twoViewsMs.store(GetTickCount64(), std::memory_order_relaxed);
}

// THE LEVEL'S RENDER MODE (2026-11-12): the client world module is built at each level load (FUN_00A6B510,
// OFFSET_CLIENT_WORLD_MODULE_CTOR: __thiscall, 4 stack args, RET 0x10), and builds the level's render targets for the
// render mode and size in force then -- a mode switched inside a level only fully applies after a reload. Recorded here
// for the dashboard's note under the render mode (sdk/settings.h NoteLevelRenderMode).
using WorldModuleCtorFn = void*(__fastcall*)(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
WorldModuleCtorFn g_originalWorldModuleCtor = nullptr;
void* g_worldModuleCtorAddress = nullptr;

void* __fastcall HookedWorldModuleCtor(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    const bool twoView = GetNativeStereoEnabled();
    NoteLevelRenderMode(twoView);
    MOHW_LOG(kLogFile, "a level is being built (client world module %p): render mode %s", self,
             twoView ? "engine two-view" : "alternating eye");
    PerfNoteLevelBuilt();
    return g_originalWorldModuleCtor(self, edx, a, b, c, d);
}

} // namespace

namespace {

// THE DRIVING MISSIONS (2026-11-18): MOHWD.exe's builder, FUN_0092c280 (sdk/mohw_offsets.h MOHWD_VIEW_LIST_BUILD), with
// its own layout. Its prologue is checked first: the address is for this build only (dllmain checks the image size).
bool InstallDrivingNativeStereoHook()
{
    static const uint8_t kPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xD4, 0x0E, 0x00, 0x00};
    void* target = reinterpret_cast<void*>(InThisExe(MOHWD_VIEW_LIST_BUILD));
    uint8_t bytes[sizeof(kPrologue)] = {};
    if (!ReadBytes(target, bytes, sizeof(bytes)) || memcmp(bytes, kPrologue, sizeof(bytes)) != 0)
    {
        MOHW_LOG(kLogFile, "MOHWD.exe view list builder (FUN_0092c280 @ %p): unexpected bytes -- engine two-view off", target);
        return false;
    }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    g_layout = &kMohwdLayout;
    s = MH_CreateHook(target, reinterpret_cast<void*>(&HookedBuildViews), reinterpret_cast<void**>(&g_originalBuildViews));
    if (s == MH_OK)
        s = MH_EnableHook(target);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MOHWD.exe view list hook (FUN_0092c280 @ %p) FAILED: %s", target, MH_StatusToString(s));
        return false;
    }
    g_buildViewsAddress = target;
    MOHW_LOG(kLogFile, "MOHWD.exe view list hook installed (FUN_0092c280 @ %p); render mode %s", target,
             GetNativeStereoEnabled() ? "engine two-view" : "alternating eye");
    return true;
}

} // namespace

bool InstallNativeStereoHook()
{
    if (g_drivingExe)
        return InstallDrivingNativeStereoHook();
    if (g_moduleBase == 0 || !g_buildVerified)
        return false;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    g_buildViewsAddress = Offset<void*>(OFFSET_VIEW_LIST_BUILD);
    s = MH_CreateHook(g_buildViewsAddress, reinterpret_cast<void*>(&HookedBuildViews),
                      reinterpret_cast<void**>(&g_originalBuildViews));
    if (s == MH_OK)
        s = MH_EnableHook(g_buildViewsAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "view list hook (FUN_00A6BDC0 @ %p) FAILED: %s", g_buildViewsAddress, MH_StatusToString(s));
        g_buildViewsAddress = nullptr;
        return false;
    }
    MOHW_LOG(kLogFile, "view list hook installed (FUN_00A6BDC0 @ %p); NativeStereo %s", g_buildViewsAddress,
             GetNativeStereoEnabled() ? "on" : "off");

    // Not fatal: without it the dashboard just never shows the reload note.
    g_worldModuleCtorAddress = Offset<void*>(OFFSET_CLIENT_WORLD_MODULE_CTOR);
    s = MH_CreateHook(g_worldModuleCtorAddress, reinterpret_cast<void*>(&HookedWorldModuleCtor),
                      reinterpret_cast<void**>(&g_originalWorldModuleCtor));
    if (s == MH_OK)
        s = MH_EnableHook(g_worldModuleCtorAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "level-load hook (FUN_00A6B510 @ %p) FAILED: %s", g_worldModuleCtorAddress,
                 MH_StatusToString(s));
        g_worldModuleCtorAddress = nullptr;
    }
    return true;
}

void RemoveNativeStereoHook()
{
    if (g_worldModuleCtorAddress)
    {
        MH_DisableHook(g_worldModuleCtorAddress);
        MH_RemoveHook(g_worldModuleCtorAddress);
        g_worldModuleCtorAddress = nullptr;
    }
    if (g_buildViewsAddress)
    {
        MH_DisableHook(g_buildViewsAddress);
        MH_RemoveHook(g_buildViewsAddress);
        g_buildViewsAddress = nullptr;
    }
}

// DEPTH OF FIELD IN TWO-VIEW (2026-11-11, "the cutscene artifact only occurs once the camera moves onto the player
// body ... something is getting duplicated into the eye render"): frame traces from that moment show passes only it
// has, once per eye: the whole side-by-side frame downsampled into a 1920x1080 target, blurred, then a full-frame pass
// reading the blur, the full image and the 3840x2160 R32 depth -- a depth-of-field blend. Its downsample holds both
// eyes, so each eye blends both eyes' images in. While two views render, and unless DepthOfFieldTwoView is on, every
// GlobalPostProcessSettings the world renderer holds gets ForceDofEnable 0 (off), DiffusionDofEnable and ADSDoFEnable
// false; the game's values come back once two-view stops. Each object's vtable is checked before it's touched.
struct DofState
{
    uintptr_t settings = 0;
    int32_t forceDof = 0;
    uint8_t diffusion = 0, ads = 0;
    bool changed = false;
};
DofState g_dof[2]; // Present thread

// The world renderer's post-process settings (slot 0 = the registered "PostProcess", 1 = the second), 0 if not there.
void FindPostProcessSettings(uintptr_t out[2])
{
    out[0] = out[1] = 0;
    uint32_t renderer = 0, module = 0, vtable = 0, world = 0, post = 0;
    if (!ReadU32(g_baseDelta + OFFSET_GAMERENDERER, &renderer) || !renderer ||
        !ReadU32(renderer + GAMERENDERER_MODULES + 4, &module) || !module || !ReadU32(module, &vtable) ||
        vtable != static_cast<uint32_t>(g_baseDelta + OFFSET_CLIENTWORLDMODULE_VTABLE) ||
        !ReadU32(module + CLIENTWORLDMODULE_WORLDRENDERER, &world) || !world || !ReadU32(world, &vtable) ||
        vtable != static_cast<uint32_t>(g_baseDelta + OFFSET_WORLDRENDERER_VTABLE) ||
        !ReadU32(world + WORLDRENDERER_POSTPROCESS, &post) || !post)
        return;
    const uint32_t slots[2] = {POSTPROCESS_SETTINGS, POSTPROCESS_SETTINGS2};
    for (int i = 0; i < 2; ++i)
    {
        uint32_t settings = 0;
        if (ReadU32(post + slots[i], &settings) && settings && ReadU32(settings, &vtable) &&
            vtable == static_cast<uint32_t>(g_baseDelta + OFFSET_GLOBALPOSTPROCESSSETTINGS_VTABLE))
            out[i] = settings;
    }
}

// The three values as they are now; false if unreadable.
bool ReadDofValues(uintptr_t settings, int32_t* forceDof, uint8_t* diffusion, uint8_t* ads)
{
    return ReadBytes(reinterpret_cast<void*>(settings + GLOBALPOSTPROCESS_FORCEDOFENABLE), forceDof, 4) &&
           ReadBytes(reinterpret_cast<void*>(settings + GLOBALPOSTPROCESS_DIFFUSIONDOFENABLE), diffusion, 1) &&
           ReadBytes(reinterpret_cast<void*>(settings + GLOBALPOSTPROCESS_ADSDOFENABLE), ads, 1);
}

void WriteDofValues(uintptr_t settings, int32_t forceDof, uint8_t diffusion, uint8_t ads)
{
    WriteBytes(reinterpret_cast<void*>(settings + GLOBALPOSTPROCESS_FORCEDOFENABLE), &forceDof, 4);
    WriteBytes(reinterpret_cast<void*>(settings + GLOBALPOSTPROCESS_DIFFUSIONDOFENABLE), &diffusion, 1);
    WriteBytes(reinterpret_cast<void*>(settings + GLOBALPOSTPROCESS_ADSDOFENABLE), &ads, 1);
}

// Self-correcting (2026-11-12): the saved values are kept through frames where the settings can't be reached (a
// level load: the first version forgot them there, then saved its own zeros as "the game's", live: "ForceDofEnable
// 0 -> 0, ADSDoFEnable 0 -> 0", which would have left depth of field off after two-view). While off, anything not
// already off is the game's (a new object, or the game setting it again): saved, then turned off. Going back, the
// game's values are written only over our own zeros.
void UpdateDepthOfField(bool twoViews)
{
    const bool off = twoViews && !GetDepthOfFieldTwoView();
    uintptr_t found[2];
    FindPostProcessSettings(found);
    for (int i = 0; i < 2; ++i)
    {
        DofState& s = g_dof[i];
        if (!found[i])
            continue; // not reachable this frame: keep what's saved
        if (s.settings != found[i])
        {
            // A different object (a level's world renderer): whatever was saved belonged to the old one.
            s = DofState{};
            s.settings = found[i];
        }
        int32_t forceDof = 0;
        uint8_t diffusion = 0, ads = 0;
        if (!ReadDofValues(s.settings, &forceDof, &diffusion, &ads))
            continue;
        const bool allOff = forceDof == 0 && diffusion == 0 && ads == 0;
        if (off && !allOff)
        {
            s.forceDof = forceDof;
            s.diffusion = diffusion;
            s.ads = ads;
            s.changed = true;
            WriteDofValues(s.settings, 0, 0, 0);
            MOHW_LOG(kLogFile,
                     "depth of field off for two-view: post-process settings %p (%s) ForceDofEnable %d -> 0, "
                     "DiffusionDofEnable %u -> 0, ADSDoFEnable %u -> 0",
                     reinterpret_cast<void*>(s.settings), i == 0 ? "the registered one" : "the second", forceDof,
                     diffusion, ads);
        }
        else if (!off && s.changed)
        {
            if (allOff)
                WriteDofValues(s.settings, s.forceDof, s.diffusion, s.ads);
            s.changed = false;
            MOHW_LOG(kLogFile, "depth of field back to the game's: post-process settings %p ForceDofEnable %d%s",
                     reinterpret_cast<void*>(s.settings), s.forceDof,
                     allOff ? "" : " (the game had set its own meanwhile: left as it is)");
        }
    }
}

// RENDER SCALE FROM STEAMVR (2026-11-12, "is there a way we can utilize the SteamVR resolution multiplier? The in game
// resolution just is too low but I don't want to create hardcoded resolutions"): GameRenderSettings.ResolutionScale
// (+0x2C, Float32, the engine's own 3D render scale; type record 02B66190) set to SteamVR's recommended eye height
// (its resolution slider and per-app multiplier included) over the eye's height in the backbuffer (the full height in
// both modes). The game's own value is logged once and put back when RenderScaleFromSteamVr is off. EXPERIMENTAL:
// whether the engine applies it live or only when its targets are rebuilt (a resize, a level load) is to be seen.
constexpr uint32_t kGameRenderSettingsResolutionScale = 0x2C;
std::atomic<unsigned> g_backbufferWidth{0}, g_backbufferHeight{0};
uintptr_t g_scaleSettings = 0; // Present thread: the settings object the saved value is from
float g_gameScale = 1.0f;
bool g_scaleChanged = false;

void UpdateRenderScale()
{
    uint32_t renderer = 0, settings = 0;
    if (!ReadU32(g_baseDelta + OFFSET_GAMERENDERER, &renderer) || !renderer ||
        !ReadU32(renderer + GAMERENDERER_SETTINGS, &settings) || !settings)
        return;
    const uintptr_t field = settings + kGameRenderSettingsResolutionScale;
    float current = 0.0f;
    if (!ReadBytes(reinterpret_cast<void*>(field), &current, 4))
        return;
    if (settings != g_scaleSettings)
    {
        g_scaleSettings = settings;
        g_gameScale = current;
        g_scaleChanged = false;
        MOHW_LOG(kLogFile, "GameRenderSettings %p: the game's ResolutionScale is %.3f", reinterpret_cast<void*>(settings),
                 current);
    }
    const unsigned bbH = g_backbufferHeight.load(std::memory_order_relaxed);
    unsigned recW = 0, recH = 0;
    if (GetRenderScaleFromSteamVr() && bbH > 0 && openvr_direct::GetRecommendedEyeSize(&recW, &recH))
    {
        // No cap of our own (2026-11-12, the user: "SteamVR has its own limit"): its resolution setting is the limit.
        float want = static_cast<float>(recH) / static_cast<float>(bbH);
        want = floorf(want * 20.0f + 0.5f) / 20.0f; // steps of 0.05, so the slider's small moves don't churn
        if (!(want >= 0.05f))
            want = 0.05f;
        if (fabsf(current - want) > 0.001f)
        {
            if (!g_scaleChanged)
                g_gameScale = current;
            if (WriteBytes(reinterpret_cast<void*>(field), &want, 4))
            {
                g_scaleChanged = true;
                MOHW_LOG(kLogFile,
                         "ResolutionScale %.3f -> %.2f: SteamVR's recommended eye %ux%u over the backbuffer's eye height %u "
                         "(backbuffer %ux%u)",
                         current, want, recW, recH, bbH, g_backbufferWidth.load(std::memory_order_relaxed), bbH);
            }
        }
    }
    else if (g_scaleChanged)
    {
        if (WriteBytes(reinterpret_cast<void*>(field), &g_gameScale, 4))
            MOHW_LOG(kLogFile, "ResolutionScale back to the game's %.3f", g_gameScale);
        g_scaleChanged = false;
    }
}

// EYE-SIZED BACKBUFFER (2026-11-12, EXPERIMENTAL, setting EyeSizedBackbuffer). In two-view each eye is half the
// backbuffer (1920x2160 of 3840x2160), so the world and the HUD reach the headset at half the width alternating eyes
// gave, and the render scale can't help: it renders larger and is shrunk back into the half. The user: "Size the
// backbuffer to SteamVR's request ... try this". So the game is asked for SteamVR's recommended eye size, two side by
// side, through its own screen state (sdk/mohw_offsets.h DXRENDERER_*): the next-resize size (+0x28/+0x2C) and, while
// the game believes it's fullscreen (KeepGameWindowed answers its request without carrying it out), the settings' mode
// size too, else it would re-apply its mode every frame. Its per-frame display update (FUN_00739b00, the one that
// presents) then runs its own full reset (FUN_007380f0): ResizeBuffers to that size, the views rebuilt. Written from
// Present, which runs inside that update on the same thread. Only with the swap chain really windowed: in exclusive
// fullscreen the size has to be a monitor mode. The game's own sizes are kept and put back when it's switched off; a
// size the game picks meanwhile (its video options) becomes the one put back. If the game keeps setting its own size
// back, it gives up (logged).
std::atomic<bool> g_swapChainWindowed{false};
struct EyeBackbuffer
{
    bool active = false;
    bool gaveUp = false;
    uintptr_t renderer = 0;
    uint32_t gameTarget[2] = {}; // the game's +0x28/+0x2C
    uint32_t gameMode[2] = {};   // the game's settings +0xC/+0x10
    uint32_t written[2] = {};
    unsigned long long windowStartMs = 0;
    int writesInWindow = 0;
};
EyeBackbuffer g_eyeBb; // Present thread

void UpdateEyeSizedBackbuffer()
{
    uint32_t renderer = 0, settings = 0;
    if (!ReadU32(g_baseDelta + OFFSET_DXRENDERER, &renderer) || !renderer ||
        !ReadU32(renderer + DXRENDERER_SETTINGS, &settings) || !settings)
        return;
    uint32_t target[2] = {}, mode[2] = {};
    uint8_t believedFullscreen = 0;
    if (!ReadU32(renderer + DXRENDERER_TARGET_WIDTH, &target[0]) ||
        !ReadU32(renderer + DXRENDERER_TARGET_HEIGHT, &target[1]) ||
        !ReadU32(settings + DXRENDERSETTINGS_MODE_WIDTH, &mode[0]) ||
        !ReadU32(settings + DXRENDERSETTINGS_MODE_HEIGHT, &mode[1]) ||
        !ReadBytes(reinterpret_cast<void*>(renderer + DXRENDERER_FULLSCREEN), &believedFullscreen, 1))
        return;

    unsigned recW = 0, recH = 0;
    const bool want = GetEyeSizedBackbuffer() && GetNativeStereoEnabled() && GetKeepGameWindowed() &&
                      g_swapChainWindowed.load(std::memory_order_relaxed) && !g_eyeBb.gaveUp &&
                      openvr_direct::GetRecommendedEyeSize(&recW, &recH) && recW >= 256 && recH >= 256;
    if (!GetEyeSizedBackbuffer() && g_eyeBb.gaveUp)
        g_eyeBb.gaveUp = false; // switching it off and on tries again

    if (!want)
    {
        if (g_eyeBb.active)
        {
            if (renderer == g_eyeBb.renderer)
            {
                WriteU32(renderer + DXRENDERER_TARGET_WIDTH, g_eyeBb.gameTarget[0]);
                WriteU32(renderer + DXRENDERER_TARGET_HEIGHT, g_eyeBb.gameTarget[1]);
                WriteU32(settings + DXRENDERSETTINGS_MODE_WIDTH, g_eyeBb.gameMode[0]);
                WriteU32(settings + DXRENDERSETTINGS_MODE_HEIGHT, g_eyeBb.gameMode[1]);
            }
            MOHW_LOG(kLogFile, "eye-sized backbuffer off: the game's %ux%u (mode %ux%u) back", g_eyeBb.gameTarget[0],
                     g_eyeBb.gameTarget[1], g_eyeBb.gameMode[0], g_eyeBb.gameMode[1]);
            g_eyeBb.active = false;
        }
        return;
    }

    // D3D11's largest texture is 16384 (the API's limit, not ours); even sizes so the halves are equal.
    uint32_t w = recW * 2, h = recH;
    if (w > 16384)
        w = 16384;
    if (h > 16384)
        h = 16384;
    w &= ~1u;
    h &= ~1u;

    if (!g_eyeBb.active || renderer != g_eyeBb.renderer)
    {
        g_eyeBb = EyeBackbuffer{};
        g_eyeBb.active = true;
        g_eyeBb.renderer = renderer;
        memcpy(g_eyeBb.gameTarget, target, sizeof(target));
        memcpy(g_eyeBb.gameMode, mode, sizeof(mode));
        MOHW_LOG(kLogFile, "eye-sized backbuffer on: the game's own size %ux%u (mode %ux%u), the game %s fullscreen",
                 target[0], target[1], mode[0], mode[1], believedFullscreen ? "believes it's" : "isn't");
    }
    // A size the game set itself since (its video options): the one to put back later.
    if ((target[0] != g_eyeBb.written[0] || target[1] != g_eyeBb.written[1]) && (target[0] != w || target[1] != h) &&
        g_eyeBb.written[0] != 0)
    {
        memcpy(g_eyeBb.gameTarget, target, sizeof(target));
        if (mode[0] != g_eyeBb.written[0] || mode[1] != g_eyeBb.written[1])
            memcpy(g_eyeBb.gameMode, mode, sizeof(mode));
        MOHW_LOG(kLogFile, "eye-sized backbuffer: the game set its own size %ux%u (mode %ux%u) -- kept as the game's",
                 target[0], target[1], mode[0], mode[1]);
    }

    const bool modeNeeded = believedFullscreen != 0;
    if (target[0] == w && target[1] == h && (!modeNeeded || (mode[0] == w && mode[1] == h)))
        return;

    const unsigned long long now = GetTickCount64();
    if (now - g_eyeBb.windowStartMs > 10000)
    {
        g_eyeBb.windowStartMs = now;
        g_eyeBb.writesInWindow = 0;
    }
    if (++g_eyeBb.writesInWindow > 5)
    {
        g_eyeBb.gaveUp = true;
        MOHW_LOG(kLogFile,
                 "eye-sized backbuffer: the game set its size back %d times in 10 s -- giving up (switch the setting off "
                 "and on to try again)",
                 g_eyeBb.writesInWindow - 1);
        return;
    }
    bool ok = WriteU32(renderer + DXRENDERER_TARGET_WIDTH, w) && WriteU32(renderer + DXRENDERER_TARGET_HEIGHT, h);
    if (ok && modeNeeded)
        ok = WriteU32(settings + DXRENDERSETTINGS_MODE_WIDTH, w) && WriteU32(settings + DXRENDERSETTINGS_MODE_HEIGHT, h);
    g_eyeBb.written[0] = w;
    g_eyeBb.written[1] = h;
    MOHW_LOG(kLogFile,
             "eye-sized backbuffer: asking the game for %ux%u (SteamVR's eye %ux%u, two side by side; was %ux%u, mode "
             "%ux%u)%s",
             w, h, recW, recH, target[0], target[1], mode[0], mode[1], ok ? "" : " -- WRITE FAILED");
}

void NativeStereoOnPresent()
{
    if (!g_buildViewsAddress)
        return;
    if (!g_drivingExe) // MOHW.exe's objects (sdk/mohw_offsets.h): not in the driving missions' build
    {
        UpdateDepthOfField(IsNativeStereoActive());
        UpdateEyeSizedBackbuffer();
        UpdateRenderScale();
    }
    const uintptr_t address = ViewCountAddress();
    uint32_t value = 0;
    if (!address || !ReadU32(address, &value))
        return;
    if (!g_haveGameViewCount)
    {
        g_haveGameViewCount = true;
        g_gameViewCount = value;
        MOHW_LOG(kLogFile, "SplitScreenTestViewCount is %u (the game's own)", value);
    }
    // Two views while the setting is on and the frame is a 3D one; the game's own count for whole 2D frames (see
    // g_wantTwoViews).
    bool want = false;
    if (GetNativeStereoEnabled())
    {
        unsigned long long forMs = 0;
        const bool flat = IsWhole2DFrameNow(&forMs);
        want = g_wantTwoViews.load(std::memory_order_relaxed);
        const unsigned long long now = GetTickCount64();
        if (want == flat && forMs >= (flat ? kOneViewAfterMs : kTwoViewsAfterMs) &&
            (g_lastSwitchMs == 0 || now - g_lastSwitchMs >= kMinSwitchGapMs))
        {
            want = !flat;
            g_lastSwitchMs = now;
            MOHW_LOG(kLogFile, "%s (for %llu ms): %s", flat ? "a whole 2D frame" : "a 3D frame", forMs,
                     flat ? "one view, the floating screen in both eyes" : "two views, one per eye");
        }
    }
    g_wantTwoViews.store(want, std::memory_order_relaxed);
    if (want)
    {
        if (value != 2 && WriteU32(address, 2))
        {
            g_viewCountChanged = true;
            MOHW_LOG(kLogFile, "SplitScreenTestViewCount %u -> 2", value);
        }
    }
    else if (g_viewCountChanged && value == 2 && WriteU32(address, g_gameViewCount))
    {
        g_viewCountChanged = false;
        MOHW_LOG(kLogFile, "SplitScreenTestViewCount back to %u", g_gameViewCount);
    }
}

void NativeStereoNoteBackbuffer(unsigned width, unsigned height)
{
    const unsigned oldW = g_backbufferWidth.exchange(width, std::memory_order_relaxed);
    const unsigned oldH = g_backbufferHeight.exchange(height, std::memory_order_relaxed);
    if ((oldW != width || oldH != height) && oldW != 0)
        MOHW_LOG(kLogFile, "backbuffer %ux%u -> %ux%u", oldW, oldH, width, height);
}

void NativeStereoNoteSwapChainWindowed(bool windowed)
{
    g_swapChainWindowed.store(windowed, std::memory_order_relaxed);
}

bool NativeStereoTwoViewsWanted()
{
    return g_buildViewsAddress != nullptr && GetNativeStereoEnabled() && g_wantTwoViews.load(std::memory_order_relaxed);
}

bool IsNativeStereoActive()
{
    // g_wantTwoViews as well: right after the count goes back to one, the last two-view frame is recent but the frames
    // now coming are single views, which mustn't be split into halves.
    return GetNativeStereoEnabled() && g_wantTwoViews.load(std::memory_order_relaxed) &&
           GetTickCount64() - g_twoViewsMs.load(std::memory_order_relaxed) < kRecentMs;
}

int NativeStereoEyeForCamera(const float position[3], unsigned* frame)
{
    float best = kEyeCameraMatchMetres * kEyeCameraMatchMetres;
    int eye = -1;
    unsigned bestFrame = 0;
    std::lock_guard<std::mutex> lock(g_eyeMutex);
    // Newest first, and an exact match (the batch's camera is a copy of the one written) ends the search: with the head
    // still, older frames can be as close.
    for (int age = 0; age < g_eyeCameraCount; ++age)
    {
        const int f = (g_eyeCameraNext - 1 - age + kEyeCameraFrames) % kEyeCameraFrames;
        for (int e = 0; e < 2; ++e)
        {
            const float* p = g_eyeCameraPos[f][e];
            const float dx = p[0] - position[0], dy = p[1] - position[1], dz = p[2] - position[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best || (d2 == 0.0f && best != 0.0f))
            {
                best = d2;
                eye = e;
                bestFrame = g_eyeCameraFrame[f];
            }
        }
        if (best == 0.0f)
            break;
    }
    if (frame)
        *frame = bestFrame;
    return eye;
}

int NativeStereoClassifyWorldCamera(const float position[3])
{
    float best = kEyeCameraMatchMetres * kEyeCameraMatchMetres;
    int which = -1;
    std::lock_guard<std::mutex> lock(g_eyeMutex);
    for (int f = 0; f < g_worldRingCount; ++f)
    {
        for (int k = 0; k < 3; ++k)
        {
            const float* p = g_worldRing[f][k];
            const float dx = p[0] - position[0], dy = p[1] - position[1], dz = p[2] - position[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best)
            {
                best = d2;
                which = k;
            }
        }
    }
    return which;
}

bool GetNativeEyeCameras(float centre[3], float right[3], float left[3], float rightEye[3])
{
    std::lock_guard<std::mutex> lock(g_eyeMutex);
    if (!g_haveWrittenEyes)
        return false;
    memcpy(centre, g_centrePos, sizeof(g_centrePos));
    memcpy(right, g_centreRight, sizeof(g_centreRight));
    memcpy(left, g_writtenEye[0], sizeof(g_writtenEye[0]));
    memcpy(rightEye, g_writtenEye[1], sizeof(g_writtenEye[1]));
    return true;
}

bool GetNativeEyeProjection(int eye, float out[4])
{
    if (eye < 0 || eye > 1)
        return false;
    std::lock_guard<std::mutex> lock(g_eyeMutex);
    if (!g_haveEyeProjection)
        return false;
    memcpy(out, g_eyeProjection[eye], sizeof(g_eyeProjection[eye]));
    return true;
}

} // namespace mohw
