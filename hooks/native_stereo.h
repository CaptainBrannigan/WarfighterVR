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
// PER VIEW (2026-11-08): the first-person weapon and body are drawn from each view's own raw camera (ViewDesc +0xE00),
// which this module moves to the eye too; hooks/camera_matrix_test_hook.cpp tells the views apart by that camera's
// position (NativeStereoEyeForCamera) and gives the gun that eye's frustum (GetNativeEyeProjection). The HUD is replayed
// once into each half (hooks/ui_pass.cpp) and placed per eye there (hooks/draw_trace_diag.cpp). The in-world markers
// come from one projection for both halves, put in the right direction at the HUD's depth
// (hooks/marker_projection_hook.cpp).

namespace mohw {

bool InstallNativeStereoHook();
void RemoveNativeStereoHook();

// Present (game thread), every frame: keeps SplitScreenTestViewCount at 2 while the setting is on, and gives the game
// its own value back when it's turned off.
void NativeStereoOnPresent();
// Present (game thread), every frame: the backbuffer's size, for the render scale from SteamVR's resolution
// (RenderScaleFromSteamVr: GameRenderSettings.ResolutionScale = SteamVR's recommended eye height / this height).
void NativeStereoNoteBackbuffer(unsigned width, unsigned height);
// Present (game thread): whether the swap chain is really windowed (IDXGISwapChain::GetFullscreenState), for the
// eye-sized backbuffer (EyeSizedBackbuffer), which only asks the game for a size that isn't a monitor mode then.
void NativeStereoNoteSwapChainWindowed(bool windowed);

// True while the setting is on and the engine built two views within the last quarter second. Any thread.
bool IsNativeStereoActive();
// Render threads (hooks/camera_matrix_test_hook.cpp): which eye a first-person batch is being built for, from its view's
// raw camera position (ViewDesc +0xE00, moved to each eye here): 0 left, 1 right, -1 not one of the last frames' eye
// cameras (then the frame's eye applies, as without this). *frame (if given) = which built frame it was, counting up,
// so the two eyes' batches of one frame can be paired (0 with no match).
int NativeStereoEyeForCamera(const float position[3], unsigned* frame = nullptr);
// Any thread: an eye's projection as written into its view: fovY (radians), aspect, viewport offset x, y. False before
// the first two-view frame, or without headset data.
bool GetNativeEyeProjection(int eye, float out[4]);
// Any thread (diagnostics): the last two-view frame's head-centre camera position, its right axis, and the world
// cameras written into view 0 (left) and view 1 (right). False before the first two-view frame.
bool GetNativeEyeCameras(float centre[3], float right[3], float left[3], float rightEye[3]);
// Any thread (diagnostics): which of the last frames' world cameras a camera position is, within 5 mm: 0 the left eye's
// (as written into view 0), 1 the right eye's (view 1), 2 the head centre (what the builder made), -1 none of them.
int NativeStereoClassifyWorldCamera(const float position[3]);

} // namespace mohw
