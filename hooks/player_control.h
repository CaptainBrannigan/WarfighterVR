#pragma once
// WHO THE PLAYER CONTROLS (2026-11-06, Ghidra; docs/control_handoffs.md). Frostbite hands the player's input to a
// "controllable": their own soldier, or whatever they've entered (a vehicle, the robot, a fixed weapon post).
// ClientPlayer, this build:
//   [[DAT_02AC5FE4 (ClientGameContext)] + 0x34] = ClientPlayerManager, + 0xB8 = the local ClientPlayer
//   player + 0x42C  m_soldier, a weak pointer: [[+0x42C]] - 4 = the soldier entity (FUN_00908630)
//   player + 0x440  m_attachedControllable (what the player sits in or has entered), + 0x444 its entry id
//   player + 0x448  m_controlledControllable (what the input drives), + 0x44C its entry id
// FUN_00908630 treats "controlled == own soldier" as on foot.

#include <cstdint>

namespace mohw {

struct PlayerControl
{
    uintptr_t player = 0, soldier = 0, attached = 0, controlled = 0;
    uint32_t attachedEntry = 0, controlledEntry = 0;
};

// SEH-safe, any thread. False outside a level (no local player).
bool GetPlayerControl(PlayerControl* out);

// Game thread, every Present: every change of what the player is attached to and controls is logged with the objects'
// reflected types (mohwvr_control.log), to find the robot section's and the sniper post's objects.
void PlayerControlOnPresent();

// Any thread, from the last PlayerControlOnPresent: true while the player drives their own soldier (or nothing is
// known), false while the input drives something else. Logged only so far: the shots go by the soldier's aim running
// (IsPlayerAimActive, live-proven for the robot section) until these offsets have been seen live.
bool IsControllingOwnSoldier();

} // namespace mohw
