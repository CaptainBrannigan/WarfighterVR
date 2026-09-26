#include "render_pose_stamp.h"

#include "aiming_controller_hook.h"
#include "alternating_eye.h"
#include "companion_bridge.h"
#include "fov_scale_hook.h"
#include "head_position.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"
#include "../sdk/vr_math.h"
#include "../shared/ipc_protocol.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_fovscale.log";
constexpr float kRadToDeg = 57.29577951f;

std::mutex g_mutex;
bool g_pendingValid = false;
float g_pendingQuat[4] = {0, 0, 0, 1};
float g_pendingPos[3] = {0, 0, 0};
bool g_pendingPosValid = false;

HANDLE g_mapping = nullptr;
mohwvr::ipc::RenderPoseBlock* g_block = nullptr;

// In-process per-eye persisted stamp for openvr_direct.cpp (2026-09-23) -- see GetPendingRenderPoseStamp's
// declaration comment. Separate from g_block (the IPC path, kept for any other consumer) since this needs
// its own per-eye staleness tracking: g_pendingQuat/etc. above are a single "current frame's stamp", tagged
// to whichever eye is active only at publish time, whereas Submit happens for BOTH eyes every call (the
// other eye reusing its last cached texture) and each needs ITS OWN last-known stamp, not the other eye's.
struct EyeStamp
{
    bool valid = false;
    float quat[4] = {0, 0, 0, 1};
    float pos[3] = {0, 0, 0};
    bool posValid = false;
    int staleFrames = 1000;
};
EyeStamp g_eyeStamps[2];
std::atomic<bool> g_stampDebugEnabled{true};

bool EnsureBlock()
{
    if (g_block)
        return true;
    if (!g_mapping)
    {
        g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(mohwvr::ipc::RenderPoseBlock),
                                       mohwvr::ipc::kRenderPoseMapName);
        if (!g_mapping)
            return false;
    }
    g_block = static_cast<mohwvr::ipc::RenderPoseBlock*>(
        MapViewOfFile(g_mapping, FILE_MAP_WRITE, 0, 0, sizeof(mohwvr::ipc::RenderPoseBlock)));
    return g_block != nullptr;
}

bool SehReadFloats12(const void* p, float* out)
{
    __try
    {
        const float* f = static_cast<const float*>(p);
        for (int i = 0; i < 12; ++i)
            out[i] = f[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

Vec3 ForwardFromYawPitch(float yaw, float pitch)
{
    // Inverse of QuatToYawPitch's extraction (yaw = atan2(f.x, f.z), pitch = asin(f.y)).
    Vec3 f{};
    f.x = cosf(pitch) * sinf(yaw);
    f.y = sinf(pitch);
    f.z = cosf(pitch) * cosf(yaw);
    return f;
}

} // namespace

void ComputeRenderPoseStamp(void* transformPtr)
{
    bool valid = false;
    float outQuat[4] = {0, 0, 0, 1};
    float lagDeg = 0.0f;

    float raw[12]{};
    mohwvr::ipc::HeadPoseBlock pose{};
    float zeroYaw = 0, zeroPitch = 0, baseYaw = 0, basePitch = 0;
    if (transformPtr && GetHeadAimEnabled() && SehReadFloats12(transformPtr, raw) &&
        GetHeadAimMapping(&zeroYaw, &zeroPitch, &baseYaw, &basePitch) && GetHeadPose(&pose))
    {
        Vec3 fwd{raw[8], raw[9], raw[10]};
        float lenSq = VecDot(fwd, fwd);
        float sens = GetHeadAimSensitivity();
        if (lenSq > 0.5f && lenSq < 2.0f && sens > 0.01f)
        {
            float pitchT = asinf(fwd.y > 1.0f ? 1.0f : (fwd.y < -1.0f ? -1.0f : fwd.y));
            // Measured from the ORDERING PROBE log: the camera transform's pitch is the AimingController's negated
            // (pitchT = -pitchAC). Yaw can't be inverted from the transform any more: it's the game's own
            // mouse/stick yaw plus the head offset (aiming_controller_hook.cpp's HEAD LOOK MODEL), so take the head
            // offset that went into this rendered frame directly instead.
            float pitchAC = -pitchT;
            float yawDelta = GetRenderedHeadYawOffset();
            float pitchDelta = pitchAC - basePitch;
            // Invert the head-aim mapping: AC = baseline + sign * sens * (head - zeroHead).
            float headYawDelta = yawDelta / sens * (GetHeadAimInvertYaw() ? -1.0f : 1.0f);
            float headPitchDelta = pitchDelta / sens * (GetHeadAimInvertPitch() ? -1.0f : 1.0f);
            float headYawEff = zeroYaw + headYawDelta;
            float headPitchEff = zeroPitch + headPitchDelta;
            if (headPitchEff > 1.5f)
                headPitchEff = 1.5f;
            if (headPitchEff < -1.5f)
                headPitchEff = -1.5f;

            Quat qCur{pose.orientationX, pose.orientationY, pose.orientationZ, pose.orientationW};
            Vec3 localForward{};
            localForward.z = 1.0f;
            Vec3 fCur = QuatRotateVector(qCur, localForward); // same +Z-forward convention QuatToYawPitch uses
            Vec3 fEff = ForwardFromYawPitch(headYawEff, headPitchEff);
            float d = VecDot(fCur, fEff);
            if (d > -0.99f)
            {
                lagDeg = acosf(d > 1.0f ? 1.0f : d) * kRadToDeg;
                if (lagDeg < 70.0f)
                {
                    // Shortest-arc rotation carrying the current facing onto the rendered facing, applied in world space.
                    Quat arc{fCur.y * fEff.z - fCur.z * fEff.y, fCur.z * fEff.x - fCur.x * fEff.z,
                             fCur.x * fEff.y - fCur.y * fEff.x, 1.0f + d};
                    float n = sqrtf(arc.x * arc.x + arc.y * arc.y + arc.z * arc.z + arc.w * arc.w);
                    if (n > 1e-6f)
                    {
                        arc.x /= n;
                        arc.y /= n;
                        arc.z /= n;
                        arc.w /= n;
                        Quat q = QuatMultiply(arc, qCur);
                        float qn = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
                        if (qn > 1e-6f)
                        {
                            outQuat[0] = q.x / qn;
                            outQuat[1] = q.y / qn;
                            outQuat[2] = q.z / qn;
                            outQuat[3] = q.w / qn;
                            valid = true;
                        }
                    }
                }
            }
        }
    }

    float headPos[3] = {0, 0, 0};
    bool posValid = GetLastAppliedHeadPosition(headPos); // the head position the camera translation was actually built from this frame
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pendingValid = valid;
        for (int i = 0; i < 4; ++i)
            g_pendingQuat[i] = outQuat[i];
        g_pendingPosValid = posValid;
        for (int i = 0; i < 3; ++i)
            g_pendingPos[i] = headPos[i];
    }

    // Per-eye breakdown added 2026-09-21 while chasing eye-specific ghosting: the plain rate-limited log below
    // used to average together an unlabeled mix of left/right samples, hiding any asymmetry between them.
    static std::atomic<unsigned long long> nextLogMs[2] = {0, 0};
    int eyeIdx = IsRightEyeActive() ? 1 : 0;
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogMs[eyeIdx].load(std::memory_order_relaxed);
    if (now >= allowed && nextLogMs[eyeIdx].compare_exchange_strong(allowed, now + 2000, std::memory_order_relaxed))
        MOHW_LOG(kLogFile, "RENDER POSE STAMP eye=%s: %s, rendered facing lags current HMD by %.2f deg", eyeIdx ? "RIGHT" : "LEFT",
                   valid ? "valid" : "INVALID (no stamp)", lagDeg);
}

void PublishRenderPoseForEye(bool rightEye)
{
    float pos[3];
    bool posValid;
    float q[4];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_pendingValid)
            return;
        for (int i = 0; i < 4; ++i)
            q[i] = g_pendingQuat[i];
        for (int i = 0; i < 3; ++i)
            pos[i] = g_pendingPos[i];
        posValid = g_pendingPosValid;
    }
    int eye = rightEye ? 1 : 0;
    int other = 1 - eye;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_eyeStamps[eye].valid = true; // g_pendingValid already gated the early-return above
        for (int i = 0; i < 4; ++i)
            g_eyeStamps[eye].quat[i] = q[i];
        for (int i = 0; i < 3; ++i)
            g_eyeStamps[eye].pos[i] = pos[i];
        g_eyeStamps[eye].posValid = posValid;
        g_eyeStamps[eye].staleFrames = 0;
        if (g_eyeStamps[other].staleFrames < 100000)
            ++g_eyeStamps[other].staleFrames;
    }

    if (!EnsureBlock())
        return;
    for (int i = 0; i < 4; ++i)
        g_block->quat[eye][i] = q[i];
    for (int i = 0; i < 3; ++i)
        g_block->pos[eye][i] = pos[i];
    g_block->posValid[eye] = posValid ? 1 : 0;
    g_block->counter[eye] += 1;
    g_block->ready = 1;
}

bool GetPendingRenderPoseStamp(bool rightEye, float outQuat[4], float outPos[3], bool* outPosValid)
{
    if (!g_stampDebugEnabled.load(std::memory_order_relaxed))
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    int eyeIdx = rightEye ? 1 : 0;
    const EyeStamp& s = g_eyeStamps[eyeIdx];

    // L/R COMPARISON DIAGNOSTIC (2026-09-25): pose-stamp Submit confirmed (via elimination -- rotation smoothing
    // fully removed, ghost persisted) to be the right-eye ghost's actual source, but static reading of
    // ComputeRenderPoseStamp/buildSubmitArgs found no eye-dependent branching in either. Logging per-eye
    // staleFrames/quat/validity here, at the actual consumption point, to compare L vs R from a live run instead
    // of guessing further from code alone.
    static std::atomic<unsigned long long> nextCmpLogMs[2] = {0, 0};
    unsigned long long nowCmp = GetTickCount64();
    unsigned long long allowedCmp = nextCmpLogMs[eyeIdx].load(std::memory_order_relaxed);
    if (nowCmp >= allowedCmp && nextCmpLogMs[eyeIdx].compare_exchange_strong(allowedCmp, nowCmp + 500, std::memory_order_relaxed))
        MOHW_LOG(kLogFile, "STAMP CONSUME eye=%s valid=%d staleFrames=%d quat=(%.5f,%.5f,%.5f,%.5f) posValid=%d pos=(%.4f,%.4f,%.4f)",
                  eyeIdx ? "RIGHT" : "LEFT", s.valid ? 1 : 0, s.staleFrames, s.quat[0], s.quat[1], s.quat[2], s.quat[3],
                  s.posValid ? 1 : 0, s.pos[0], s.pos[1], s.pos[2]);

    if (!s.valid || s.staleFrames > 30) // ~same "menu/loading/paused -> use the live pose" convention as companion/main.cpp's ApplyPoseStamp
        return false;
    for (int i = 0; i < 4; ++i)
        outQuat[i] = s.quat[i];
    for (int i = 0; i < 3; ++i)
        outPos[i] = s.pos[i];
    *outPosValid = s.posValid;
    return true;
}

void CheckRenderPoseStampDebugHotkey()
{
    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;
    if (down && !wasDown)
    {
        bool newValue = !g_stampDebugEnabled.load(std::memory_order_relaxed);
        g_stampDebugEnabled.store(newValue, std::memory_order_relaxed);
        MOHW_LOG(kLogFile, "NUMPAD4 pressed -- render-pose stamping (Submit_TextureWithPose) now %s", newValue ? "ON" : "OFF");
    }
    wasDown = down;
}

} // namespace mohw
