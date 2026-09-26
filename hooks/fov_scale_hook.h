#pragma once
// Isolated FOV-widening hook, split out of commit_view_transform_hook.cpp
// (2026-08-22) now that render-side ROTATION there is retired -- confirmed
// live that the game natively syncs its camera to
// AimingController+0xC/+0x10 (hooks/aiming_controller_hook.cpp), making
// commit_view_transform_hook.cpp's independent rotation writes redundant
// and (very likely) actively fighting that native sync for a good chunk
// of this project's render-pitch bugs. See commit_view_transform_hook.h's
// updated header comment for the full story.
//
// FOV widening, however, is a genuinely independent effect (patches
// RenderViewDesc::fovY, sdk/renderview.h) with no native equivalent, and
// was confirmed live (2026-08-04) to meaningfully help VR depth
// perception alongside IPD scale -- kept alive here at the SAME injection
// point (OFFSET_COMMITVIEWTRANSFORM) purely for the FOV patch, originally
// with rotation/position left completely untouched.
//
// EXPANDED (2026-08-22) to also apply head ROLL: confirmed via a
// read-only ordering probe (still present in the .cpp, temporary) that
// this injection point sees a transform ALREADY correctly yaw/pitched by
// the native AimingController sync (pitch is an exact negation of
// AimingController's own value every frame -- a known sign-convention
// difference, not a bug; yaw settles to a fixed relationship once head
// movement stops, consistent with some native smoothing/lag but
// genuinely derived from AimingController, not independent). Since roll
// is mathematically orthogonal to yaw/pitch (rotating around forward
// doesn't touch either), and AimingController itself has no roll concept
// at all, adding roll here is additive on top of whatever the native sync
// already produced -- NOT competing with it the way the old full-rotation
// hook was. Roll is extracted directly from real-time HMD orientation via
// sdk/vr_math.h's ExtractRoll (absolute, relative to world-level -- no
// recenter dependency, unlike yaw/pitch) and applied to ONLY the
// transform's left/up rows; forward/trans are left untouched (pure roll
// leaves forward mathematically unchanged by construction).
//
// MinHook doesn't support two independent hooks on the same target
// address, which is why roll (and the temporary ordering-probe
// diagnostic) live in THIS file rather than a separate one -- this file
// already owns OFFSET_COMMITVIEWTRANSFORM.

namespace mohw {

bool InstallFovScaleHook();
void RemoveFovScaleHook();

// F5/F6 step: live control for the game's internal FOV scale, same
// hotkeys commit_view_transform_hook.cpp used to poll for this.
// F2: toggle head-roll on/off (sdk/settings.h's HeadRollEnabled, default
// OFF -- brand new, not yet live-validated). F12: toggle roll inversion
// (HeadRollInvert) if it turns out backwards, same pattern as every other
// axis in this project needing a live sign-flip test.
void CheckFovScaleHotkeys();

// The head-driven share of the yaw in the camera transform just processed (AimingController space, radians):
// aiming_controller_hook.h's GetHeadYawOffset run through the same smoothing filter and pair-freeze as the camera's
// own rotation, so it matches what was actually rendered. Separates head yaw from mouse/stick turning for
// hooks/render_pose_stamp.cpp. Equals GetHeadYawOffset() when rotation smoothing is off.
float GetRenderedHeadYawOffset();

// The roll (radians, about the camera's local Z) ApplyHeadRoll last added to the world camera; 0 while head roll is off
// or no pose is available. The first-person rig's camera doesn't get it, so camera_matrix_test_hook.cpp adds the
// same rotation to the body to keep it from rolling with the head.
float GetAppliedHeadRoll();

} // namespace mohw
