# When something else has the controls (2026-11-06)

Frostbite hands the player's input to a "controllable": the player's own soldier, or whatever they've entered or
taken over. MOHW's two problem sequences are both in **050_Somalia**: the MAARS robot (`b_050_somalia_maarsrobot`,
`levels/sp/050_somalia/animations/maars/maars_robotanim`) and the sniper setup (`b_050_somalia_snipersetup`, scripting
in `levels/sp/050_somalia/des_sam_sniping/layer1_des_sniper_scripting` and `des_snipersetup`, player loadout
`customizationdata/weapons/spec/lo_pl_som_sniper`). Asset names from `tools/fb2dump`.

## The player, this build (Ghidra)

| What | Where |
|---|---|
| ClientGameContext | `[0x02AC5FE4]` (OFFSET_CLIENTGAMECONTEXT) |
| ClientPlayerManager | context `+0x34` |
| the local ClientPlayer | manager `+0xB8` (`FUN_00901700` and many others) |
| m_soldier (weak pointer) | player `+0x42C`: `[[+0x42C]] - 4` is the soldier entity |
| m_attachedControllable, its entry id | player `+0x440`, `+0x444` |
| m_controlledControllable, its entry id | player `+0x448`, `+0x44C` |

`FUN_00908630` checks "controlled == own soldier" for being on foot. These match the MOHW column of the community
offsets file (`MOHW-BF3 offsets found online.txt`), not the BF3 one.

## What changed

- **`hooks/player_control`:** logs every change of what the player is attached to and controls, with each object's
  reflected type, to `mohwvr_control.log` (polled every 250 ms from Present). On foot it shows the soldier as the
  controlled object. The robot and the sniper post will show what they really are (a vehicle entity, an emplacement
  entry, or the soldier still), which says which aim and camera code applies there.
- **Shots:** the controller no longer steers shots while the soldier's own aim isn't running (`IsPlayerAimActive`:
  no AimingController update for 300 ms, live-confirmed for the robot section in October). Before, the last head-aim
  yaw/pitch stayed cached, and every bullet in the robot section, fired from the robot's gun and aimed by its camera,
  was turned toward the right controller and moved to the hand. That fits "shots not registering always".

## The sniper post, open questions for the next play-through

The symptoms: half the gun not rendered, no scope, and zoom shows the rifle at a fixed screen position.

- **What's controlled:** does the control log show the soldier still controlled there? If yes, the post is a scripted
  pose of the soldier with a forced weapon (`lo_pl_som_sniper`) and probably a locked camera. If it shows another
  entity, it's an emplacement-style entry with its own camera and zoom.
- **Is the soldier's aim running** (head look moves the view)? If not, the head-aim, weapon drive and laser are all
  off by design, and the gun is drawn where the game puts it.
- **Half the gun:** the weapon drive moves the first-person gun batches to the controller, but a scripted sniper pose
  may draw part of the rifle as a different mesh (a mounted, skinned body part), so half follows the hand and half
  doesn't. `mohwvr_cameramatrix.log`'s batch lines during the sequence will show which parts are being moved.
- **No scope:** MOHW's scopes are drawn by the weapon's zoom (a scope overlay or a scope mesh at the eye). With the gun
  on the controller, the scope isn't in front of the eye. The game's zoom view would need the gun returned to its own
  pose while zoomed (the weapon drive off), or the scope image drawn at the controller.

Next step: play the sequence once with this build, then read `mohwvr_control.log` and `mohwvr_aimingcontroller.log`
for that stretch.
