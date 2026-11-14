#include "marker_projection_hook.h"

#include "companion_bridge.h"
#include "draw_trace_diag.h"
#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_marker.log";
// Per executable (image base 0x400000): the projection, and the value it writes to both outputs when |w| ~ 0.
// MOHW.exe: sdk/mohw_offsets.h OFFSET_HUDICON_PROJECT. MOHWD.exe (the driving missions, 2026-10-16): the same
// function found by its code bytes (one match; only caller FUN_0128d920, which reads ScreenSafeAreaWidth like
// MOHW.exe's FUN_016ab1c0), instruction for instruction the same, its sentinel load at 0x0128B809.
constexpr uintptr_t kMohwSentinel = 0x27A8320u;
constexpr uintptr_t kMohwdProject = 0x128B670u;
constexpr uintptr_t kMohwdSentinel = 0x230B6E8u;
uintptr_t g_sentinelAddress = 0;

uintptr_t InThisExe(uintptr_t staticAddress)
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) - 0x400000u + staticAddress;
}

using ProjectFn = uint32_t(__stdcall*)(const float* local, const float* transform, const float* viewProj, float halfX,
                                       float halfY, float* out);
ProjectFn g_original = nullptr;
std::atomic<long long> g_calls{0}, g_mapped{0};

bool ReadSentinel(float* out)
{
    __try
    {
        *out = *reinterpret_cast<const float*>(g_sentinelAddress);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

float Length3(float x, float y, float z)
{
    return std::sqrt(x * x + y * y + z * z);
}

// The last markers mapped in two-view mode (diagnostics: hooks/ui_pass.cpp logs them beside the HUD list's commands, to
// find the marker draws). Guarded by g_recentMutex.
struct RecentMarker
{
    float game[2], hud[2];
    unsigned long long ms;
};
constexpr int kRecentMarkers = 16;
std::mutex g_recentMutex;
RecentMarker g_recent[kRecentMarkers] = {};
int g_recentNext = 0;

void NoteRecentMarker(float u, float v, float hudU, float hudV)
{
    std::lock_guard<std::mutex> lock(g_recentMutex);
    g_recent[g_recentNext] = RecentMarker{{u, v}, {hudU, hudV}, GetTickCount64()};
    g_recentNext = (g_recentNext + 1) % kRecentMarkers;
}

// NATIVE TWO-VIEW (2026-11-08, "the world markers do not [follow]... they are also scaled with the rest of the hud"):
// both halves replay one HUD, so a marker has one position for both eyes, and the game projects it with its own camera
// (the head centre, its own 50 deg 16:9 view: the eyes' frusta are given to the views only). So: the point's direction
// from the head centre, from the game's projection (its scale is in viewProj: rows are right/up/back, so the length of a
// column's first three entries over the w column's is that axis's 1/tan), then the HUD position whose two eye images
// average to that direction. Each eye shows HUD position h at fraction rect.x + h * rect.w of its half, at tangent
// left + that * width; both eyes share the vertical. The marker then sits at the HUD's own depth, in the right direction.
bool ReadProjectionScale(const float* viewProj, float* sx, float* sy)
{
    __try
    {
        const float w = Length3(viewProj[3], viewProj[7], viewProj[11]);
        if (!(w > 1e-6f))
            return false;
        *sx = Length3(viewProj[0], viewProj[4], viewProj[8]) / w;
        *sy = Length3(viewProj[1], viewProj[5], viewProj[9]) / w;
        return *sx > 1e-4f && *sy > 1e-4f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool MapForTwoView(const float* viewProj, float spanX, float spanY, float* out)
{
    float rects[2][4];
    if (!GetNativeHudRectsForMarkers(rects))
        return false;
    mohwvr::ipc::HmdViewBlock hmd{};
    if (!GetHmdView(&hmd))
        return false;
    float k = GetFrustumScale(); // the eyes are drawn with tangents * k (hooks/native_stereo.cpp EyeFrustum)
    if (!(k > 0.2f && k < 4.0f))
        k = 1.0f;
    float sx = 0.0f, sy = 0.0f;
    if (!ReadProjectionScale(viewProj, &sx, &sy))
        return false;
    const float ndcX = out[0] / spanX * 2.0f - 1.0f, ndcY = 1.0f - out[1] / spanY * 2.0f;
    const float tx = ndcX / sx, ty = ndcY / sy;
    float numerator = 2.0f * tx, denominator = 0.0f;
    for (int eye = 0; eye < 2; ++eye)
    {
        const float left = tanf(hmd.angleLeft[eye]) * k, width = (tanf(hmd.angleRight[eye]) - tanf(hmd.angleLeft[eye])) * k;
        numerator -= left + rects[eye][0] * width;
        denominator += rects[eye][2] * width;
    }
    const float up = tanf(hmd.angleUp[0]) * k, down = tanf(hmd.angleDown[0]) * k;
    if (!(denominator > 1e-6f) || !(up - down > 1e-6f) || !(rects[0][3] > 1e-6f))
        return false;
    const float hx = numerator / denominator;
    const float hy = ((up - ty) / (up - down) - rects[0][1]) / rects[0][3];
    if (!std::isfinite(hx) || !std::isfinite(hy))
        return false;
    const float u = out[0], v = out[1];
    out[0] = hx * spanX;
    out[1] = hy * spanY;
    NoteRecentMarker(u, v, out[0], out[1]);
    static std::atomic<long long> mapped{0};
    const long long m = ++mapped;
    if (m == 1 || m % 5000 == 0)
        MOHW_LOG(kLogFile, "marker projection, two views: (%.3f, %.3f) of the game's view (1/tan %.3f, %.3f) -> (%.3f, %.3f) "
                           "for HUD rectangles x %.3f / %.3f, w %.3f (%lld mapped)",
                 u, v, sx, sy, out[0], out[1], rects[0][0], rects[1][0], rects[0][2], m);
    return true;
}

uint32_t __stdcall Hooked(const float* local, const float* transform, const float* viewProj, float halfX, float halfY,
                          float* out)
{
    uint32_t inFront = g_original(local, transform, viewProj, halfX, halfY, out);
    long long n = ++g_calls;
    if (!out)
        return inFront;
    float sentinel = 0.0f;
    if (ReadSentinel(&sentinel) && out[0] == sentinel && out[1] == sentinel)
        return inFront; // no position (the point is on the camera plane): left for the game's own handling
    float u = out[0], v = out[1];
    if (!std::isfinite(u) || !std::isfinite(v))
        return inFront;
    // The 0..1 of the frame is halfX * 2 / halfY * 2 wide (0.5 / 0.5 from the only caller); kept general.
    float spanX = 2.0f * halfX, spanY = 2.0f * halfY;
    if (!(spanX > 1e-6f) || !(spanY > 1e-6f))
        return inFront;
    if (viewProj && MapForTwoView(viewProj, spanX, spanY, out))
        return inFront;
    float rect[4];
    if (!GetHudRectForMarkers(rect))
        return inFront;
    out[0] = (u / spanX - rect[0]) / rect[2] * spanX;
    out[1] = (v / spanY - rect[1]) / rect[3] * spanY;
    long long m = ++g_mapped;
    if (m == 1 || m % 5000 == 0)
        MOHW_LOG(kLogFile, "marker projection: (%.3f, %.3f) -> (%.3f, %.3f) in the HUD rectangle %.3f,%.3f %.3fx%.3f "
                           "(%lld mapped of %lld)",
                  u, v, out[0], out[1], rect[0], rect[1], rect[2], rect[3], m, n);
    return inFront;
}

bool PrologueMatches(const uint8_t* p)
{
    __try
    {
        // push ebp; mov ebp,esp; and esp,0xFFFFFFF0 (Ghidra, MOHW_dump_SCY.exe)
        return p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && p[3] == 0x83 && p[4] == 0xE4 && p[5] == 0xF0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

} // namespace

int DescribeRecentMarkers(char* out, int size, unsigned long long withinMs)
{
    if (!out || size <= 0)
        return 0;
    out[0] = '\0';
    const unsigned long long now = GetTickCount64();
    int count = 0, used = 0;
    std::lock_guard<std::mutex> lock(g_recentMutex);
    for (int i = 0; i < kRecentMarkers; ++i)
    {
        const RecentMarker& m = g_recent[i];
        if (m.ms == 0 || now - m.ms > withinMs)
            continue;
        const int n = _snprintf_s(out + used, size - used, _TRUNCATE, " [game %.3f,%.3f hud %.3f,%.3f]", m.game[0],
                                  m.game[1], m.hud[0], m.hud[1]);
        if (n < 0)
            break;
        used += n;
        ++count;
    }
    return count;
}

bool InstallMarkerProjectionHook()
{
    if (!g_drivingExe && (g_moduleBase == 0 || !g_buildVerified))
    {
        MOHW_LOG(kLogFile, "InstallMarkerProjectionHook: module base not resolved / build not verified yet -- refusing");
        return false;
    }
    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }
    void* target = reinterpret_cast<void*>(InThisExe(g_drivingExe ? kMohwdProject : OFFSET_HUDICON_PROJECT));
    g_sentinelAddress = InThisExe(g_drivingExe ? kMohwdSentinel : kMohwSentinel);
    if (!PrologueMatches(static_cast<const uint8_t*>(target)))
    {
        MOHW_LOG(kLogFile, "marker projection @ %p: unexpected bytes -- not hooking", target);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&Hooked), reinterpret_cast<void**>(&g_original));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(marker projection @ %p) FAILED: %s", target, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook(marker projection) FAILED: %s", MH_StatusToString(s));
        return false;
    }
    MOHW_LOG(kLogFile, "marker projection hook installed @ %p", target);
    return true;
}

} // namespace mohw
