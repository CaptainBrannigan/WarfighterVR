#pragma once
// Head look and stereo for the driving missions (game\MOHWD.exe, 2026-10-14). The camera source is MOHWD.exe's
// CommitViewTransform (FUN_00926dd0, found from the views' caller chains -- see the .cpp's CAMERA SOURCE): the game's
// view for the frame passes through it once, and this hook hands the renderer a copy turned by the head (pitch and roll;
// yaw too with a stick look range of 0) with this frame's eye shift and frustum. Every view drawn with is derived from it.
// UpdateMatrices (0x70CC60, signature-matched from MOHW.exe's FUN_00707CB0) is hooked too: it ranks the views by call
// rate (logged to mohwvr_drivingcamera.log), checks the source frustum reaches them, and turns a manual range of views
// for debugging.

namespace mohw {

// Driving executable only (g_drivingExe).
bool InstallDrivingCameraHook();

// Head yaw as right-stick look for the vehicle camera (2026-10-14): turning the view by render alone left the game's
// own camera -- and its culling -- pointing ahead, so meshes popped in or were missing where the head looked. The
// vehicle camera's stick free-look is a position (held deflection = held angle, springs back on release), so the
// head's yaw from a level straight-ahead maps to a deflection: yaw / DrivingStickLookRange, clamped, lifted past the
// game's deadzone (DrivingStickDeadzone). The render then adds only pitch and roll. False when not active (outside the
// driving executable, head look off, or range 0 = render-only yaw). For hooks/xinput_hook.cpp.
bool GetDrivingStickLook(float* stickX);

// Whether the driving view got its per-eye shift and true frustum in the last 300 ms (DrivingStereo on, a 3D scene):
// the frame is a real per-eye view then, shown full-view instead of as the whole-frame floating screen
// (hooks/draw_trace_diag.cpp Is2DFrame).
bool DrivingStereoActive();

// The in-game menu in the driving missions (2026-10-14): the simulation is taken as paused once the game's own committed
// view has stayed bit-identical for 300 ms (in play the vehicle camera moves every tick) -- the same 300 ms rule MOHW.exe
// applies to its aim updates (hooks/draw_trace_diag.cpp IsSimulationPaused, which uses GetDrivingViewChangedMs here).
// While paused, head yaw and the steering wheel stay off the sticks, so the menu gets the real sticks.
unsigned long long GetDrivingViewChangedMs();
bool DrivingSimulationPaused();

// Present hook, once per Present, with the eye the frame is routed to: logs frames whose committed view wasn't exactly
// one of the presented eye (diagnosing flicker between stereo and a double image).
void DrivingStereoOnPresent(bool presentedRightEye);

// CUTSCENES (2026-11-21, the user: the cutscene at the end of the driving level "tries to remain in eyes"). It renders
// through a normal 3D camera, so it was taken for play. What sets it apart, live (mohwvr_driving_present.log
// 08:46:18): the game presents its cutscenes and loading screens with vsync (syncInterval 1, 30 fps) and play with
// syncInterval 0. So a frame presented with a higher sync interval than the lowest seen in 3D play is a cutscene: no
// head look, and the whole-frame floating screen like the rest of the game's cutscenes. With the game's own vsync on,
// play and cutscenes present alike and nothing changes. Present hook, every frame (driving executable only).
void DrivingNotePresentSync(unsigned syncInterval);
bool DrivingCutsceneNow();

} // namespace mohw
