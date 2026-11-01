#include "fire_candidate_redirect_hook.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/mohw_common.h"
#include "../sdk/vr_math.h"
#include "../sdk/motion_controller_aim.h"
#include "laser_raycast.h"

#include <windows.h>
#include <cmath>
#include <cstring>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_firecandidate.log";
// Switched from fixed test-only rotations (45-degree pitch-down, then
// 15-degree yaw-right) to the real right motion controller's live aim
// direction (2026-09-11), per the user's explicit request: "direct the
// shot" using controller ROTATION, keeping the game's own muzzle/reticle
// ORIGIN untouched. The ray's original length is preserved -- only its
// heading changes -- since that length is this function's own
// max-engagement-range value, not something we have reason to alter.

// No upper-bound safety cap here (removed 2026-09-11) -- live testing
// showed EVERY captured call on this function has a ray length of
// ~417-429 units regardless of actual target distance (this+0x400 looks
// like it's the weapon's max-engagement-range endpoint, not the real
// candidate's position), so any fixed cap risks silently no-opping every
// call again like the first (100-unit) and second (2000-unit, still too
// close to some untested longer-range case) attempts did. Only the
// near-zero degenerate check below remains.

// FUN_007d4680's confirmed real ABI (see sdk/mohw_offsets.h's
// OFFSET_FIRECANDIDATE comment, from the live Ghidra decompile):
// __thiscall, param_1 (ECX) = this (a per-shot context object, NOT the
// persistent bullet/AI entity), 2 real stack parameters forwarded unmodified
// into the internal FUN_007CF810 call. Return value is a 1-byte flag.
using FireCandidateFn = unsigned char(__thiscall*)(void* param1, unsigned int param2, unsigned int param3);

FireCandidateFn g_originalFireCandidate = nullptr;
void* g_hookAddress = nullptr;

int g_callCount = 0;

thread_local bool t_raysPlaced = false; // SetRaysAlreadyPlaced

// WHAT THIS FUNCTION SEES (live 2026-11-01): it tests one ray against one soldier's hit capsules, inside every
// GameWorld::RayCast that can hit soldiers, so it's handed each cast's own segment, once per soldier near it:
// - the client bullet's and the laser's casts, already where they belong (SetRaysAlreadyPlaced). Sending them to the laser's
//   start kept each one's length, so a slow-motion bullet's 1.9 m segment beside a soldier tested the 1.9 m at the hand,
//   and the laser's second 11.6 m step tested the first again: nothing past the first step could hit a soldier.
// - a copy of the player's shot that steps natively along the game's aim, 11.6 m a tick from the soldier's eye, and no
//   RayCast of ours. Followed here from step to step: each runs on the laser's line as far along as the shot has come.
// - other soldiers' shots, left alone while the laser runs.
struct ShotStep
{
    Vec3 from, to;
    float travelled;
};
constexpr int kShotSteps = 32;
std::mutex g_shotStepsMutex;
ShotStep g_shotSteps[kShotSteps];
int g_nextShotStep = 0;

bool SamePoint(const Vec3& a, const Vec3& b)
{
    constexpr float kSameMetres = 0.01f;
    Vec3 d = a - b;
    return d.Dot(d) < kSameMetres * kSameMetres;
}

// False for a shot that isn't the player's.
bool FollowPlayerShot(const Vec3& from, const Vec3& to, float* travelled)
{
    constexpr float kMuzzleFromEyeMetres = 1.5f;
    std::lock_guard<std::mutex> lock(g_shotStepsMutex);
    float carried = -1.0f;
    for (const ShotStep& step : g_shotSteps)
    {
        if (SamePoint(step.from, from) && SamePoint(step.to, to))
        {
            *travelled = step.travelled; // the same step, for the next soldier near it
            return true;
        }
        if (SamePoint(step.to, from))
        {
            Vec3 length = step.to - step.from;
            carried = step.travelled + sqrtf(length.Dot(length));
        }
    }
    if (carried < 0.0f)
    {
        float eye[3];
        if (GetGameCameraPosition(eye))
        {
            Vec3 fromEye = from - Vec3{eye[0], eye[1], eye[2]};
            if (fromEye.Dot(fromEye) > kMuzzleFromEyeMetres * kMuzzleFromEyeMetres)
                return false;
        }
        carried = 0.0f;
    }
    g_shotSteps[g_nextShotStep] = ShotStep{from, to, carried};
    g_nextShotStep = (g_nextShotStep + 1) % kShotSteps;
    *travelled = carried;
    return true;
}

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
// hooks: a non-static member function on a simple, non-virtual, single-
// inheritance class compiles to __thiscall with the implicit "this" as
// param1, and its member-function pointer has the same representation as a
// plain code address on the 32-bit MSVC ABI (verified via static_assert
// below).
struct ThisCallTrampoline
{
    unsigned char Hooked(unsigned int param2, unsigned int param3)
    {
        void* param1 = this;
        unsigned char* base = reinterpret_cast<unsigned char*>(param1);

        float* originPtr = reinterpret_cast<float*>(base + 0x180);
        float* endPtr = reinterpret_cast<float*>(base + 0x190);
        int* countField = reinterpret_cast<int*>(base + 0x1b4);

        if (!SehSafeTouch(originPtr) || !SehSafeTouch(endPtr) || !SehSafeTouch(countField))
        {
            MOHW_LOG(kLogFile, "Hooked: this=%p field access violation, calling original unmodified", param1);
            return g_originalFireCandidate(param1, param2, param3);
        }

        int n = ++g_callCount;
        int countBefore = *countField;

        Vec3 origin{originPtr[0], originPtr[1], originPtr[2]};
        Vec3 end{endPtr[0], endPtr[1], endPtr[2]};
        Vec3 direction = end - origin;

        float rayLength = sqrtf(direction.Dot(direction));
        if (rayLength < 0.0001f)
        {
            MOHW_LOG(kLogFile, "call #%d: near-zero ray length %.4f, calling original unmodified", n, rayLength);
            unsigned char result = g_originalFireCandidate(param1, param2, param3);
            int countAfter = *countField;
            MOHW_LOG(kLogFile, "call #%d: result=%u candidateCount %d -> %d", n, result, countBefore, countAfter);
            return result;
        }

        float laserStart[3];
        const bool fromLaser = GetLaserShotStart(laserStart);
        float travelled = 0.0f;
        if (t_raysPlaced || (fromLaser && !FollowPlayerShot(origin, end, &travelled)))
            return g_originalFireCandidate(param1, param2, param3);

        LaserNoteShotDirection(direction.x, direction.y, direction.z); // hooks/laser_raycast.h: the camera's forward sign

        Vec3 controllerDir;
        static ShotSpreadRemover spreadRemover; // game thread only
        if (!GetControllerAimDirection(direction, &controllerDir, &spreadRemover))
        {
            MOHW_LOG(kLogFile, "call #%d: no controller pose available, calling original unmodified", n);
            unsigned char result = g_originalFireCandidate(param1, param2, param3);
            int countAfter = *countField;
            MOHW_LOG(kLogFile, "call #%d: result=%u candidateCount %d -> %d", n, result, countBefore, countAfter);
            return result;
        }

        // The shot starts at the controller (the game's origin moved by the hand's offset from the head, see
        // GetControllerOriginOffset) and points along the controller. Both of this per-shot context object's points
        // are restored after the call so nothing is left permanently altered.
        float saved[3] = {endPtr[0], endPtr[1], endPtr[2]};
        float savedOrigin[3] = {originPtr[0], originPtr[1], originPtr[2]};

        Vec3 handOffset{};
        bool fromHand = GetControllerOriginOffset(&handOffset);
        Vec3 newOrigin = fromHand ? origin + handOffset : origin;
        // From the laser's own start when it's running (2026-11-01): the game's origin isn't the head, so origin + the
        // hand's offset from the head landed about a metre from the hand (live: {343.59, 70.56, 206.28}, the hand at
        // {344.57, 70.72, 206.14}).
        if (fromLaser)
        {
            newOrigin = Vec3{laserStart[0], laserStart[1], laserStart[2]};
            fromHand = true;
        }
        Vec3 aimDirection = controllerDir;
        // SHOTS FOLLOW THE LASER (2026-10-15): aimed from the hand at the laser's point (hooks/laser_raycast.h -- its
        // hit, or its end), so a shot lands on the dot, whatever small difference the shot's own origin or the
        // spread mapping would otherwise leave. Without a running laser, the controller's direction as before.
        float laserPoint[3];
        bool onLaser = GetLaserAimPoint(laserPoint);
        if (onLaser)
        {
            Vec3 toPoint{laserPoint[0] - newOrigin.x, laserPoint[1] - newOrigin.y, laserPoint[2] - newOrigin.z};
            float toPointLength = sqrtf(toPoint.Dot(toPoint));
            // Only a point AHEAD along the controller: a laser cast that went wrong (live 2026-10-15 18:17, one cast
            // behind the player) must not turn the shot around -- the controller's direction then, like the tracer.
            if (toPointLength > 0.05f && toPoint.Dot(controllerDir) > 0.0f)
                aimDirection = toPoint * (1.0f / toPointLength);
            else
                onLaser = false;
        }
        if (fromLaser)
            newOrigin = newOrigin + aimDirection * travelled;
        Vec3 newEnd = newOrigin + aimDirection * rayLength;

        originPtr[0] = newOrigin.x;
        originPtr[1] = newOrigin.y;
        originPtr[2] = newOrigin.z;
        endPtr[0] = newEnd.x;
        endPtr[1] = newEnd.y;
        endPtr[2] = newEnd.z;

        MOHW_LOG(kLogFile,
                   "call #%d: origin={%.3f,%.3f,%.3f} -> %s{%.3f,%.3f,%.3f} original end={%.3f,%.3f,%.3f} "
                   "%s-redirected end={%.3f,%.3f,%.3f} (candidateCount before=%d)",
                   n, origin.x, origin.y, origin.z, fromHand ? "hand " : "unchanged ", newOrigin.x, newOrigin.y,
                   newOrigin.z, end.x, end.y, end.z, onLaser ? "laser" : "controller", newEnd.x, newEnd.y, newEnd.z,
                   countBefore);
        float yawOffset = 0.0f, pitchOffset = 0.0f;
        bool locked = spreadRemover.Get(&yawOffset, &pitchOffset);
        const char* status = !GetRemoveShotSpread()            ? "kept (RemoveShotSpread off)"
                             : !locked                         ? "kept (measuring)"
                             : spreadRemover.lastWasOutlier    ? "kept (abnormal shot: game's own direction)"
                                                               : "REMOVED";
        MOHW_LOG(kLogFile,
                   "call #%d: spread %s -- this shot's spread yaw %.2f pitch %.2f deg (locked offsets K %.3f C %.3f rad, "
                   "outlier run %d)",
                   n, status, spreadRemover.lastSpreadYaw * 57.29578f, spreadRemover.lastSpreadPitch * 57.29578f,
                   yawOffset, pitchOffset, spreadRemover.outlierRun);

        unsigned char result = g_originalFireCandidate(param1, param2, param3);

        endPtr[0] = saved[0];
        endPtr[1] = saved[1];
        endPtr[2] = saved[2];
        originPtr[0] = savedOrigin[0];
        originPtr[1] = savedOrigin[1];
        originPtr[2] = savedOrigin[2];

        int countAfter = *countField;
        MOHW_LOG(kLogFile, "call #%d: result=%u candidateCount %d -> %d (done)", n, result, countBefore, countAfter);

        return result;
    }
};

} // namespace

void SetRaysAlreadyPlaced(bool placed)
{
    t_raysPlaced = placed;
}

bool InstallFireCandidateRedirectHook()
{
    if (g_moduleBase == 0 || !g_buildVerified)
    {
        MOHW_LOG(kLogFile, "InstallFireCandidateRedirectHook: module base not resolved / build not verified yet -- "
                             "refusing to install");
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    g_hookAddress = Offset<void*>(OFFSET_FIRECANDIDATE);

    auto memberFn = &ThisCallTrampoline::Hooked;
    static_assert(sizeof(memberFn) == sizeof(void*),
                   "ThisCallTrampoline::Hooked's member-function-pointer isn't a plain code address -- "
                   "check for accidental virtual functions or multiple inheritance");
    void* detour = nullptr;
    memcpy(&detour, &memberFn, sizeof(detour));

    MH_STATUS s = MH_CreateHook(g_hookAddress, detour, reinterpret_cast<void**>(&g_originalFireCandidate));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(FUN_007d4680 @ %p) FAILED: %s", g_hookAddress, MH_StatusToString(s));
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
               "Fire-candidate redirect hook installed @ %p -- every fire-time candidate test's ray end will be "
               "redirected to the right motion controller's live aim direction (origin unchanged)",
               g_hookAddress);
    return true;
}

void RemoveFireCandidateRedirectHook()
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
