#pragma once
// Hooks OFFSET_AIMINGCONTROLLER_UPDATE (sdk/mohw_offsets.h) -- the function
// that computes and writes the player's authoritative absolute yaw/pitch
// facing (AimingController+0xC/+0x10, radians) from real mouse/gamepad
// input every call. Confirmed via extensive live tracing (project memory:
// project_mohw_yaw_facing_investigation.md) to be called ONLY in the local
// player's context -- never for AI, never during a menu, loading, or the
// main menu -- so this hook needs no separate game-state gate: forwarding
// to the original every call (matching every other hook in this project)
// and then overwriting the same two fields from the HMD's current
// orientation naturally only ever affects the player, only during live
// gameplay.
//
// This SUPERSEDES the abandoned hooks/gameplay_input_hook.cpp approach
// (additively patching a per-frame input ACCUMULATOR,
// OFFSET_INPUTACCUMULATOR) -- that target was never confirmed
// authoritative and the hook itself crashed on its one live test from an
// unconfirmed stack-parameter count. This hook targets the field PROVEN
// authoritative via a freeze test (forcing it held visible facing,
// freeze-confirmed) and reuses the EXACT SAME settings/hotkey group
// (HeadAimEnabled/HeadAimSensitivity, F12/F1/F2, sdk/settings.h) since it's
// the same feature goal, just a correctly-targeted implementation --
// gameplay_input_hook.cpp/h are left in the tree, still disabled, for
// reference only. present_hook.cpp's hotkey poll has been repointed from
// CheckHeadAimHotkeys() (gameplay_input_hook.h) to
// CheckAimingControllerHotkeys() (this file) so the shared hotkeys aren't
// wired to both simultaneously.
//
// Signature CONFIRMED (2026-08-21, live: RET 0x94 = 37 stack dwords beyond
// the implicit ECX=this) and handled via a byte-size-matching passthrough
// struct -- see sdk/mohw_offsets.h's OFFSET_AIMINGCONTROLLER_UPDATE
// comment and this hook's own .cpp comment on Passthrough37 for why that's
// safe without decoding all 37 fields individually. This resolves the
// exact stack-parameter-count risk that caused this project's only crash
// so far (OFFSET_INPUTACCUMULATOR) -- no further verification needed
// before enabling this hook's install call in proxy_dll/dllmain.cpp.
// Recommended first test regardless: install with head-aim left at its
// default OFF (F12 to enable) so the FIRST live run validates only "does
// the hook install without crashing" before testing the actual override.

namespace mohw {

bool InstallAimingControllerHook();
void RemoveAimingControllerHook();

// Runtime debug hotkeys, polled from Hooked_Present same as this project's
// other debug hotkeys:
//   F12    = toggle head-driven facing/aim on/off (shared setting with the
//            old, now-superseded gameplay_input_hook.cpp)
//   F1/F2  = step sensitivity down/up (shared setting, same reason)
//   F3     = recenter -- re-anchors "looking straight ahead" to the HMD's
//            current orientation without needing to toggle off/on. Also
//            fires automatically the moment F12 turns head-aim ON, so a
//            manual F3 press is only needed to re-anchor mid-session (e.g.
//            after standing up or adjusting the headset).
//   F4     = toggle yaw inversion (sdk/settings.h's HeadAimInvertYaw) --
//            live sign-flip for the not-yet-independently-validated axis
//            convention (see sdk/vr_math.h's QuatToYawPitch comment).
//   F1     = toggle pitch inversion (HeadAimInvertPitch), same reason.
//            Reused (2026-08-22) -- freed up by temporarily disabling
//            commit_view_transform_hook.cpp's install + hotkey poll (see
//            dllmain.cpp/present_hook.cpp) to isolate this hook for
//            testing; re-check for a collision if that hook is
//            re-enabled before picking a different key.
//
// HeadAimClampPitch (sdk/settings.h) is still a real, persisted, Get/Set
// setting but has NO dedicated hotkey -- F9/F10 are claimed by
// memory_dump.cpp's dump hotkeys. Edit mohwvr_settings.ini directly and
// relaunch to change it until a real settings menu exists.
void CheckAimingControllerHotkeys();

// Returns the yaw/pitch this hook itself last successfully wrote into
// AimingController (i.e. what the native camera sync should be reading on
// its next tick), NOT a fresh memory read. Returns false (outputs
// untouched) if this hook has never written anything yet. Added
// (2026-08-22) so other hooks -- e.g. a prospective roll-only render
// patch -- can verify their own ordering assumptions against what this
// hook actually applied, without each hook needing its own independent
// way to read AimingController.
bool GetLastAppliedYawPitch(float* outYaw, float* outPitch);

// The head-aim mapping currently in force: the HMD yaw/pitch captured at the last recenter (zeroHead*) and the game
// yaw/pitch it maps to (baseline*). AimingController yaw/pitch = baseline + sign * sensitivity * (headAngle - zeroHead).
// Returns false until head-aim has recentered at least once. Used by hooks/render_pose_stamp.cpp to invert the mapping.
bool GetHeadAimMapping(float* zeroHeadYaw, float* zeroHeadPitch, float* baselineYaw, float* baselinePitch);

// HMD position (XR local space, meters) captured at the last recenter -- the origin head-position tracking measures from.
bool GetHeadAimPositionOrigin(float out[3]);

} // namespace mohw
