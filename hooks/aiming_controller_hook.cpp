#include "aiming_controller_hook.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/vr_math.h"
#include "../sdk/settings.h"
#include "companion_bridge.h"

#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <atomic>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_aimingcontroller.log";

// Byte offsets into AimingController, all confirmed live -- see
// sdk/mohw_offsets.h's OFFSET_AIMINGCONTROLLER_UPDATE comment.
constexpr int kYawByteOffset = 0xC;
constexpr int kPitchByteOffset = 0x10;
constexpr int kPitchClampMinByteOffset = 0x58; // degrees, live-observed ~-75
constexpr int kPitchClampMaxByteOffset = 0x5C; // degrees, live-observed ~72
constexpr float kFallbackPitchClampMinDeg = -75.0f;
constexpr float kFallbackPitchClampMaxDeg = 72.0f;
constexpr float kDegToRad = 0.017453292519943295f;

// Isolated, no C++ objects requiring unwinding in the frame -- same
// __try/__except-can't-coexist-with-object-unwinding constraint as every
// other SEH-safe helper in this project (see engine_function_hook.cpp's
// SehSafeRead for the original statement of this pattern).
bool SehSafeReadFloat(float* dst, const void* src)
{
    __try
    {
        memcpy(dst, src, sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SehSafeWriteFloat(void* dst, float value)
{
    __try
    {
        memcpy(dst, &value, sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Recenter anchor -- captured once when head-aim turns on (or on an
// explicit F3 press), NOT re-read every frame, so the anchor stays fixed
// while the HMD delta since that moment accumulates on top of it. Same
// "baseline = whatever was already there" philosophy as
// commit_view_transform_hook.cpp's rotation composition, just in scalar
// yaw/pitch space instead of a full basis (AimingController stores plain
// angles, not a rotation matrix).
std::atomic<bool> g_haveRecenter{false};
std::atomic<bool> g_recenterRequested{true}; // true on module load so the very first eligible call recenters
std::atomic<float> g_zeroHeadYaw{0.0f};
std::atomic<float> g_zeroHeadPitch{0.0f};
std::atomic<float> g_baselineYaw{0.0f};
std::atomic<float> g_baselinePitch{0.0f};
// HMD position at the last recenter (XR local space, meters) -- origin for head-position tracking (hooks/head_position.cpp).
std::atomic<float> g_zeroHeadPosX{0.0f}, g_zeroHeadPosY{0.0f}, g_zeroHeadPosZ{0.0f};

// The last yaw/pitch WE ourselves successfully wrote -- see Recenter()'s
// comment for why this, not a fresh memory read, is what recenter should
// baseline from.
std::atomic<bool> g_haveLastWritten{false};
std::atomic<float> g_lastWrittenYaw{0.0f};
std::atomic<float> g_lastWrittenPitch{0.0f};

// fallbackYaw/fallbackPitch: whatever's currently in AimingController's
// fields, ONLY used on the very first-ever recenter (before we've written
// anything ourselves yet). On every SUBSEQUENT recenter, baseline instead
// from our OWN last-written value, not a fresh memory read -- confirmed
// live (2026-08-22) that a fresh read at recenter time is unreliable: it
// consistently returned the exact same fixed value (matching this
// project's already-documented "upstream corrector pulls this field
// toward a stored baseline" mystery, see project_mohw_yaw_facing_
// investigation.md's "Addendum" section) rather than wherever the
// character was actually, visibly facing. On every NORMAL (non-recenter)
// frame this was never a problem, since newYaw/newPitch are computed
// purely from baseline+delta and the fresh read is never used for
// anything -- recenter was the ONLY place a corrector-contaminated raw
// read could leak into the mod's own state. Baselining from our own last
// write instead sidesteps the corrector entirely, since it reflects
// wherever OUR continuous override actually placed the character, which
// is exactly what "recenter to current facing" should mean.
void Recenter(float fallbackYaw, float fallbackPitch)
{
    mohwvr::ipc::HeadPoseBlock pose{};
    if (!GetHeadPose(&pose))
        return; // companion not running / no pose yet -- try again next call

    Quat q{pose.orientationX, pose.orientationY, pose.orientationZ, pose.orientationW};
    float headYaw = 0.0f, headPitch = 0.0f;
    QuatToYawPitch(q, &headYaw, &headPitch);

    bool haveLastWritten = g_haveLastWritten.load(std::memory_order_relaxed);
    float baselineYaw = haveLastWritten ? g_lastWrittenYaw.load(std::memory_order_relaxed) : fallbackYaw;
    float baselinePitch = haveLastWritten ? g_lastWrittenPitch.load(std::memory_order_relaxed) : fallbackPitch;

    g_zeroHeadYaw.store(headYaw, std::memory_order_relaxed);
    g_zeroHeadPitch.store(headPitch, std::memory_order_relaxed);
    g_baselineYaw.store(baselineYaw, std::memory_order_relaxed);
    g_baselinePitch.store(baselinePitch, std::memory_order_relaxed);
    g_zeroHeadPosX.store(pose.positionX, std::memory_order_relaxed);
    g_zeroHeadPosY.store(pose.positionY, std::memory_order_relaxed);
    g_zeroHeadPosZ.store(pose.positionZ, std::memory_order_relaxed);
    g_haveRecenter.store(true, std::memory_order_relaxed);

    MOHW_LOG(kLogFile,
              "Recenter: zeroHead(yaw=%.4f pitch=%.4f) baseline(yaw=%.4f pitch=%.4f) source=%s", headYaw, headPitch,
              baselineYaw, baselinePitch, haveLastWritten ? "last-written" : "fresh-read(first-ever recenter)");
}

void ApplyHeadAim(void* aimingController)
{
    if (!GetHeadAimEnabled())
    {
        g_recenterRequested.store(true, std::memory_order_relaxed); // re-anchor fresh next time it's turned on
        return;
    }

    unsigned char* base = reinterpret_cast<unsigned char*>(aimingController);

    float currentYaw = 0.0f, currentPitch = 0.0f;
    if (!SehSafeReadFloat(&currentYaw, base + kYawByteOffset) || !SehSafeReadFloat(&currentPitch, base + kPitchByteOffset))
        return;
    if (!std::isfinite(currentYaw) || !std::isfinite(currentPitch))
        return;

    if (g_recenterRequested.exchange(false, std::memory_order_relaxed) || !g_haveRecenter.load(std::memory_order_relaxed))
    {
        Recenter(currentYaw, currentPitch);
        return; // no meaningful delta on the very same call as the anchor
    }

    mohwvr::ipc::HeadPoseBlock pose{};
    if (!GetHeadPose(&pose))
        return; // companion not running / no pose yet -- leave the field untouched this call

    Quat q{pose.orientationX, pose.orientationY, pose.orientationZ, pose.orientationW};
    float headYaw = 0.0f, headPitch = 0.0f;
    QuatToYawPitch(q, &headYaw, &headPitch);

    float sensitivity = GetHeadAimSensitivity();
    float yawDelta = WrapAngleSigned(headYaw - g_zeroHeadYaw.load(std::memory_order_relaxed)) * sensitivity;
    float pitchDelta = WrapAngleSigned(headPitch - g_zeroHeadPitch.load(std::memory_order_relaxed)) * sensitivity;
    if (GetHeadAimInvertYaw())
        yawDelta = -yawDelta;
    if (GetHeadAimInvertPitch())
        pitchDelta = -pitchDelta;

    float newYaw = WrapAngleUnsigned(g_baselineYaw.load(std::memory_order_relaxed) + yawDelta);
    float newPitch = g_baselinePitch.load(std::memory_order_relaxed) + pitchDelta;

    float clampMinDeg = kFallbackPitchClampMinDeg, clampMaxDeg = kFallbackPitchClampMaxDeg;
    if (GetHeadAimClampPitch())
    {
        // Respect AimingController's OWN pitch clamp, read live off the
        // same object rather than hardcoding -- falls back to the
        // live-observed ~-75/72 degree values if the read looks
        // implausible (e.g. a different AimingController layout than the
        // one this was derived from).
        float readMin = 0.0f, readMax = 0.0f;
        if (SehSafeReadFloat(&readMin, base + kPitchClampMinByteOffset) &&
            SehSafeReadFloat(&readMax, base + kPitchClampMaxByteOffset) && std::isfinite(readMin) &&
            std::isfinite(readMax) && readMin < 0.0f && readMax > 0.0f && readMin > -180.0f && readMax < 180.0f)
        {
            clampMinDeg = readMin;
            clampMaxDeg = readMax;
        }
        float clampMinRad = clampMinDeg * kDegToRad;
        float clampMaxRad = clampMaxDeg * kDegToRad;
        if (newPitch < clampMinRad)
            newPitch = clampMinRad;
        if (newPitch > clampMaxRad)
            newPitch = clampMaxRad;
    }

    if (!SehSafeWriteFloat(base + kYawByteOffset, newYaw) || !SehSafeWriteFloat(base + kPitchByteOffset, newPitch))
        return;

    g_lastWrittenYaw.store(newYaw, std::memory_order_relaxed);
    g_lastWrittenPitch.store(newPitch, std::memory_order_relaxed);
    g_haveLastWritten.store(true, std::memory_order_relaxed);

    static std::atomic<unsigned long long> nextLogAllowedMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogAllowedMs.load(std::memory_order_relaxed);
    if (now >= allowed && nextLogAllowedMs.compare_exchange_strong(allowed, now + 1000, std::memory_order_relaxed))
    {
        MOHW_LOG(kLogFile,
                  "head-aim applied: head(yaw=%.4f pitch=%.4f) delta(yaw=%.4f pitch=%.4f) -> AimingController now "
                  "(yaw=%.4f pitch=%.4f) clamp=%s[%.1f,%.1f]deg",
                  headYaw, headPitch, yawDelta, pitchDelta, newYaw, newPitch,
                  GetHeadAimClampPitch() ? "" : "(off) ", clampMinDeg, clampMaxDeg);
    }
}

// CONFIRMED signature (2026-08-21 live check, see sdk/mohw_offsets.h's
// OFFSET_AIMINGCONTROLLER_UPDATE comment): the function's own RET is
// `RET 0x94` -- ECX=this (implicit, not counted in the RET N total) plus
// 0x94=148 bytes (37 dwords) of real stack parameters, never individually
// decoded field-by-field. Rather than reverse-engineer all 37, this uses a
// byte-size-matching flat dummy blob: on x86 MSVC, a by-value struct
// parameter is ALWAYS pushed as a raw stack copy (never an
// invisible-reference/pointer, unlike x64 ABIs) for every convention used
// in this project, so a 37-uint32_t struct reliably compiles to exactly a
// 0x94-byte stack push and a matching `RET 0x94` cleanup, regardless of
// what the real 37 dwords' individual types are. Their contents are never
// read or interpreted here -- just forwarded byte-for-byte to g_original()
// untouched, so the original function receives input identical to what it
// always would have. This keeps ESP correctly balanced on every call, the
// exact stack-corruption class of bug that caused this project's only
// crash so far (OFFSET_INPUTACCUMULATOR, an underestimated stack-parameter
// count).
struct Passthrough37
{
    uint32_t dwords[37];
};
static_assert(sizeof(Passthrough37) == 0x94,
               "must match FUN_009DDA90's confirmed RET 0x94 stack-cleanup size exactly");

using AimingControllerUpdateFn = void(__thiscall*)(void*, Passthrough37);
AimingControllerUpdateFn g_original = nullptr;
void* g_hookAddress = nullptr;

// TEMPORARY DIAGNOSTIC (2026-08-23), not a real feature -- live-diagnosing
// head-turn-only jitter (mouse untouched, HeadAimEnabled=1, confirmed
// present in BOTH draw-duplication and alternating-eye) alongside visible
// character-mesh ghosting during rotation only, never during translation --
// a classic staleness signature. ApplyHeadAim writes new yaw/pitch once per
// call to THIS function, a GAMEPLAY-tick-driven update, not a render-driven
// one -- already confirmed CommitViewTransform keeps exact 1:1 pace with
// Present (see fov_scale_hook.cpp's own counter vs present_hook.cpp's), but
// never confirmed THIS function's own call rate against that same Present
// rate. If this fires meaningfully less often, that's a direct, measurable
// confirmation that the render side is reading a head-orientation-derived
// value that's stale for multiple render frames at a time between ticks --
// exactly what would produce visible lag/ghosting on anything posed
// relative to it during fast rotation. Same "#N every 300 calls" counter
// convention as present_hook.cpp/fov_scale_hook.cpp's own diagnostics, so
// their timestamps can be directly compared. Remove once this has answered
// the question.
std::atomic<long long> g_updateCalls{0};

struct ThisCallTrampoline
{
    void Hooked(Passthrough37 args)
    {
        void* self = this;
        // ALWAYS forward first, byte-identical args -- preserves every
        // other side effect this function has (aim-assist state, etc.);
        // we only want to override the two facing fields afterward, not
        // replace the function.
        g_original(self, args);

        long long n = ++g_updateCalls;
        if (n == 1 || n % 300 == 0)
            MOHW_LOG(kLogFile, "AimingControllerUpdate #%lld", n);

        // TRACK 2 DISABLE TEST (2026-09-25) RESULT: disabling ApplyHeadAim entirely (so g_haveRecenter/pose-stamp
        // never activate) produced correctly-fused stereo 3D with NO right-eye ghost -- BETTER than the baseline
        // with Track 2 active. Restored here to isolate pose-stamping specifically as the next suspect: see
        // openvr_direct.cpp's buildSubmitArgs, which is now forced to Submit_Default unconditionally instead.
        ApplyHeadAim(self);
    }
};

} // namespace

bool InstallAimingControllerHook()
{
    if (g_moduleBase == 0 || !g_buildVerified)
    {
        MOHW_LOG(kLogFile,
                  "InstallAimingControllerHook: module base not resolved / build not verified yet -- refusing to install");
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    g_hookAddress = Offset<void*>(OFFSET_AIMINGCONTROLLER_UPDATE);

    auto memberFn = &ThisCallTrampoline::Hooked;
    static_assert(sizeof(memberFn) == sizeof(void*),
                   "ThisCallTrampoline::Hooked's member-function-pointer isn't a plain code address -- "
                   "check for accidental virtual functions or multiple inheritance");
    void* detour = nullptr;
    memcpy(&detour, &memberFn, sizeof(detour));

    MH_STATUS s = MH_CreateHook(g_hookAddress, detour, reinterpret_cast<void**>(&g_original));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(AimingControllerUpdate @ %p) FAILED: %s", g_hookAddress, MH_StatusToString(s));
        g_hookAddress = nullptr;
        return false;
    }

    s = MH_EnableHook(g_hookAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook FAILED: %s", MH_StatusToString(s));
        return false;
    }

    g_recenterRequested.store(true, std::memory_order_relaxed);
    MOHW_LOG(kLogFile,
              "AimingController hook installed @ %p -- head-aim %s by default (see mohwvr_settings.ini, F12 to "
              "toggle, F3 to recenter)",
              g_hookAddress, GetHeadAimEnabled() ? "ENABLED" : "disabled");
    return true;
}

void RemoveAimingControllerHook()
{
    if (g_hookAddress)
    {
        MH_DisableHook(g_hookAddress);
        MH_RemoveHook(g_hookAddress);
        g_hookAddress = nullptr;
    }
    // MH_Uninitialize() is called once centrally from dllmain.cpp.
}

namespace {
constexpr int kHeadAimToggleHotkey = VK_F12;
constexpr int kHeadAimSensitivityDownHotkey = VK_F1;
constexpr int kHeadAimSensitivityUpHotkey = VK_F2;
constexpr int kRecenterHotkey = VK_F3;
constexpr int kInvertYawHotkey = VK_F4;
// F1 reused (2026-08-22) -- freed up by temporarily disabling
// commit_view_transform_hook.cpp's install + hotkey poll (see
// dllmain.cpp/present_hook.cpp) to isolate this hook for testing.
// HeadAimClampPitch (sdk/settings.h) still has no dedicated hotkey --
// edit mohwvr_settings.ini directly and relaunch to change it.
constexpr int kInvertPitchHotkey = VK_F1;
constexpr float kHeadAimSensitivityStep = 0.25f;
constexpr float kHeadAimSensitivityMin = 0.0f;
} // namespace

void CheckAimingControllerHotkeys()
{
    // F12 toggle and F1/F2 sensitivity step TEMPORARILY DISABLED (2026-08-22,
    // user request) -- repeated F1 presses during testing silently walked
    // HeadAimSensitivity down to 0.0 (the allowed floor), which persisted to
    // mohwvr_settings.ini and made head-aim look completely dead on a later
    // session despite the toggle/recenter/enable logic all working
    // correctly (real bug, real diagnosis, see project memory) -- fixed by
    // hand-editing the settings file back to 1.0. Narrowing the live hotkey
    // surface to just recenter (F3) and yaw-invert (F4) while validating the
    // axis convention removes that whole failure mode for now.
    // HeadAimEnabled/HeadAimSensitivity still work exactly as before via
    // mohwvr_settings.ini (hand-edit + relaunch) -- only these two hotkeys'
    // LIVE polling is disabled, re-enable by uncommenting below.
#if 0
    // Same edge-detected repeatable-toggle pattern as draw_duplication_hook.cpp's
    // CheckStereoDebugHotkeys -- fire once per press, not once per frame held down.
    static bool toggleKeyWasDown = false;
    bool toggleKeyDown = (GetAsyncKeyState(kHeadAimToggleHotkey) & 0x8000) != 0;
    if (toggleKeyDown && !toggleKeyWasDown)
    {
        bool newValue = !GetHeadAimEnabled();
        SetHeadAimEnabled(newValue);
        if (newValue)
            g_recenterRequested.store(true, std::memory_order_relaxed); // fresh anchor every time it's turned on
        MOHW_LOG(kLogFile, "F12 pressed -- head-driven facing/aim now %s (saved to mohwvr_settings.ini)",
                  newValue ? "ENABLED" : "disabled");
    }
    toggleKeyWasDown = toggleKeyDown;

    static bool sensDownKeyWasDown = false;
    bool sensDownKeyDown = (GetAsyncKeyState(kHeadAimSensitivityDownHotkey) & 0x8000) != 0;
    if (sensDownKeyDown && !sensDownKeyWasDown)
    {
        float newValue = GetHeadAimSensitivity() - kHeadAimSensitivityStep;
        if (newValue < kHeadAimSensitivityMin)
            newValue = kHeadAimSensitivityMin;
        SetHeadAimSensitivity(newValue);
        MOHW_LOG(kLogFile, "F1 pressed -- head-aim sensitivity now %.2f (saved to mohwvr_settings.ini)", newValue);
    }
    sensDownKeyWasDown = sensDownKeyDown;

    static bool sensUpKeyWasDown = false;
    bool sensUpKeyDown = (GetAsyncKeyState(kHeadAimSensitivityUpHotkey) & 0x8000) != 0;
    if (sensUpKeyDown && !sensUpKeyWasDown)
    {
        float newValue = GetHeadAimSensitivity() + kHeadAimSensitivityStep;
        SetHeadAimSensitivity(newValue);
        MOHW_LOG(kLogFile, "F2 pressed -- head-aim sensitivity now %.2f (saved to mohwvr_settings.ini)", newValue);
    }
    sensUpKeyWasDown = sensUpKeyDown;
#endif

    static bool recenterKeyWasDown = false;
    bool recenterKeyDown = (GetAsyncKeyState(kRecenterHotkey) & 0x8000) != 0;
    if (recenterKeyDown && !recenterKeyWasDown)
    {
        g_recenterRequested.store(true, std::memory_order_relaxed);
        MOHW_LOG(kLogFile, "F3 pressed -- head-aim will recenter on the next applicable frame");
    }
    recenterKeyWasDown = recenterKeyDown;

    static bool invertYawKeyWasDown = false;
    bool invertYawKeyDown = (GetAsyncKeyState(kInvertYawHotkey) & 0x8000) != 0;
    if (invertYawKeyDown && !invertYawKeyWasDown)
    {
        bool newValue = !GetHeadAimInvertYaw();
        SetHeadAimInvertYaw(newValue);
        MOHW_LOG(kLogFile, "F4 pressed -- head-aim yaw inversion now %s (saved to mohwvr_settings.ini)",
                  newValue ? "ON" : "off");
    }
    invertYawKeyWasDown = invertYawKeyDown;

    static bool invertPitchKeyWasDown = false;
    bool invertPitchKeyDown = (GetAsyncKeyState(kInvertPitchHotkey) & 0x8000) != 0;
    if (invertPitchKeyDown && !invertPitchKeyWasDown)
    {
        bool newValue = !GetHeadAimInvertPitch();
        SetHeadAimInvertPitch(newValue);
        MOHW_LOG(kLogFile, "F1 pressed -- head-aim pitch inversion now %s (saved to mohwvr_settings.ini)",
                  newValue ? "ON" : "off");
    }
    invertPitchKeyWasDown = invertPitchKeyDown;

    // HeadAimClampPitch (sdk/settings.h) still has no dedicated hotkey --
    // edit mohwvr_settings.ini directly and relaunch to change it.
}

bool GetLastAppliedYawPitch(float* outYaw, float* outPitch)
{
    if (!g_haveLastWritten.load(std::memory_order_relaxed))
        return false;
    *outYaw = g_lastWrittenYaw.load(std::memory_order_relaxed);
    *outPitch = g_lastWrittenPitch.load(std::memory_order_relaxed);
    return true;
}

bool GetHeadAimPositionOrigin(float out[3])
{
    if (!g_haveRecenter.load(std::memory_order_relaxed))
        return false;
    out[0] = g_zeroHeadPosX.load(std::memory_order_relaxed);
    out[1] = g_zeroHeadPosY.load(std::memory_order_relaxed);
    out[2] = g_zeroHeadPosZ.load(std::memory_order_relaxed);
    return true;
}

bool GetHeadAimMapping(float* zeroHeadYaw, float* zeroHeadPitch, float* baselineYaw, float* baselinePitch)
{
    if (!g_haveRecenter.load(std::memory_order_relaxed))
        return false;
    *zeroHeadYaw = g_zeroHeadYaw.load(std::memory_order_relaxed);
    *zeroHeadPitch = g_zeroHeadPitch.load(std::memory_order_relaxed);
    *baselineYaw = g_baselineYaw.load(std::memory_order_relaxed);
    *baselinePitch = g_baselinePitch.load(std::memory_order_relaxed);
    return true;
}

} // namespace mohw
