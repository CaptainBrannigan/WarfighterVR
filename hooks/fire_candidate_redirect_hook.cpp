#include "fire_candidate_redirect_hook.h"

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
        Vec3 newEnd = newOrigin + controllerDir * rayLength;

        originPtr[0] = newOrigin.x;
        originPtr[1] = newOrigin.y;
        originPtr[2] = newOrigin.z;
        endPtr[0] = newEnd.x;
        endPtr[1] = newEnd.y;
        endPtr[2] = newEnd.z;

        MOHW_LOG(kLogFile,
                   "call #%d: origin={%.3f,%.3f,%.3f} -> %s{%.3f,%.3f,%.3f} original end={%.3f,%.3f,%.3f} "
                   "controller-redirected end={%.3f,%.3f,%.3f} (candidateCount before=%d)",
                   n, origin.x, origin.y, origin.z, fromHand ? "hand " : "unchanged ", newOrigin.x, newOrigin.y,
                   newOrigin.z, end.x, end.y, end.z, newEnd.x, newEnd.y, newEnd.z, countBefore);
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
