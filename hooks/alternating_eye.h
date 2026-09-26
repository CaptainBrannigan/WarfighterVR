#pragma once
// Alternating-eye rendering (2026-08-23): renders ONE eye per frame at the game's normal, single-pass cost,
// alternating which eye between frames. Trades simultaneous stereo for temporal stereo (each eye is one frame
// "stale" relative to the other) -- the only stereo strategy this project ships (draw-duplication's same-frame,
// both-eyes-every-frame alternative was tried and retired; see project memory for why).
//
// This module does NOT own an independent MinHook hook -- it's a plain helper module called INTO from two hooks
// that already exist:
//   - fov_scale_hook.cpp's CommitViewTransform hook calls ApplyEyeOffset, additive on top of the native
//     AimingController yaw/pitch sync and fov_scale_hook.cpp's own head-roll: offsets ONLY the transform's trans
//     (position) row, by +-IPD/2 along the transform's own left axis depending on which eye is currently active.
//   - present_hook.cpp's Present hook calls AdvanceEyeToNextFrame once per frame (so parity flips exactly once
//     per displayed frame, independent of how many times CommitViewTransform itself fires that frame), then
//     routes that frame's captured backbuffer to whichever eye slot IsRightEyeActive() reports, passing nullptr
//     for the OTHER eye -- both UpdateCompanionEyes and openvr_direct's UpdateOpenVrDirect treat a null source as
//     "leave that eye's shared texture untouched this frame", which is exactly the temporal-stereo semantics this
//     needs (the untouched eye keeps showing its last frame until its next turn).

namespace mohw {

// Reuses sdk/settings.h's existing IPD scale (GetIpdScale, F7/F8) rather than introducing a second,
// independently tunable IPD value -- see ApplyEyeOffset's kBaseIpdMeters comment for the human-IPD assumption
// this builds on.

// Which eye the CURRENT frame is rendering. Set by AdvanceEyeToNextFrame; read by both ApplyEyeOffset
// (fov_scale_hook.cpp) and present_hook.cpp's backbuffer routing, so the two stay in lockstep for a given frame.
bool IsRightEyeActive();

// Call once per Present, before this frame's transform/draw work happens -- flips which eye is active for the
// frame about to render.
void AdvanceEyeToNextFrame();

// Call from fov_scale_hook.cpp's CommitViewTransform hook, alongside (not instead of) its own FOV/head-roll
// patches. Reads/writes only the transform's trans row (floats 12-14); left/up/forward are left untouched.
bool ApplyEyeOffset(void* transformPtr);

// The offset ApplyEyeOffset adds for the active eye, in meters along the camera transform's row 0 (left eye
// negative, right eye positive; half the scaled IPD). Lets other code find the head-centre camera from an eye's.
float GetActiveEyeOffsetAlongRow0();

// F1-F12 are all already claimed elsewhere in this project (see this project's other hooks' hotkey constants).
// Edge-detected toggle/step, same pattern as every other runtime hotkey in this project. Call once per Present
// from present_hook.cpp. Also polls F7/F8 (IPD scale step, migrated here from the retired draw-duplication path
// -- see sdk/settings.h's GetIpdScale/SetIpdScale) and CAPS LOCK (see IsStalenessIsolationTestActive comment).
void CheckAlternatingEyeHotkeys();

// STALENESS ISOLATION TEST (2026-09-24), CAPS LOCK: every content/order/index/parity swap tried while chasing the
// persistent right-eye ghost always left the physical right eye (OpenVR index 1) receiving SOME alternating
// stale/fresh submission pattern -- none of them tested "what if that slot is simply never held stale". When
// this is active, present_hook.cpp forces the right eye's shared texture to update every frame instead of being
// left untouched on off-parity frames (visually wrong content ~half the time, an accepted diagnostic cost) to
// isolate staleness-duration from content-identity as the variable under test. Confirmed monocularly visible
// (user closed left eye, ghost still present in right eye alone) -- see project memory for the fuller context.
bool IsStalenessIsolationTestActive();

} // namespace mohw
