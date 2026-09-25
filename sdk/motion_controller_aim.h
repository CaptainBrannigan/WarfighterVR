#pragma once
// Shared helper for redirect hooks that want to point a shot using the
// right VR motion controller's live orientation instead of a fixed
// test-only rotation. See shared/ipc_protocol.h's ControllerPoseBlock and
// hooks/companion_bridge.h's GetRightControllerPose -- the companion
// process publishes this from OpenXR's "aim" pose action
// (companion/main.cpp's CreateControllerActions/SyncAndPublishControllerPose).

#include "mohw_common.h"
#include "vr_math.h"
#include "../hooks/companion_bridge.h"

namespace mohw {

// Returns true and fills outDirection (unit vector) from the right
// controller's live orientation, using the same local-forward convention
// as vr_math.h's QuatToYawPitch/ExtractRoll (localForward.z = 1.0f before
// quaternion rotation). Returns false if the companion isn't running,
// hasn't published a pose yet, or the controller isn't currently tracked --
// callers should fall back to leaving the shot unmodified in that case.
inline bool GetControllerAimDirection(Vec3* outDirection)
{
    mohwvr::ipc::ControllerPoseBlock pose{};
    if (!GetRightControllerPose(&pose))
        return false;

    Quat q{pose.orientationX, pose.orientationY, pose.orientationZ, pose.orientationW};
    Vec3 localForward{};
    localForward.z = 1.0f;
    Vec3 dir = QuatRotateVector(q, localForward);

    // CONFIRMED LIVE (2026-09-11): pitch came out inverted -- tilting the
    // controller up sent the shot down and vice versa. Negating the
    // vertical (Y, up) component only, leaving X/Z (yaw) untouched, fixed
    // it in testing. Root cause not fully chased down (plausible: OpenXR's
    // right-handed/-Z-forward convention vs. this project's established
    // +Z-forward local-axis convention interacting differently for a
    // controller's "point where I'm aiming" pose than it does for the
    // head's forward-look pose it was originally validated against) --
    // if a future runtime/controller shows the OPPOSITE sign, this is the
    // line to flip back, not QuatRotateVector itself (head pose still
    // uses that unmodified and correctly).
    dir.y = -dir.y;
    *outDirection = dir;
    return true;
}

} // namespace mohw
