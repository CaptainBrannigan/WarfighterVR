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
inline bool GetControllerAimDirection(const Vec3& nativeDirection, Vec3* outDirection)
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
    float worldYaw = atan2f(native.x, native.z) - WrapAngleSigned(aimYaw - viewYaw);
    float worldPitch = asinf(nativeY) + (aimPitch - viewPitch);
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
