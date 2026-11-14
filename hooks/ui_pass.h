#pragma once
// THE UI PASS (2026-11-06, Ghidra; docs/ui_render_path.md): the game's Scaleform UI is replayed from two command lists
// by FUN_00CBE3D0 (__thiscall, ECX = the UI's movie manager, one arg: a parameter block, RET 4), called from the UI
// render module (FUN_008F42F0, registered in GameRenderer's module slots 0 and 2, the world being slot 1). Block[3]
// picks the list: 0 = UI drawn before the world into its own target (block[5]), 1 = the HUD over the finished frame
// (and once per eye when the engine's stereo is on).
//
// This hook only marks the pass: while it runs, the thread knows which UI list is being drawn, and the D3D draw hooks
// (hooks/draw_trace_diag.cpp) count the draws that land inside it. If they do -- the D3D calls are made right there,
// not recorded for later -- this is the HUD's exact boundary, to replace the draw-order guesses in the final pass, and
// the place to give the HUD a render target of its own. Logged every 5 s to mohwvr_uipass.log.

namespace mohw {

bool InstallUiPassHook();
void RemoveUiPassHook();

// This thread is inside the UI pass for list 0 (before the world) or 1 (the HUD over the frame); -1 outside.
int CurrentUiPass();

// From the D3D draw hooks, every draw: counted when it lands inside the UI pass on this thread.
void UiPassNoteDraw();
// NATIVE TWO-VIEW (hooks/native_stereo.h, 2026-11-08): the HUD list is replayed once per half here, the way the
// engine's own stereo HUD replays it once per eye (FUN_00CBB4E0, OFFSET_UI_LIST_REPLAY). The half this thread is
// drawing it into: 0 left, 1 right, -1 none. hooks/draw_trace_diag.cpp places each replay in its eye's HUD rectangle.
int CurrentHudEye();
// Changes with every such replay, so the draw hooks can tell a new one started.
int CurrentHudReplay();

} // namespace mohw
