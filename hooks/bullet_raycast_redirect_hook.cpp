#include "bullet_raycast_redirect_hook.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/mohw_common.h"
#include "../sdk/vr_math.h"
#include "../sdk/motion_controller_aim.h"

#include <windows.h>
#include <cmath>
#include <cstring>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_bulletraycast.log";
// Switched from a fixed 45-degree pitch-down test rotation to the real
// right motion controller's live aim direction (2026-09-11) -- this is the
// VFX/cosmetic bullet-path hook (confirmed live to genuinely redirect the
// visual tracer/impact), re-enabled alongside fire_candidate_redirect_hook
// so the user can visually see where a controller-directed shot travels
// while that hook's real-hit-scan effect is being tested. Kept as a
// SEPARATE call to GetControllerAimDirection (not shared state with
// fire_candidate_redirect_hook.cpp) since the two hooks fire from
// different call sites/threads-of-control and there's no guarantee they
// see the exact same polled pose on the same shot regardless.

// The specific call site this hook targets -- confirmed live via a stack
// dump at GameWorld::RayCast's entry during actual player weapon fire. This
// function is generic/shared; every OTHER ident string passing through must
// be left untouched.
constexpr const char* kTargetIdent = "BulletEntity updateTransformSync";

// GameWorld::RayCast's confirmed real ABI (see sdk/mohw_offsets.h's
// OFFSET_GAMEWORLD_RAYCAST comment): __thiscall, param_1 (ECX) = the
// GameWorld singleton. hits/excluded are opaque (RayCastHit/fixed_vector
// aren't reversed yet) -- passed through untouched as void*.
using GameWorldRayCastFn = bool(__thiscall*)(void* param1, const char* ident, int rayCastTest, Vec3* start,
                                                Vec3* end, void* hits, unsigned int maxHitCount,
                                                unsigned int materialFlags, unsigned int flags, void* excluded);

GameWorldRayCastFn g_originalRayCast = nullptr;
void* g_hookAddress = nullptr;

int g_redirectCount = 0;

bool SehSafeTouch(const void* addr)
{
    __try
    {
        volatile const unsigned char probe = *reinterpret_cast<const unsigned char*>(addr);
        (void)probe;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Same established __thiscall-hooking workaround as this project's other
// hooks (engine_function_hook.cpp, shot_redirect_hook.cpp,
// player_hitscan_redirect_hook.cpp): a non-static member function on a
// simple, non-virtual, single-inheritance class compiles to __thiscall with
// the implicit "this" as param1, and its member-function pointer has the
// same representation as a plain code address on the 32-bit MSVC ABI
// (verified via static_assert below).
struct ThisCallTrampoline
{
    bool Hooked(const char* ident, int rayCastTest, Vec3* start, Vec3* end, void* hits, unsigned int maxHitCount,
                 unsigned int materialFlags, unsigned int flags, void* excluded)
    {
        void* param1 = this;

        if (ident == nullptr || !SehSafeTouch(ident) || std::strcmp(ident, kTargetIdent) != 0)
        {
            return g_originalRayCast(param1, ident, rayCastTest, start, end, hits, maxHitCount, materialFlags,
                                       flags, excluded);
        }

        if (start == nullptr || end == nullptr || !SehSafeTouch(start) || !SehSafeTouch(end))
        {
            MOHW_LOG(kLogFile, "Hooked: matched ident but start/end unreadable (start=%p end=%p), calling "
                                 "original unmodified",
                       start, end);
            return g_originalRayCast(param1, ident, rayCastTest, start, end, hits, maxHitCount, materialFlags,
                                       flags, excluded);
        }

        Vec3 origin = *start;
        Vec3 originalEnd = *end;
        Vec3 direction = originalEnd - origin;

        int n = ++g_redirectCount;

        // Degenerate/near-zero segment guard -- skip the log noise and be
        // explicit about why nothing changed for this call.
        float segLength = sqrtf(direction.Dot(direction));
        if (segLength < 0.0001f)
        {
            MOHW_LOG(kLogFile, "call #%d: near-zero segment, leaving unmodified", n);
            return g_originalRayCast(param1, ident, rayCastTest, start, end, hits, maxHitCount, materialFlags,
                                       flags, excluded);
        }

        Vec3 controllerDir;
        static ShotSpreadRemover spreadRemover; // its own: this raycast may carry a different spread than the hit-scan
        if (!GetControllerAimDirection(direction, &controllerDir, &spreadRemover))
        {
            MOHW_LOG(kLogFile, "call #%d: no controller pose available, leaving unmodified", n);
            return g_originalRayCast(param1, ident, rayCastTest, start, end, hits, maxHitCount, materialFlags,
                                       flags, excluded);
        }

        Vec3 newEnd = origin + controllerDir * segLength;

        // Deliberately NOT restored after the call -- end represents the
        // bullet's actual new position this tick, and should persist so
        // the trajectory change compounds naturally on the next tick
        // rather than snapping back to the original path.
        *end = newEnd;

        MOHW_LOG(kLogFile, "call #%d: origin={%.3f,%.3f,%.3f} original end={%.3f,%.3f,%.3f} redirected "
                             "end={%.3f,%.3f,%.3f}",
                   n, origin.x, origin.y, origin.z, originalEnd.x, originalEnd.y, originalEnd.z, newEnd.x, newEnd.y,
                   newEnd.z);

        bool result = g_originalRayCast(param1, ident, rayCastTest, start, end, hits, maxHitCount, materialFlags,
                                          flags, excluded);

        MOHW_LOG(kLogFile, "call #%d: RayCast returned %s", n, result ? "true (hit)" : "false (no hit)");

        return result;
    }
};

} // namespace

bool InstallBulletRaycastRedirectHook()
{
    if (g_moduleBase == 0 || !g_buildVerified)
    {
        MOHW_LOG(kLogFile, "InstallBulletRaycastRedirectHook: module base not resolved / build not verified "
                             "yet -- refusing to install");
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    g_hookAddress = Offset<void*>(OFFSET_GAMEWORLD_RAYCAST);

    auto memberFn = &ThisCallTrampoline::Hooked;
    static_assert(sizeof(memberFn) == sizeof(void*),
                   "ThisCallTrampoline::Hooked's member-function-pointer isn't a plain code address -- "
                   "check for accidental virtual functions or multiple inheritance");
    void* detour = nullptr;
    memcpy(&detour, &memberFn, sizeof(detour));

    MH_STATUS s = MH_CreateHook(g_hookAddress, detour, reinterpret_cast<void**>(&g_originalRayCast));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(GameWorld::RayCast @ %p) FAILED: %s", g_hookAddress,
                   MH_StatusToString(s));
        g_hookAddress = nullptr;
        return false;
    }

    s = MH_EnableHook(g_hookAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook FAILED: %s", MH_StatusToString(s));
        return false;
    }

    MOHW_LOG(kLogFile,
               "Bullet raycast redirect hook installed @ %p -- every GameWorld::RayCast call with ident=\"%s\" "
               "will have its per-tick segment redirected to the right motion controller's live aim direction",
               g_hookAddress, kTargetIdent);
    return true;
}

void RemoveBulletRaycastRedirectHook()
{
    if (g_hookAddress)
    {
        MH_DisableHook(g_hookAddress);
        MH_RemoveHook(g_hookAddress);
        g_hookAddress = nullptr;
    }
    // MH_Uninitialize() is called once centrally from dllmain.cpp.
}

} // namespace mohw
