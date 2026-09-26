#pragma once
// Hooks FUN_006FC2A0 (the camera-class matrix setter, __thiscall, one 4x4 float* arg, RET 4) and acts only when it's
// called from FUN_008B2AB0's first-person viewmodel batch build (OFFSET_CAMERAMATRIX_CALLER_VIEWMODEL): the local
// camera each first-person batch is drawn from. Weapon batches (count != 3) can be redrawn as if held by the right
// controller (the weapon drive, see the .cpp's v3 comment); body+hands batches pass through. Also hooks FUN_00723800
// for the per-instance body hide test (legs/arms/hands), currently inert.

namespace mohw {

bool InstallCameraMatrixTestHook();
void RemoveCameraMatrixTestHook();

// Numpad . once per frame (present_hook.cpp): toggles the weapon drive. Turning it on snaps the gun onto the controller
// at the ini's grip point (WeaponGripRight/Up/Back) and logs where the hand actually is, for tuning that point.
void CheckWeaponDriveHotkey();

// For projection_aspect_hook.cpp's UpdateMatrices hook: true (once) if `camera` is the first-person viewmodel camera
// this thread just set a batch matrix on, so the viewmodel can be given the same per-eye true frustum as the world
// (its own 55 degree symmetric projection doesn't match the headset, which is what doubles and head-locks the gun).
bool ConsumeViewmodelCamera(const void* camera);

// "Player skeleton loaded" signal (2026-09-22, for draw_trace_diag.cpp's UI element separation): true if the
// viewmodel batch matrix setter (this file's own HookImpl, gated on OFFSET_CAMERAMATRIX_CALLER_VIEWMODEL's call
// site -- i.e. the confirmed first-person body+hands/weapon rig, never fires in a menu) has fired within the last
// half second. Cheap (a tick-count compare) and reuses a hook that's already running every gameplay frame, instead
// of a fresh memory scan. Replaces the previous OFFSET_PLAYERCAMERAOBJECT position check, which stayed non-zero (and
// so looked "loaded") even on the main menu -- likely a leftover/background camera used for its showcase scene.
bool IsPlayerSkeletonLoaded();

} // namespace mohw
