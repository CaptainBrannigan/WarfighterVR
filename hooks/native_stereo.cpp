#include "native_stereo.h"

#include "alternating_eye.h"
#include "companion_bridge.h"
#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_nativestereo.log";
constexpr unsigned long long kRecentMs = 250;
constexpr float kMaxFovYRad = 3.05f; // hooks/eye_matched_fov.cpp's clamp: a fovY near pi breaks the projection

using BuildViewsFn = void(__fastcall*)(void* self, void* edx, void* params, uint32_t* count, unsigned char* views);
BuildViewsFn g_originalBuildViews = nullptr;
void* g_buildViewsAddress = nullptr;

std::atomic<unsigned long long> g_twoViewsMs{0};

// The game's own SplitScreenTestViewCount, before this module first changed it.
bool g_haveGameViewCount = false;
uint32_t g_gameViewCount = 0;
bool g_viewCountChanged = false;

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

// GameRenderSettings::SplitScreenTestViewCount's address, 0 while there's no GameRenderer.
uintptr_t ViewCountAddress()
{
    uint32_t renderer = 0, settings = 0;
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
        if (!WriteBytes(renderView + RENDERVIEW_FOVY, &projection->fovY, 4) ||
            !WriteBytes(renderView + RENDERVIEW_ASPECT, &projection->aspect, 4) ||
            !WriteBytes(renderView + RENDERVIEW_VIEWPORTOFFSET, offsets, sizeof(offsets)))
            return false;
    }
    uint32_t dirty = 0;
    return ReadU32(reinterpret_cast<uintptr_t>(renderView + RENDERVIEW_DIRTY), &dirty) &&
           WriteU32(reinterpret_cast<uintptr_t>(renderView + RENDERVIEW_DIRTY), dirty | 0xD);
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
    float centre[16], centrePrev[16];
    if (!ReadBytes(views + VIEWDESC_RENDERVIEW, centre, sizeof(centre)) ||
        !ReadBytes(views + VIEWDESC_RENDERVIEW_PREV, centrePrev, sizeof(centrePrev)) || !SaneCamera(centre))
        return;
    const bool havePrev = SaneCamera(centrePrev);

    mohwvr::ipc::HmdViewBlock hmd{};
    EyeProjection eyes[2];
    bool haveFrustum = GetHmdView(&hmd) && EyeFrustum(hmd, 0, &eyes[0]) && EyeFrustum(hmd, 1, &eyes[1]);

    for (int eye = 0; eye < 2; ++eye)
    {
        unsigned char* view = views + eye * VIEWDESC_SIZE;
        const float offset = GetEyeOffsetAlongRow0(eye == 1);
        const EyeProjection* projection = haveFrustum ? &eyes[eye] : nullptr;
        MakeEye(view + VIEWDESC_RENDERVIEW, centre, offset, projection);
        MakeEye(view + VIEWDESC_RENDERVIEW_COPY, centre, offset, projection);
        if (havePrev) // the previous frame's camera, for motion vectors: the same eye of it
            MakeEye(view + VIEWDESC_RENDERVIEW_PREV, centrePrev, offset, projection);
    }

    static unsigned long long nextLogMs = 0;
    const unsigned long long now = GetTickCount64();
    if (now >= nextLogMs)
    {
        nextLogMs = now + 5000;
        uint16_t vp[2][4] = {};
        ReadBytes(views + VIEWDESC_VIEWPORT, vp[0], sizeof(vp[0]));
        ReadBytes(views + VIEWDESC_SIZE + VIEWDESC_VIEWPORT, vp[1], sizeof(vp[1]));
        MOHW_LOG(kLogFile,
                 "two views: left viewport (%u,%u %ux%u), right (%u,%u %ux%u); eye offsets %.4f / %.4f m; frustum %s "
                 "(left fov %.1f deg aspect %.3f offset %.3f,%.3f; right offset %.3f,%.3f)",
                 vp[0][0], vp[0][1], vp[0][2], vp[0][3], vp[1][0], vp[1][1], vp[1][2], vp[1][3],
                 GetEyeOffsetAlongRow0(false), GetEyeOffsetAlongRow0(true), haveFrustum ? "per eye" : "the game's",
                 haveFrustum ? eyes[0].fovY * 57.29578f : 0.0f, haveFrustum ? eyes[0].aspect : 0.0f,
                 haveFrustum ? eyes[0].offX : 0.0f, haveFrustum ? eyes[0].offY : 0.0f,
                 haveFrustum ? eyes[1].offX : 0.0f, haveFrustum ? eyes[1].offY : 0.0f);
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
                 n == 2 ? "" : " (two needs the setting on at the level's load: reload the level)");
    }
    if (n != 2)
        return;
    MakeEyes(views);
    g_twoViewsMs.store(GetTickCount64(), std::memory_order_relaxed);
}

} // namespace

bool InstallNativeStereoHook()
{
    if (g_moduleBase == 0 || !g_buildVerified || g_drivingExe)
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
    return true;
}

void RemoveNativeStereoHook()
{
    if (g_buildViewsAddress)
    {
        MH_DisableHook(g_buildViewsAddress);
        MH_RemoveHook(g_buildViewsAddress);
        g_buildViewsAddress = nullptr;
    }
}

void NativeStereoOnPresent()
{
    if (!g_buildViewsAddress)
        return;
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
    if (GetNativeStereoEnabled())
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

bool IsNativeStereoActive()
{
    return GetNativeStereoEnabled() &&
           GetTickCount64() - g_twoViewsMs.load(std::memory_order_relaxed) < kRecentMs;
}

} // namespace mohw
