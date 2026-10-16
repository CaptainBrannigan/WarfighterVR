#include "marker_projection_hook.h"

#include "draw_trace_diag.h"
#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_marker.log";
constexpr uint32_t kSentinelAddress = 0x27A8320u; // the value written to both outputs when |w| ~ 0

using ProjectFn = uint32_t(__stdcall*)(const float* local, const float* transform, const float* viewProj, float halfX,
                                       float halfY, float* out);
ProjectFn g_original = nullptr;
std::atomic<long long> g_calls{0}, g_mapped{0};

bool ReadSentinel(float* out)
{
    __try
    {
        *out = *Offset<const float*>(kSentinelAddress);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

uint32_t __stdcall Hooked(const float* local, const float* transform, const float* viewProj, float halfX, float halfY,
                          float* out)
{
    uint32_t inFront = g_original(local, transform, viewProj, halfX, halfY, out);
    long long n = ++g_calls;
    float rect[4];
    if (!out || !GetHudRectForMarkers(rect))
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

bool InstallMarkerProjectionHook()
{
    if (g_moduleBase == 0 || !g_buildVerified)
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
    void* target = Offset<void*>(OFFSET_HUDICON_PROJECT);
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
