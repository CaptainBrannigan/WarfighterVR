#include "fov_scale_hook.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"
#include "../sdk/vr_math.h"
#include "aiming_controller_hook.h"
#include "companion_bridge.h"
#include "alternating_eye.h"
#include "eye_matched_fov.h"
#include "render_pose_stamp.h"
#include "head_position.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <atomic>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_fovscale.log";

// Isolated, no C++ objects requiring unwinding in the frame -- same
// __try/__except-can't-coexist-with-object-unwinding constraint as every
// other SEH-safe helper in this project.
bool SehSafeReadFloats(float* dst, const void* src, int count)
{
    __try
    {
        memcpy(dst, src, static_cast<size_t>(count) * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SehSafeWriteFloats(void* dst, const float* src, int count)
{
    __try
    {
        memcpy(dst, src, static_cast<size_t>(count) * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Throttled degree-level telemetry (moved verbatim from
// commit_view_transform_hook.cpp) -- logs the ACTUAL native and scaled
// fovY, read live off the buffer every call, in both radians and degrees.
constexpr float kRadToDeg = 57.29577951f; // 180/pi
void LogFovIfDue(float fovYRad, float newFovYRad, bool clamped)
{
    static std::atomic<unsigned long long> nextFovLogAllowedMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextFovLogAllowedMs.load(std::memory_order_relaxed);
    if (now < allowed)
        return;
    if (!nextFovLogAllowedMs.compare_exchange_strong(allowed, now + 2000, std::memory_order_relaxed))
        return; // another thread already claimed this window

    MOHW_LOG(kLogFile,
              "FOV now: native=%.4f rad (%.1f deg) scale=%.2fx -> scaled=%.4f rad (%.1f deg)%s", fovYRad,
              fovYRad * kRadToDeg, GetFovScale(), newFovYRad, newFovYRad * kRadToDeg,
              clamped ? " [CLAMPED]" : "");
}

bool ScaleFovYAt(unsigned char* fovYPtr)
{
    float fovY = 0.0f;
    if (!SehSafeReadFloats(&fovY, fovYPtr, 1))
        return false;
    if (fovY <= 0.0f || fovY >= 3.14159265f) // sanity check -- reject garbage/uninitialized reads rather than writing something worse back
        return false;

    float newFovY = fovY * GetFovScale();
    bool clamped = newFovY >= 3.05f;
    if (clamped) // clamp -- a fovY at/near pi breaks the tan()-based projection math downstream (division blows up)
        newFovY = 3.05f;

    LogFovIfDue(fovY, newFovY, clamped);

    return SehSafeWriteFloats(fovYPtr, &newFovY, 1);
}

// RenderViewDesc::fovY sits at byte offset 0x48 from the transform pointer
// (sdk/renderview.h: transform at 0x00 [0x40 bytes], type at 0x40,
// PAD(0x4), fovY at 0x48) -- widens the game's own 3D-world field of
// view. Does NOT touch the transform's left/up/forward/trans fields at
// all -- rotation/position are left completely alone, now solely the
// native AimingController-to-camera sync's responsibility (see this
// file's header comment).
bool ApplyFovScale(void* transformPtr)
{
    unsigned char* base = reinterpret_cast<unsigned char*>(transformPtr);
    return ScaleFovYAt(base + 0x48);
}

std::atomic<int> g_logsWritten{0};
std::atomic<unsigned long long> g_nextLogAllowedMs{0};

void LogIfDue(void* param2, bool applied)
{
    if (g_logsWritten.load() >= 100)
        return;
    unsigned long long now = GetTickCount64();
    if (now < g_nextLogAllowedMs.load())
        return;
    g_nextLogAllowedMs.store(now + 500);
    ++g_logsWritten;

    MOHW_LOG(kLogFile, "FOV hook fired: param2=%p applied=%d", param2, applied);
}

// TEMPORARY DIAGNOSTIC (2026-08-22), not a real feature -- READ-ONLY,
// never writes anything. Added to this ALREADY-installed hook (rather
// than a separate one) because MinHook doesn't support two independent
// hooks stacked on the same target address, and this file already hooks
// OFFSET_COMMITVIEWTRANSFORM.
//
// Question: does this injection point fire BEFORE or AFTER the game's
// native AimingController-to-camera sync each tick (see
// project_mohw_yaw_facing_investigation.md's "MAJOR ARCHITECTURE
// FINDING")? Matters for a prospective roll-only render patch -- if the
// incoming transform's forward vector already reflects AimingController's
// current yaw/pitch, adding roll on top here is safe; if it fires before
// that sync, a write here risks the same fighting-over-the-same-field
// problem the now-retired full-rotation hook had. Compares the transform's
// own extracted yaw/pitch (same atan2(x,z)/asin(y) formula as
// sdk/vr_math.h's QuatToYawPitch, applied to a vector already in hand) with
// aiming_controller_hook.h's GetLastAppliedYawPitch(). Remove once this has
// answered the question.
bool SehSafeReadFloat3(float* dst, const void* src)
{
    __try
    {
        memcpy(dst, src, 3 * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void LogOrderingComparisonIfDue(void* transformPtr)
{
    static std::atomic<unsigned long long> nextLogAllowedMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogAllowedMs.load(std::memory_order_relaxed);
    if (now < allowed)
        return;
    if (!nextLogAllowedMs.compare_exchange_strong(allowed, now + 1000, std::memory_order_relaxed))
        return;

    // Forward row is float index 8 (byte offset 0x20) in this transform
    // layout -- same as the retired commit_view_transform_hook.cpp.
    float forward[3]{};
    if (!SehSafeReadFloat3(forward, reinterpret_cast<unsigned char*>(transformPtr) + 0x20))
        return;

    float transformYaw = atan2f(forward[0], forward[2]);
    float clampedY = forward[1];
    if (clampedY > 1.0f)
        clampedY = 1.0f;
    if (clampedY < -1.0f)
        clampedY = -1.0f;
    float transformPitch = asinf(clampedY);

    float appliedYaw = 0.0f, appliedPitch = 0.0f;
    if (GetLastAppliedYawPitch(&appliedYaw, &appliedPitch))
        MOHW_LOG(kLogFile,
                  "ORDERING PROBE: transform(yaw=%.4f pitch=%.4f) vs AimingController-last-applied(yaw=%.4f "
                  "pitch=%.4f) diff(yaw=%.4f pitch=%.4f)",
                  transformYaw, transformPitch, appliedYaw, appliedPitch, transformYaw - appliedYaw,
                  transformPitch - appliedPitch);
    else
        MOHW_LOG(kLogFile,
                  "ORDERING PROBE: transform(yaw=%.4f pitch=%.4f) -- AimingController hasn't written anything yet "
                  "(head-aim off?)",
                  transformYaw, transformPitch);
}

// Camera rotation SMOOTHING (2026-08-23) -- live-diagnosed fix, see
// sdk/settings.h's GetRotationSmoothingEnabled declaration comment for the
// full root-cause writeup (MOHW's own gameplay tick confirmed live at a
// fixed ~30Hz via aiming_controller_hook.cpp's AimingControllerUpdate
// counter, vs this hook's own ~75-90Hz CommitViewTransform rate -- a hard
// staircase in the native camera sync's output). Extracts yaw/pitch from
// the incoming transform (same atan2/asin technique as
// LogOrderingComparisonIfDue above, already trusted), detects when it's
// actually CHANGED since the last-seen sample (a new 30Hz tick landed) vs.
// still showing the previous tick's frozen value, and when it changes,
// interpolates the OUTPUT smoothly from the previous target to the new one
// over an assumed ~33.3ms (1/30s) window instead of snapping. Runs FIRST
// in this hook's pipeline (before ApplyHeadRoll) so roll's own already-
// smooth, continuously-sampled effect layers on top of the smoothed base
// rather than being smoothed itself (it doesn't need it -- see
// ApplyHeadRoll's own comment), and long before alternating-eye's
// FreezeRotationForPair/ApplyEyeOffset, which need to see the final result
// same as everything else downstream.
//
// Yaw/pitch-only reconstruction (discards any native roll) -- same
// accepted tradeoff as the retired commit_view_transform_hook.cpp's
// MakeYawPitchDelta, whose own comment already established this project's
// position on it: AimingController has no roll concept at all, so there's
// nothing meaningful to lose. The reconstruction quaternion (quatYaw
// composed with quatPitch, applied to canonical local axes) is the SAME
// pattern MakeYawPitchDelta already used and this project already trusts
// as the inverse of QuatToYawPitch/this file's own atan2+asin extraction --
// not a fresh, unverified formula.
void LogSmoothingIfDue(float rawYaw, float rawPitch, float interpYaw, float interpPitch, float t, float windowMs)
{
    static std::atomic<unsigned long long> nextLogAllowedMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogAllowedMs.load(std::memory_order_relaxed);
    if (now >= allowed && nextLogAllowedMs.compare_exchange_strong(allowed, now + 1000, std::memory_order_relaxed))
        MOHW_LOG(kLogFile,
                  "rotation smoothing: raw(yaw=%.4f pitch=%.4f) interp(yaw=%.4f pitch=%.4f) t=%.2f windowMs=%.2f",
                  rawYaw, rawPitch, interpYaw, interpPitch, t, windowMs);
}

// See GetRenderedHeadYawOffset's declaration comment. Written by SmoothCameraRotation, read (same thread, right after,
// via ComputeRenderPoseStamp) through GetRenderedHeadYawOffset.
std::atomic<float> g_renderedHeadYawOffset{0.0f};
std::atomic<bool> g_haveRenderedHeadYawOffset{false};

bool SmoothCameraRotation(void* transformPtr)
{
    g_haveRenderedHeadYawOffset.store(false, std::memory_order_relaxed);
    if (!GetRotationSmoothingEnabled())
        return false;

    float raw[12]{};
    if (!SehSafeReadFloats(raw, transformPtr, 12))
        return false;

    Vec3 forward{};
    forward.x = raw[8];
    forward.y = raw[9];
    forward.z = raw[10];

    float lenSq = VecDot(forward, forward);
    if (lenSq < 0.5f || lenSq > 2.0f) // sanity check, same philosophy as ApplyHeadRoll's -- reject a non-camera transform
        return false;

    float clampedY = forward.y;
    if (clampedY > 1.0f)
        clampedY = 1.0f;
    if (clampedY < -1.0f)
        clampedY = -1.0f;
    float rawYaw = atan2f(forward.x, forward.z);
    float rawPitch = asinf(clampedY);

    // PAIR FREEZE, RESTORED (2026-09-25): the original design (per ipc_protocol.h/render_pose_stamp.h's own
    // "smoothing + pair freeze" comments, and the 2026-09-21 bug-fix comment describing "FreezeRotationForPair")
    // always made the right eye reuse the LEFT eye's already-smoothed rotation, guaranteeing both eyes agree.
    // That function no longer exists anywhere in the codebase (confirmed via a full grep) -- at some point it was
    // removed without the callers/comments describing it being updated, leaving each eye smoothing fully
    // independently with nothing reconciling them. Diagnosed as the cause of a rotation-only jitter (present at
    // every window size and with head-roll off) that raw-sample logging showed was NOT noise in either eye's own
    // data -- consistent with two individually-smooth but mutually-unsynced per-eye trajectories drifting apart
    // by small amounts, which no per-eye window tuning could ever fix.
    //
    // Reimplemented here rather than as a separate post-step: only the LEFT eye runs real tick-detection/
    // interpolation (single, non-indexed state -- also sidesteps the ORIGINAL pre-2026-09-21 bug, since the
    // right eye's raw sample never touches this state at all now); the right eye just reuses left's last
    // computed result. Falls through (returns false, transform untouched) if no left value exists yet.
    bool rightEye = IsRightEyeActive();
    static bool haveLeftValue = false;
    static float lastLeftInterpYaw = 0.0f, lastLeftInterpPitch = 0.0f, lastLeftHeadYawOffset = 0.0f;
    float interpYaw, interpPitch;

    if (rightEye)
    {
        if (!haveLeftValue)
            return false;
        interpYaw = lastLeftInterpYaw;
        interpPitch = lastLeftInterpPitch;
        g_renderedHeadYawOffset.store(lastLeftHeadYawOffset, std::memory_order_relaxed);
        g_haveRenderedHeadYawOffset.store(true, std::memory_order_relaxed);
    }
    else
    {
        // CONTINUOUS LOW-PASS FILTER (2026-09-25), REPLACING TICK-DETECTION: the old design assumed a discrete
        // 30Hz staircase to interpolate between ticks -- but logging showed its own `t` pinned near 0.19
        // regardless of window size, meaning nearly every call was being treated as "a new tick," so the
        // interpolation never actually progressed. Independently confirmed the same night: native MOUSE-driven
        // rotation (zero mod code involved before our own AimingController overwrite even runs) shows the
        // identical jitter signature as head-tracked rotation -- meaning the raw signal reaching this function
        // was never a clean staircase this design could smooth in the first place. Replaced with a standard
        // frame-rate-independent exponential filter: no tick detection, no reset, a continuous blend toward the
        // raw value every call, with blend strength derived from real elapsed time via QueryPerformanceCounter
        // (GetTickCount64's confirmed ~15.6ms resolution is too coarse for a sub-frame filter). Still reuses
        // RotationSmoothingWindowMs as the tuning knob -- now the filter's time constant in ms, not an assumed
        // tick interval.
        static bool haveSample = false;
        static float smoothYaw = 0.0f, smoothPitch = 0.0f;
        // The head-driven part of the yaw, filtered identically. The filter is linear, so smoothing the head
        // offset on its own gives exactly the head's share of the smoothed yaw, with mouse/stick turning left out.
        static float smoothHeadYawOffset = 0.0f;
        float rawHeadYawOffset = GetHeadYawOffset();
        static LARGE_INTEGER lastCallQpc{};
        static LARGE_INTEGER qpcFreq{};
        static bool haveQpcFreq = false;
        if (!haveQpcFreq)
        {
            QueryPerformanceFrequency(&qpcFreq);
            haveQpcFreq = true;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        if (!haveSample)
        {
            smoothYaw = rawYaw;
            smoothPitch = rawPitch;
            smoothHeadYawOffset = rawHeadYawOffset;
            lastCallQpc = now;
            haveSample = true;
            return false; // nothing to blend from yet, leave untouched
        }

        double dtSeconds = static_cast<double>(now.QuadPart - lastCallQpc.QuadPart) / static_cast<double>(qpcFreq.QuadPart);
        lastCallQpc = now;
        if (dtSeconds < 0.0)
            dtSeconds = 0.0;

        float tauMs = GetRotationSmoothingWindowMs();
        if (tauMs < 1.0f) // guard against div-by-zero/negative from a bad manual ini edit
            tauMs = 1.0f;
        double tauSeconds = static_cast<double>(tauMs) / 1000.0;

        // Standard frame-rate-independent exponential smoothing: alpha = 1 - e^(-dt/tau). A fast call (small dt)
        // barely moves toward raw this call; a slow call (large dt, e.g. after a stall) snaps close to raw
        // rather than lagging forever.
        float alpha = static_cast<float>(1.0 - exp(-dtSeconds / tauSeconds));
        if (alpha < 0.0f)
            alpha = 0.0f;
        if (alpha > 1.0f)
            alpha = 1.0f;

        smoothYaw = smoothYaw + WrapAngleSigned(rawYaw - smoothYaw) * alpha;
        smoothPitch = smoothPitch + (rawPitch - smoothPitch) * alpha;
        smoothHeadYawOffset = WrapAngleSigned(smoothHeadYawOffset +
                                              WrapAngleSigned(rawHeadYawOffset - smoothHeadYawOffset) * alpha);

        interpYaw = smoothYaw;
        interpPitch = smoothPitch;

        LogSmoothingIfDue(rawYaw, rawPitch, interpYaw, interpPitch, alpha, tauMs);

        lastLeftInterpYaw = interpYaw;
        lastLeftInterpPitch = interpPitch;
        lastLeftHeadYawOffset = smoothHeadYawOffset;
        haveLeftValue = true;
        g_renderedHeadYawOffset.store(smoothHeadYawOffset, std::memory_order_relaxed);
        g_haveRenderedHeadYawOffset.store(true, std::memory_order_relaxed);
    }

    // NEGATED vs. the retired commit_view_transform_hook.cpp's
    // MakeYawPitchDelta, which this was otherwise copied from -- confirmed
    // live (2026-08-23) that its pitch sign was backwards for THIS
    // extraction convention (pitch=asin(forward.y)): rotating localForward
    // by an UNnegated quatPitch{sin(p/2),0,0,cos(p/2)} algebraically
    // produces forward=(0,-sin(p),cos(p)), i.e. positive pitch (look up,
    // forward.y>0 by this file's own convention) yields forward.y<0 --
    // inverted. That retired file shipped its own g_renderInvertPitch
    // hotkey specifically because this sign was never actually confirmed
    // there either, just carried forward unverified -- worth remembering
    // next time this pattern gets reused elsewhere.
    Quat quatYaw{0.0f, sinf(interpYaw * 0.5f), 0.0f, cosf(interpYaw * 0.5f)};
    Quat quatPitch{-sinf(interpPitch * 0.5f), 0.0f, 0.0f, cosf(interpPitch * 0.5f)};
    Quat rotation = QuatMultiply(quatYaw, quatPitch);

    Vec3 localLeft{};
    localLeft.x = 1.0f;
    Vec3 localUp{};
    localUp.y = 1.0f;
    Vec3 localForward{};
    localForward.z = 1.0f;

    Vec3 newLeft = QuatRotateVector(rotation, localLeft);
    Vec3 newUp = QuatRotateVector(rotation, localUp);
    Vec3 newForward = QuatRotateVector(rotation, localForward);

    raw[0] = newLeft.x;
    raw[1] = newLeft.y;
    raw[2] = newLeft.z;
    raw[4] = newUp.x;
    raw[5] = newUp.y;
    raw[6] = newUp.z;
    raw[8] = newForward.x;
    raw[9] = newForward.y;
    raw[10] = newForward.z;

    return SehSafeWriteFloats(transformPtr, raw, 12);
}

// Head ROLL (2026-08-22) -- reads the current transform's left/up/forward
// (already correctly yaw/pitched by the game's native AimingController
// sync, confirmed safe by the ORDERING PROBE above), applies an ABSOLUTE
// roll rotation extracted directly from real-time HMD orientation (see
// sdk/vr_math.h's ExtractRoll -- relative to world-level, no recenter
// dependency, unlike yaw/pitch), and writes back ONLY left/up. Forward and
// trans are left completely untouched -- pure roll around the forward axis
// leaves forward mathematically unchanged, which doubles as a correctness
// check on this math (if forward visibly drifted, something would be
// wrong). Independent of and additive on top of the native yaw/pitch sync,
// not competing with it -- see the ordering probe's own comment for why
// this injection point is safe for exactly this kind of patch.
void LogRollIfDue(bool rightEye, float rollAngle, const Vec3& left, const Vec3& up, const Vec3& forward)
{
    // EYE-TAGGED (2026-09-24): part of a 3-way simultaneous comparison (head-roll, head-position, eye-offset)
    // to check for a per-eye asymmetry the persistent right-eye-only ghost investigation hasn't explained yet
    // -- see alternating_eye.cpp's LogOffsetIfDue and head_position.cpp's own log for the other two.
    static std::atomic<unsigned long long> nextLogAllowedMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogAllowedMs.load(std::memory_order_relaxed);
    if (now >= allowed && nextLogAllowedMs.compare_exchange_strong(allowed, now + 1000, std::memory_order_relaxed))
        MOHW_LOG(kLogFile,
                  "head-roll applied: eye=%s rollAngle=%.4f rad (%.1f deg) left=(%.3f,%.3f,%.3f) up=(%.3f,%.3f,%.3f) "
                  "forward=(%.3f,%.3f,%.3f)",
                  rightEye ? "RIGHT" : "LEFT", rollAngle, rollAngle * 57.29577951f, left.x, left.y, left.z, up.x, up.y,
                  up.z, forward.x, forward.y, forward.z);
}

std::atomic<float> g_appliedHeadRoll{0.0f}; // see GetAppliedHeadRoll

bool ApplyHeadRoll(void* transformPtr)
{
    if (!GetHeadRollEnabled())
    {
        g_appliedHeadRoll.store(0.0f, std::memory_order_relaxed);
        return false;
    }

    mohwvr::ipc::HeadPoseBlock pose{};
    if (!GetHeadPose(&pose))
    {
        g_appliedHeadRoll.store(0.0f, std::memory_order_relaxed);
        return false;
    }

    Quat current{pose.orientationX, pose.orientationY, pose.orientationZ, pose.orientationW};
    float rollAngle = ExtractRoll(current);
    if (GetHeadRollInvert())
        rollAngle = -rollAngle;
    g_appliedHeadRoll.store(rollAngle, std::memory_order_relaxed);

    float raw[16]{};
    if (!SehSafeReadFloats(raw, transformPtr, 16))
        return false;

    Vec3 left{};
    left.x = raw[0];
    left.y = raw[1];
    left.z = raw[2];
    Vec3 up{};
    up.x = raw[4];
    up.y = raw[5];
    up.z = raw[6];
    Vec3 forward{};
    forward.x = raw[8];
    forward.y = raw[9];
    forward.z = raw[10];

    // Sanity check, same philosophy as the retired commit_view_transform_hook.cpp's
    // rejection check -- reject rather than corrupt a non-camera transform.
    auto isUnitLength = [](const Vec3& v) {
        float lenSq = VecDot(v, v);
        return lenSq > 0.5f && lenSq < 2.0f;
    };
    if (!isUnitLength(left) || !isUnitLength(up) || !isUnitLength(forward))
        return false;

    // Pure rotation about local Z (forward, per this project's established
    // localForward.z=1 convention) -- by definition, rolling doesn't
    // change where forward points.
    Quat rollDelta{0.0f, 0.0f, sinf(rollAngle * 0.5f), cosf(rollAngle * 0.5f)};

    Vec3 localLeft{};
    localLeft.x = 1.0f;
    Vec3 localUp{};
    localUp.y = 1.0f;

    Vec3 rotLeft = QuatRotateVector(rollDelta, localLeft);
    Vec3 rotUp = QuatRotateVector(rollDelta, localUp);

    Vec3 leftR = VecAdd(VecAdd(VecScale(left, rotLeft.x), VecScale(up, rotLeft.y)), VecScale(forward, rotLeft.z));
    Vec3 upR = VecAdd(VecAdd(VecScale(left, rotUp.x), VecScale(up, rotUp.y)), VecScale(forward, rotUp.z));

    raw[0] = leftR.x;
    raw[1] = leftR.y;
    raw[2] = leftR.z;
    raw[4] = upR.x;
    raw[5] = upR.y;
    raw[6] = upR.z;

    LogRollIfDue(IsRightEyeActive(), rollAngle, leftR, upR, forward);

    return SehSafeWriteFloats(transformPtr, raw, 8); // only left+up rows (floats 0-7), forward/trans untouched
}

// CONFIRMED signature (read straight out of a Ghidra decompile, unlike
// OFFSET_AIMINGCONTROLLER_UPDATE's -- see sdk/mohw_offsets.h's
// OFFSET_COMMITVIEWTRANSFORM comment): __thiscall, param_1 (ECX) =
// GameRenderer-relative "this", param_2 (one stack arg) = the transform
// pointer.
using CommitViewTransformFn = void(__thiscall*)(int param1, void* param2);
CommitViewTransformFn g_original = nullptr;
void* g_hookAddress = nullptr;

// TEMPORARY DIAGNOSTIC (2026-08-22) -- raw, uncapped call counter, same
// "#N every 300 calls" convention as present_hook.cpp's own Present
// counter, specifically so the two logs' timestamps can be directly
// compared to check for a simulation-tick-vs-Present rate mismatch (a
// classic VR judder cause: if this fires less often than Present, multiple
// presented frames show the identical stale camera orientation before the
// next update). Remove once that's answered.
std::atomic<long long> g_commitCalls{0};

struct ThisCallTrampoline
{
    void Hooked(void* param2)
    {
        int param1 = reinterpret_cast<int>(this);

        long long n = ++g_commitCalls;
        if (n == 1 || n % 300 == 0)
            MOHW_LOG(kLogFile, "CommitViewTransform #%lld", n);

        bool applied = false;
        if (param2 != nullptr)
        {
            // SWAP POINT (2026-09-20): the eye-matched method (hooks/eye_matched_fov.cpp)
            // sets fovY from the headset's real per-eye frustum. The old method is
            // ApplyFovScale(param2) (FovScale multiplier, still defined above) -- swap back
            // by changing this one call (and the matching #if in companion/main.cpp).
            applied = ApplyEyeMatchedFov(param2);
            LogOrderingComparisonIfDue(param2); // TEMPORARY DIAGNOSTIC, read-only
            // No-op unless RotationSmoothingEnabled is on -- see its own
            // declaration comment. Runs BEFORE head-roll so roll's already-
            // smooth effect layers on top of the smoothed base.
            SmoothCameraRotation(param2);
            ApplyHeadRoll(param2); // additive on top of the native yaw/pitch sync, see its own comment
            // EXPERIMENTAL head position (hooks/head_position.h): moves the camera translation by the real head delta since recenter.
            ApplyHeadPosition(param2);
            ApplyEyeOffset(param2);
            // EXPERIMENTAL render-pose stamp (hooks/render_pose_stamp.h): the camera rotation is final here.
            ComputeRenderPoseStamp(param2);
        }

        LogIfDue(param2, applied);

        g_original(param1, param2);
    }
};

} // namespace

bool InstallFovScaleHook()
{
    if (g_moduleBase == 0 || !g_buildVerified)
    {
        MOHW_LOG(kLogFile, "InstallFovScaleHook: module base not resolved / build not verified yet -- refusing to install");
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    g_hookAddress = Offset<void*>(OFFSET_COMMITVIEWTRANSFORM);

    auto memberFn = &ThisCallTrampoline::Hooked;
    static_assert(sizeof(memberFn) == sizeof(void*),
                   "ThisCallTrampoline::Hooked's member-function-pointer isn't a plain code address -- "
                   "check for accidental virtual functions or multiple inheritance");
    void* detour = nullptr;
    memcpy(&detour, &memberFn, sizeof(detour));

    MH_STATUS s = MH_CreateHook(g_hookAddress, detour, reinterpret_cast<void**>(&g_original));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(FovScale @ %p) FAILED: %s", g_hookAddress, MH_StatusToString(s));
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
              "FovScale hook installed @ %p -- FOV + head-roll (%s, F2 to toggle) + alternating-eye offset "
              "(always on) + rotation smoothing (%s, HOME to toggle) + ordering probe, "
              "yaw/pitch untouched",
              g_hookAddress, GetHeadRollEnabled() ? "ENABLED" : "disabled",
              GetRotationSmoothingEnabled() ? "ENABLED" : "disabled");
    return true;
}

void RemoveFovScaleHook()
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
constexpr int kFovScaleDownHotkey = VK_F5;
constexpr int kFovScaleUpHotkey = VK_F6;
constexpr float kFovScaleStep = 0.2f;
constexpr float kFovScaleMin = 1.0f; // below 1x the game's own native FOV, no reason to go smaller
// F2/F12 reused -- both free (F2 was head-aim sensitivity, F12 was render
// test-pitch, both retired/disabled elsewhere -- see
// aiming_controller_hook.cpp's own hotkey comments).
constexpr int kHeadRollToggleHotkey = VK_F2;
constexpr int kHeadRollInvertHotkey = VK_F12;
// F1-F12 all claimed elsewhere, VK_INSERT claimed by alternating_eye.cpp's
// own toggle -- VK_HOME is unused and, like VK_INSERT, very unlikely to
// double as a real gameplay bind for a first-person shooter.
constexpr int kRotationSmoothingToggleHotkey = VK_HOME;
// PageUp/PageDown -- both unused elsewhere, same reasoning as VK_HOME.
constexpr int kRotationSmoothingWindowUpHotkey = VK_PRIOR;   // PageUp
constexpr int kRotationSmoothingWindowDownHotkey = VK_NEXT;  // PageDown
constexpr float kRotationSmoothingWindowStepMs = 2.0f;
constexpr float kRotationSmoothingWindowMinMs = 0.0f;   // 0 = no smoothing window, snaps instantly (degenerate but harmless)
constexpr float kRotationSmoothingWindowMaxMs = 200.0f; // beyond this it's just perceived input lag, not smoothing
} // namespace

void CheckFovScaleHotkeys()
{
    // Same edge-detected repeatable-toggle pattern as draw_duplication_hook.cpp's
    // CheckStereoDebugHotkeys -- fire once per press, not once per frame held down.
    // In the true-per-eye-frustum view mode F5/F6 tune FrustumScale (0.1 steps) instead of FovScale.
    mohwvr::ipc::HmdViewBlock hv{};
    const bool trueFrustumMode = GetHmdView(&hv) && hv.viewMode == mohwvr::ipc::kViewModeTrueFrustum;

    static bool downKeyWasDown = false;
    bool downKeyDown = (GetAsyncKeyState(kFovScaleDownHotkey) & 0x8000) != 0;
    if (downKeyDown && !downKeyWasDown)
    {
        if (trueFrustumMode)
        {
            float k = GetFrustumScale() - 0.1f;
            if (k < 0.5f)
                k = 0.5f;
            SetFrustumScale(k);
            MOHW_LOG(kLogFile, "F5 pressed -- FrustumScale now %.2f (saved to mohwvr_settings.ini)", k);
        }
        else
        {
            float newScale = GetFovScale() - kFovScaleStep;
            if (newScale < kFovScaleMin)
                newScale = kFovScaleMin;
            SetFovScale(newScale);
            MOHW_LOG(kLogFile, "F5 pressed -- FOV scale now %.2fx (saved to mohwvr_settings.ini)", newScale);
        }
    }
    downKeyWasDown = downKeyDown;

    static bool upKeyWasDown = false;
    bool upKeyDown = (GetAsyncKeyState(kFovScaleUpHotkey) & 0x8000) != 0;
    if (upKeyDown && !upKeyWasDown)
    {
        if (trueFrustumMode)
        {
            float k = GetFrustumScale() + 0.1f;
            if (k > 2.5f)
                k = 2.5f;
            SetFrustumScale(k);
            MOHW_LOG(kLogFile, "F6 pressed -- FrustumScale now %.2f (saved to mohwvr_settings.ini)", k);
        }
        else
        {
            float newScale = GetFovScale() + kFovScaleStep;
            SetFovScale(newScale);
            MOHW_LOG(kLogFile, "F6 pressed -- FOV scale now %.2fx (saved to mohwvr_settings.ini)", newScale);
        }
    }
    upKeyWasDown = upKeyDown;

    static bool rollToggleKeyWasDown = false;
    bool rollToggleKeyDown = (GetAsyncKeyState(kHeadRollToggleHotkey) & 0x8000) != 0;
    if (rollToggleKeyDown && !rollToggleKeyWasDown)
    {
        bool newValue = !GetHeadRollEnabled();
        SetHeadRollEnabled(newValue);
        MOHW_LOG(kLogFile, "F2 pressed -- head-roll now %s (saved to mohwvr_settings.ini)",
                  newValue ? "ENABLED" : "disabled");
    }
    rollToggleKeyWasDown = rollToggleKeyDown;

    static bool rollInvertKeyWasDown = false;
    bool rollInvertKeyDown = (GetAsyncKeyState(kHeadRollInvertHotkey) & 0x8000) != 0;
    if (rollInvertKeyDown && !rollInvertKeyWasDown)
    {
        bool newValue = !GetHeadRollInvert();
        SetHeadRollInvert(newValue);
        MOHW_LOG(kLogFile, "F12 pressed -- head-roll inversion now %s (saved to mohwvr_settings.ini)",
                  newValue ? "ON" : "off");
    }
    rollInvertKeyWasDown = rollInvertKeyDown;

    static bool smoothingToggleKeyWasDown = false;
    bool smoothingToggleKeyDown = (GetAsyncKeyState(kRotationSmoothingToggleHotkey) & 0x8000) != 0;
    if (smoothingToggleKeyDown && !smoothingToggleKeyWasDown)
    {
        bool newValue = !GetRotationSmoothingEnabled();
        SetRotationSmoothingEnabled(newValue);
        MOHW_LOG(kLogFile, "HOME pressed -- rotation smoothing now %s (saved to mohwvr_settings.ini)",
                  newValue ? "ENABLED" : "disabled");
    }
    smoothingToggleKeyWasDown = smoothingToggleKeyDown;

    static bool windowUpKeyWasDown = false;
    bool windowUpKeyDown = (GetAsyncKeyState(kRotationSmoothingWindowUpHotkey) & 0x8000) != 0;
    if (windowUpKeyDown && !windowUpKeyWasDown)
    {
        float newWindow = GetRotationSmoothingWindowMs() + kRotationSmoothingWindowStepMs;
        if (newWindow > kRotationSmoothingWindowMaxMs)
            newWindow = kRotationSmoothingWindowMaxMs;
        SetRotationSmoothingWindowMs(newWindow);
        // ~80Hz is this session's own live-measured average Present rate --
        // illustrative only (actual rate varies), not a live re-measurement.
        MOHW_LOG(kLogFile,
                  "PageUp pressed -- rotation smoothing window now %.1fms (~%.1f frames at 80Hz, saved to "
                  "mohwvr_settings.ini)",
                  newWindow, newWindow * 80.0f / 1000.0f);
    }
    windowUpKeyWasDown = windowUpKeyDown;

    static bool windowDownKeyWasDown = false;
    bool windowDownKeyDown = (GetAsyncKeyState(kRotationSmoothingWindowDownHotkey) & 0x8000) != 0;
    if (windowDownKeyDown && !windowDownKeyWasDown)
    {
        float newWindow = GetRotationSmoothingWindowMs() - kRotationSmoothingWindowStepMs;
        if (newWindow < kRotationSmoothingWindowMinMs)
            newWindow = kRotationSmoothingWindowMinMs;
        SetRotationSmoothingWindowMs(newWindow);
        MOHW_LOG(kLogFile,
                  "PageDown pressed -- rotation smoothing window now %.1fms (~%.1f frames at 80Hz, saved to "
                  "mohwvr_settings.ini)",
                  newWindow, newWindow * 80.0f / 1000.0f);
    }
    windowDownKeyWasDown = windowDownKeyDown;
}

float GetRenderedHeadYawOffset()
{
    if (g_haveRenderedHeadYawOffset.load(std::memory_order_relaxed))
        return g_renderedHeadYawOffset.load(std::memory_order_relaxed);
    return GetHeadYawOffset();
}

float GetAppliedHeadRoll()
{
    return g_appliedHeadRoll.load(std::memory_order_relaxed);
}

} // namespace mohw
