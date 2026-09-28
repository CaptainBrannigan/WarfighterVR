#pragma once
// Shared helper for redirect hooks that want to point a shot using the right VR motion controller's live orientation.
// See shared/ipc_protocol.h's ControllerPoseBlock and hooks/companion_bridge.h's GetRightControllerPose, published by
// openvr_direct/vr_input.cpp from SteamVR Input's RightHandAim pose action.

#include "mohw_common.h"
#include "vr_math.h"
#include "settings.h"
#include "../hooks/aiming_controller_hook.h"
#include "../hooks/companion_bridge.h"
#include "../hooks/head_position.h"

#include <cmath>

namespace mohw {

// SHOT SPREAD REMOVAL (2026-09-28): the native direction the game hands the fire hook already has the weapon's random
// spread in it (the spread traced earlier -- Box-Muller cone, aim-assist accuracy radius -- is all upstream of this),
// and the relative mapping below carries it straight through. The spread-free direction is the AimingController's own
// yaw/pitch, which map to world angles by fixed offsets: world yaw = K - AC yaw, world pitch = C + AC pitch (slopes
// live-confirmed 2026-09-26). K and C are measured from the shots themselves: each shot gives K_i = native yaw + view
// yaw and C_i = native pitch - view pitch, i.e. the offset plus that shot's spread, and the spread averages out, so
// the median of recent shots is the offset. Until kMinSamples shots are in, the relative (spread-keeping) mapping is
// used. One instance per caller: the cosmetic raycast may carry its own, different spread.
//
// LOCKED + OUTLIER GATE (2026-09-28, live regression): some shots come out ~100-180 deg away from the view the head
// aim last applied -- seen after a 17 s gap, likely a scripted camera (breach) steering the view while the
// AimingController stood still -- and enough of them in the median dragged K to -2..-3 rad, throwing every normal shot
// off. So once most samples agree (within kLockAgreeRad of their median), K and C are LOCKED. A shot more than
// kOutlierRad from the locked values keeps the game's own direction (the relative mapping) and stays out of the
// estimate. kRelockRun consecutive outliers that agree with each other re-lock to their value, in case the
// convention genuinely changed (e.g. a new level).
struct ShotSpreadRemover
{
    static constexpr int kSamples = 15;
    static constexpr int kMinSamples = 5;
    static constexpr float kLockAgreeRad = 10.0f * 3.14159265f / 180.0f;
    static constexpr float kOutlierRad = 15.0f * 3.14159265f / 180.0f;
    static constexpr int kRelockRun = 20;
    float yawDev[kSamples] = {};   // K_i - yawRef
    float pitchDev[kSamples] = {}; // C_i
    int count = 0;
    int next = 0;
    bool haveRef = false;
    float yawRef = 0.0f;
    bool locked = false;
    float lockedYaw = 0.0f, lockedPitch = 0.0f;
    int outlierRun = 0;
    bool lastWasOutlier = false;
    // The last shot's spread (measured minus estimated offset), radians, for callers to log.
    float lastSpreadYaw = 0.0f, lastSpreadPitch = 0.0f;

    void ResetSamples()
    {
        count = 0;
        next = 0;
        haveRef = false;
    }

    static float Median(const float* values, int n)
    {
        float sorted[kSamples];
        for (int i = 0; i < n; ++i)
            sorted[i] = values[i];
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && sorted[j - 1] > sorted[j]; --j)
            {
                float t = sorted[j];
                sorted[j] = sorted[j - 1];
                sorted[j - 1] = t;
            }
        return (n & 1) ? sorted[n / 2] : 0.5f * (sorted[n / 2 - 1] + sorted[n / 2]);
    }

    void Add(float yawOffset, float pitchOffset)
    {
        if (!haveRef)
        {
            yawRef = yawOffset;
            haveRef = true;
        }
        yawDev[next] = WrapAngleSigned(yawOffset - yawRef);
        pitchDev[next] = pitchOffset;
        next = (next + 1) % kSamples;
        if (count < kSamples)
            ++count;
    }

    // The game's own shot runs along the camera's forward, which is exactly the AimingController's yaw/pitch, so the
    // true offsets are 0 -- measured values across sessions scattered +/-1 deg around 0 (spread the median didn't fully
    // cancel), and at 70 m that 1 deg put hits a metre off the controller's line. Within kSnapRad of 0 they snap to 0;
    // the measurement still catches a genuinely different offset.
    static constexpr float kSnapRad = 3.0f * 3.14159265f / 180.0f;

    bool Median2(float* yawOffset, float* pitchOffset) const
    {
        if (count < kMinSamples)
            return false;
        *yawOffset = WrapAngleSigned(yawRef + Median(yawDev, count));
        *pitchOffset = Median(pitchDev, count);
        if (fabsf(*yawOffset) < kSnapRad)
            *yawOffset = 0.0f;
        if (fabsf(*pitchOffset) < kSnapRad)
            *pitchOffset = 0.0f;
        return true;
    }

    // Feeds one shot's measured offsets. Returns true (and the offsets to aim with) if the spread can be removed for
    // this shot; false = use the game's own direction.
    bool Classify(float shotYawOffset, float shotPitchOffset, float* yawOffset, float* pitchOffset)
    {
        lastWasOutlier = false;
        if (!locked)
        {
            Add(shotYawOffset, shotPitchOffset);
            float y = 0.0f, p = 0.0f;
            if (!Median2(&y, &p))
                return false;
            int agree = 0;
            for (int i = 0; i < count; ++i)
                if (fabsf(WrapAngleSigned(yawRef + yawDev[i] - y)) <= kLockAgreeRad &&
                    fabsf(pitchDev[i] - p) <= kLockAgreeRad)
                    ++agree;
            if (agree * 5 < count * 4) // under 80% agreement: keep measuring
                return false;
            locked = true;
            lockedYaw = y;
            lockedPitch = p;
            ResetSamples();
        }
        float dy = WrapAngleSigned(shotYawOffset - lockedYaw), dp = shotPitchOffset - lockedPitch;
        if (fabsf(dy) > kOutlierRad || fabsf(dp) > kOutlierRad)
        {
            lastWasOutlier = true;
            // Consecutive outliers that agree among themselves = a genuinely new offset: re-lock to it.
            if (outlierRun == 0)
                ResetSamples();
            ++outlierRun;
            Add(shotYawOffset, shotPitchOffset);
            float y = 0.0f, p = 0.0f;
            if (outlierRun >= kRelockRun && Median2(&y, &p))
            {
                int agree = 0;
                for (int i = 0; i < count; ++i)
                    if (fabsf(WrapAngleSigned(yawRef + yawDev[i] - y)) <= kLockAgreeRad &&
                        fabsf(pitchDev[i] - p) <= kLockAgreeRad)
                        ++agree;
                if (agree * 5 >= count * 4)
                {
                    lockedYaw = y;
                    lockedPitch = p;
                    outlierRun = 0;
                    ResetSamples();
                }
            }
            return false;
        }
        outlierRun = 0;
        lastSpreadYaw = dy;
        lastSpreadPitch = dp;
        *yawOffset = lockedYaw;
        *pitchOffset = lockedPitch;
        return true;
    }

    // For logging: the offsets currently aimed with, if locked.
    bool Get(float* yawOffset, float* pitchOffset) const
    {
        if (!locked)
            return false;
        *yawOffset = lockedYaw;
        *pitchOffset = lockedPitch;
        return true;
    }
};

// Returns true and fills outDirection (game-world unit vector) for a shot pointed by the right controller, given the
// game's own direction for that shot (nativeDirection, any length: where the view points). Returns false if the
// controller isn't tracked or head-aim hasn't applied yet -- callers should leave the shot unmodified.
//
// Relative, so the game's world-direction convention never has to be guessed: the controller's yaw/pitch go through
// the SAME mapping head look uses (hooks/aiming_controller_hook.cpp's HEAD LOOK MODEL, head convention via
// QuatToYawPitch), giving the AimingController yaw/pitch the head would produce facing where the controller points.
// The difference from the yaw/pitch actually applied this tick (the view) then rotates the native direction. A
// controller pointed where the view points fires the game's own shot exactly. Slopes, live-confirmed 2026-09-26:
// world yaw moves opposite to AimingController yaw, world pitch the same way as AimingController pitch.
inline bool GetControllerAimDirection(const Vec3& nativeDirection, Vec3* outDirection,
                                      ShotSpreadRemover* spreadRemover = nullptr)
{
    mohwvr::ipc::ControllerPoseBlock pose{};
    if (!GetRightControllerPose(&pose))
        return false;
    float zeroYaw = 0.0f, zeroPitch = 0.0f, gameYaw = 0.0f, basePitch = 0.0f;
    float viewYaw = 0.0f, viewPitch = 0.0f;
    if (!GetHeadAimMapping(&zeroYaw, &zeroPitch, &gameYaw, &basePitch) || !GetLastAppliedYawPitch(&viewYaw, &viewPitch))
        return false;
    float nativeLenSq = VecDot(nativeDirection, nativeDirection);
    if (nativeLenSq < 1e-8f)
        return false;
    Vec3 native = VecScale(nativeDirection, 1.0f / sqrtf(nativeLenSq));

    Quat q{pose.orientationX, pose.orientationY, pose.orientationZ, pose.orientationW};
    float controllerYaw = 0.0f, controllerPitch = 0.0f;
    QuatToYawPitch(q, &controllerYaw, &controllerPitch);

    float sensitivity = GetHeadAimSensitivity();
    float yawSign = GetHeadAimInvertYaw() ? -1.0f : 1.0f;
    float pitchSign = GetHeadAimInvertPitch() ? -1.0f : 1.0f;
    float aimYaw = gameYaw + yawSign * sensitivity * WrapAngleSigned(controllerYaw - zeroYaw);
    float aimPitch = basePitch + pitchSign * sensitivity * (controllerPitch - zeroPitch);

    float nativeY = native.y > 1.0f ? 1.0f : (native.y < -1.0f ? -1.0f : native.y);
    float nativeYaw = atan2f(native.x, native.z);
    float nativePitch = asinf(nativeY);
    float worldYaw = nativeYaw - WrapAngleSigned(aimYaw - viewYaw);
    float worldPitch = nativePitch + (aimPitch - viewPitch);
    if (spreadRemover)
    {
        float shotYawOffset = WrapAngleSigned(nativeYaw + viewYaw);
        float shotPitchOffset = nativePitch - viewPitch;
        float yawOffset = 0.0f, pitchOffset = 0.0f;
        if (spreadRemover->Classify(shotYawOffset, shotPitchOffset, &yawOffset, &pitchOffset) && GetRemoveShotSpread())
        {
            worldYaw = yawOffset - aimYaw;
            worldPitch = pitchOffset + aimPitch;
        }
    }
    outDirection->x = cosf(worldPitch) * sinf(worldYaw);
    outDirection->y = sinf(worldPitch);
    outDirection->z = cosf(worldPitch) * cosf(worldYaw);
    return true;
}

// Game-world offset from the game's own shot origin to the right controller, so a shot can start from the hand
// instead of the eye. The game's origin is its own camera, which sits where the head was at the last recenter
// (hooks/head_position.cpp only moves the RENDER camera), so with head position on the offset is measured from that
// recenter anchor; with it off the view sits on the game camera, so it's measured from the head's current position.
// Clamped as a safety net against a bad pose. False if the controller or head isn't tracked or head-aim hasn't
// recentered.
inline bool GetControllerOriginOffset(Vec3* outOffset)
{
    constexpr float kMaxOffsetMeters = 1.5f;
    mohwvr::ipc::ControllerPoseBlock controller{};
    mohwvr::ipc::HeadPoseBlock head{};
    if (!GetRightControllerPose(&controller) || !GetHeadPose(&head))
        return false;
    float anchor[3] = {head.positionX, head.positionY, head.positionZ};
    if (GetHeadPositionEnabled() && !GetHeadAimPositionOrigin(anchor))
        return false;

    float world[3];
    if (!TrackingOffsetToGameWorld(controller.positionX - anchor[0], controller.positionY - anchor[1],
                                   controller.positionZ - anchor[2], world))
        return false;
    Vec3 offset{};
    offset.x = world[0];
    offset.y = world[1];
    offset.z = world[2];
    float len = sqrtf(VecDot(offset, offset));
    if (len > kMaxOffsetMeters)
        offset = VecScale(offset, kMaxOffsetMeters / len);
    *outOffset = offset;
    return true;
}

} // namespace mohw
