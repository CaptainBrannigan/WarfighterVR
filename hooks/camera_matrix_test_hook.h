#pragma once
// Diagnostic test (2026-09-19): hooks FUN_006FC2A0 (the camera-class matrix
// setter, __thiscall, one 4x4 float* arg, RET 4) and, ONLY when called from
// FUN_0099C2A0's call site (return address 0x0099C473), yaws the incoming
// matrix's three basis rows by kYawDegrees before the original copies it into
// the camera object. Found via Cheat Engine: this is the write that fills the
// camera at (that session's) 02AE2890, and freezing that object left the
// arms+gun moving with the body -- so this test shows what does and doesn't
// follow the game-side camera rotation. Translation row is left untouched.
// Always-on once installed, no settings.ini toggle.

namespace mohw {

bool InstallCameraMatrixTestHook();
void RemoveCameraMatrixTestHook();

// "Player skeleton loaded" signal (2026-09-22, for draw_trace_diag.cpp's UI element separation): true if the
// viewmodel batch matrix setter (this file's own HookImpl, gated on OFFSET_CAMERAMATRIX_CALLER_VIEWMODEL's call
// site -- i.e. the confirmed first-person body+hands/weapon rig, never fires in a menu) has fired within the last
// half second. Cheap (a tick-count compare) and reuses a hook that's already running every gameplay frame, instead
// of a fresh memory scan. Replaces the previous OFFSET_PLAYERCAMERAOBJECT position check, which stayed non-zero (and
// so looked "loaded") even on the main menu -- likely a leftover/background camera used for its showcase scene.
bool IsPlayerSkeletonLoaded();

} // namespace mohw
