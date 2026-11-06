#pragma once
// NATIVE TWO-VIEW STEREO (EXPERIMENTAL, 2026-11-06; docs/native_stereo_render_path.md): both eyes rendered by the engine
// in ONE frame, the way the AvP mod renders from its camera slots, instead of one eye per frame (hooks/alternating_eye.h).
//
// Frostbite builds a list of views every frame (FUN_00A6BDC0, sdk/mohw_offsets.h OFFSET_VIEW_LIST_BUILD) and its world
// renderer draws up to two of them. GameRenderSettings::SplitScreenTestViewCount = 2, a split-screen test the game
// still carries, makes that list two views side by side: the left half and the right half of the frame. This module
// keeps the setting at 2 while NativeStereo is on, and after the list is built makes the two views the two eyes: the
// same camera moved by -IPD/2 / +IPD/2 along its right axis, each with its eye's real asymmetric frustum written into
// the view's own projection inputs (fovY, aspect, viewport offset: the engine's projection builder reads them, so no
// per-call matching is needed). Present then hands the two halves of the backbuffer to the headset as one pair.
//
// While it's active the per-frame eye machinery stands down: no eye offset in CommitViewTransform, no per-eye
// projection hook, no parity flip (IsRightEyeActive stays false), Present paced to the display rate, not twice it.
// NOT YET ADAPTED (first light): the first-person weapon and body rewrite (hooks/camera_matrix_test_hook.cpp) and the
// HUD placement (hooks/draw_trace_diag.cpp) still assume one eye per frame, the left one.

namespace mohw {

bool InstallNativeStereoHook();
void RemoveNativeStereoHook();

// Present (game thread), every frame: keeps SplitScreenTestViewCount at 2 while the setting is on, and gives the game
// its own value back when it's turned off.
void NativeStereoOnPresent();

// True while the setting is on and the engine built two views within the last quarter second. Any thread.
bool IsNativeStereoActive();

} // namespace mohw
