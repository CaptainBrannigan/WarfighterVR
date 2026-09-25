#pragma once
// Live test of FUN_007D4680 (sdk/mohw_offsets.h's OFFSET_FIRECANDIDATE), the
// fire-time wrapper around FUN_007CF810/FUN_007F4D80's geometric hit test.
// Confirmed via live breakpoint to fire ONLY when actually firing at a live
// AI target -- not on a wall shot, and not during the continuous reticle
// aim-assist pass that also goes through FUN_007CF810. This makes it the
// most surgical player-fire-time candidate found so far for the real
// authoritative hit-scan.
//
// This hook redirects the ray's heading (this+0x190, the end point) to the
// right VR motion controller's live aim direction (this+0x180, the origin,
// is left unmodified -- same game-provided muzzle/reticle position, only
// rotation comes from the controller), preserving the ray's original
// length, then restores this+0x190 afterward. Falls back to calling the
// original unmodified if the companion process isn't running or hasn't
// published a controller pose yet (see sdk/motion_controller_aim.h). It
// also logs this+0x1b4
// (the running candidate count) before/after each call, to directly observe
// whether a candidate was actually recorded without needing to decode the
// internal branch flag.
//
// Diagnostic/test only, no settings.ini toggle, always-on once installed.

namespace mohw {

bool InstallFireCandidateRedirectHook();
void RemoveFireCandidateRedirectHook();

} // namespace mohw
