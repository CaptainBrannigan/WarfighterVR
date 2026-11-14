#pragma once
// SCRIPTED SCENES FROM THE GAME'S OWN INPUT LOCKS (2026-11-12, Ghidra; sdk/mohw_offsets.h PLAYER_INPUT_ALLOWED_*).
// Frostbite takes the controls away in scripted moments with an InputRestrictionEntity: per input (Yaw, Pitch, Fire,
// Jump, ...) it clears one bit of a mask kept on the server player (FUN_00b245b0 -> FUN_00b2ac60), and sets them all
// back when it ends (FUN_00b22c80). The first-person rig's only signal so far was the soldier's aim stopping
// (GetPlayerAimDriveWeight), and the user: "No sequence in game has been immune to player ik once the camera is placed
// first person" -- the aim keeps running through them (frame trace 2026-11-12 10:43-10:45: "soldier's aim running" all
// through a cutscene). This reads the mask instead:
//   look (Yaw or Pitch) taken: head look goes onto the game's camera (fov_scale_hook.cpp's scripted look) instead of
//     into the aim, the heading guard stands aside, and the gun and arms go back to the game;
//   fire taken: the gun and arms go back to the game.
// Setting ScriptedScenesFromLocks (default on) switches the use off; the log runs either way.

namespace mohw {

// Game thread, every Present (MOHW.exe only): reads the human server player's mask; every change goes to
// mohwvr_control.log with the inputs taken.
void InputRestrictionOnPresent();

// Any thread. False when unknown (no level, the mask unreadable) or with the setting off.
bool IsLookRestricted();
bool IsFireRestricted();

// Any thread: how much the player owns the gun and arms as far as the locks go, 0..1. 1 while nothing that matters is
// taken; blends to 0 over 200 ms when look or fire is taken, and back 250 ms after both are given back, over 200 ms
// (as GetPlayerAimDriveWeight does for the aim stopping).
float GetInputLockDriveWeight();

} // namespace mohw
