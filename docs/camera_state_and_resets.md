# The game's own camera state, and what has to reset after a level (2026-11-10)

A tester on v1.2 sent two log sets with "crashes in levels". Neither was a crash in the mod's code. Both pointed at the
mod's camera edits reaching state the game keeps, and at mod state that outlived the soldier's aim.

## The reports

**2026-11-07: fell through the floor after a cutscene.**

- 23:20:39: the player stands at (-370.3, 218.1, -176.2).
- 23:20:40 to 23:21:08: a cutscene camera flies around (up to y 432).
- 23:21:08: back on the same spot.
- From 23:21:09 the camera drops straight down, x and z unchanged, accelerating like a free fall: 205.9 at 23:21:09,
  -1,222 at 23:21:20, -17,576 at 23:23:57, the last line. The game died far below the level.
- Most of the session's logs were lost: the game was started twice more before they were sent, and one previous
  session is kept per log.

**2026-11-09: stuck after a reload.**

- 00:48:07: a new mission starts.
- 00:48:16-24: about 8 s with no first-person body (a cutscene).
- 00:48:24: the player comes back 16 m higher and sinks, accelerating, until the soldier's aim stops at 00:48:28.
- 00:48:38: the game leaves the level. 00:48:59: it loads back at the checkpoint, but the soldier's aim never runs
  again.
- 00:49:35 and 00:49:51: the game drops out of fullscreen (focus loss). 00:49:59: a normal process exit (the proxy's
  "detaching: process exit"), most likely the tester closing it.

The user: "our head tracking is forcing the capsule through the map, that's the only thing that affects the player
capsule", and "we aren't resetting some data on death. I have noticed that if I quit to main menu, the screen would
stop following my head".

## Fix 1: the game's view is never written

`hooks/fov_scale_hook.cpp`'s CommitViewTransform hook (`FUN_008AB580`) edited its argument in place: the eye's fov,
the smoothed rotation, head roll, head position, the eye offset. That argument is the game's own, stable view source.

- The function's bytes (with the mod's hook on its first five): `MOV ESI,[EBP+8]` (the argument), `LEA EDI,[EBX+0x50]`,
  `MOV ECX,0x394`, `REP MOVSD` at 0x008AB5A4, then `LEA ESI,[ESP+0x30]` for a second copy from its own stack.
- `[EBP+8]` is read nowhere else in the function (0x008AB580-0x008AB8AF, a byte scan for every `[EBP+8]` form). So
  all it does with the view is copy 0x394 dwords into renderer+0x50.
- Now the hook copies the view (16-byte aligned, SEH-guarded), makes every edit on the copy, and commits the copy. The
  renderer gets exactly what it got before, and the game's own camera state never carries the head's translation, the
  eye shift or the roll. The driving missions' hook (`FUN_00926dd0`, 0x290 dwords) has worked this way from the start.
- If the copy fails, the view passes through untouched (logged).

On master (v1.2) head position already skipped frames without the soldier's aim, so there the game's view carried
the eye offset and roll during the cutscene, and the head's translation once control came back.

## Fix 2: no stale render pose after a level

`hooks/render_pose_stamp.cpp`: each frame is submitted to SteamVR with the head pose it was rendered for (the stamp),
so SteamVR reprojects only the lag.

- With no stamp for a frame (menus, a death, a loading screen, the robot section: the soldier's aim isn't running),
  `PublishRenderPoseForEye` returned without touching the eye's stored stamp. Only the other eye's publish ages a
  stamp, so nothing did.
- So `GetPendingRenderPoseStamp` went on handing Submit the last gameplay pose. SteamVR reprojected every later frame
  against that frozen pose, and the head-locked image stayed put in the world: "the screen would stop following my
  head", until a level gave the aim back. In the 2026-11-09 stall the image would have looked frozen in place.
- Now a frame without a stamp clears that eye's stamp, and it's submitted with the live pose.

## Fix 3: the frustum-match log, removed

`hooks/projection_aspect_hook.cpp`'s "TRUE FRUSTUM MATCH CHANGED" logged every change of the matched camera. In a scene
with many matching cameras that was over a thousand lines a second: the tester's `mohwvr_fovscale.log` reached 33 MB
in under three minutes and rotated everything before it away. It was a 2026-09-21 ghosting diagnostic (that ghosting
was solved by the player-position filter) and nothing read it, so it's gone; the periodic frustum summary stays.

## Not affected: head-driven turning

The commit hook never carried the head's yaw. That goes into the AimingController itself
(`hooks/aiming_controller_hook.cpp`: the game's yaw plus the head's offset, written at +0xC), and the game builds both
the camera and the soldier's facing and movement from it. Shots and the laser take the game's camera before any edit.

## Scripted scenes: the game's input locks (2026-11-12)

The user: "No sequence in game has been immune to player ik once the camera is placed first person", and "The player
camera doesn't follow the head and when looking around I can see the borders of the scene". The only signal for a
scripted stretch was the soldier's aim stopping (`GetPlayerAimDriveWeight`, 2026-11-08), but the aim keeps running
through most of them: the 2026-11-12 frame trace says "soldier's aim running" in every frame of a cutscene, including
12 s with no first-person body drawn.

**The game's own signal (Ghidra).**
- Frostbite takes the controls away with a `ServerInputRestrictionEntity`. Its data (`InputRestrictionEntityData`,
  type record `02B73808`) is one Boolean per input: Throttle, Strafe, Yaw, Pitch, Fire, Zoom, Jump, Reload, PeekAndLean
  and so on, false = taken.
- Applying it (`FUN_00b245b0`) calls `FUN_00b2ac60(input, 0)` on the player for each one taken. Ending it
  (`FUN_00b22c80`) calls it with 1 for all 74 inputs. The friendly-fire game over (`FUN_00ee0340`) takes Fire the same
  way.
- `FUN_00b2ac60` keeps one bit per input on the player, set = allowed: inputs 0..11 at +0xC0, 12..73 in the 64-bit
  word at +0xC8. `FUN_00b2ac00` reads it back. Yaw = 4, Pitch = 5, Fire = 9.
- The player is the server's: `[[0x2AEAF8C]+0x30]` +0x78..+0x7C is the player list (`FUN_00b264b0`), and the human
  one has +0x354 == 0 (also checked by the idle check `FUN_00e1e000`).

**What the mod does** (`hooks/input_restriction.cpp`, setting View > "Scripted scenes follow the game's input locks",
`ScriptedScenesFromLocks`, on by default):
- Reads the mask every Present and logs each change to `mohwvr_control.log`, with the inputs taken ("INPUT LOCKS ...").
  A mask that reads 0 is taken as not set up, not as everything taken.
The mod's head and control hooks step aside while the game has the scene, leaving only the stereo rendering (the
eyes' frusta, the eye offset, the HUD placement, rotation smoothing with its pair freeze). The user (2026-11-12): "just
toggle our head and control hooks, leaving only the stereo rendering enabled".

- **Look taken (Yaw or Pitch): the head stands aside.** One predicate, `IsHeadDrivingSoldier()`
  (`aiming_controller_hook.h`: the soldier's aim runs and look isn't taken), gates every head hook:
  - the aiming controller update is passed through untouched: no head look written (`ApplyHeadAim` keeps only its
    bookkeeping, so the aim picks up cleanly when look comes back), no stored-heading guard (so the game's placing and
    turning of the player isn't undone; skipping it also breaks its continuity, so the heading the scene ends on is
    accepted as the game's), no pitch-input or fire-pull suppression;
  - no head position, no head roll (and so no roll on the first-person rig either);
  - no render-pose stamp: the frame is submitted with the live pose, so it's shown strapped to the head. Exactly what
    already happened when the aim stops altogether (the robot section, menus, a death);
  - the virtual pad's stick pitch passes through.
- **Look or fire taken: the controls stand aside.** The gun and arms go back to the game's animation, blended out over
  200 ms and back in 250 ms after both are given back (`GetInputLockDriveWeight`, combined with the aim-stopped weight
  in `GetPlayerAimDriveWeight`). The reticle comes back and the hidden arm parts are drawn. Below a weight of 0.5 (the
  reticle's threshold) the shots, the laser and the sight dots go back to the game's aim too.
- **The body.** The user: "both my head movement and the game's camera adjustment move the body around in ingame
  cutscenes". Two camera-relative adjustments stand aside while the rig is the game's (`GetPlayerAimDriveWeight` < 1):
  - `MoveBody`'s Body offset (0.3 m forward by default) and "Keep body upright" turn are along the camera's facing,
    which in a scene is the game's animated camera: the offset fades out with the weight, and the upright turn stops
    below 0.5;
  - `AlignRigToBones` ("Keep body and gun steady on head turns") eases the rig's turn against the camera, which made an
    animated body lag and drift: skipped.
- **Replaced (same day): a scripted look.** The first version turned the game's camera by the head in these scenes
  (yaw = the camera's own + the head's offset, pitch = the head's), stamped the frame with that head pose, turned the
  first-person rig to match, and mapped head position and the controllers through the scene camera's heading. It worked
  for look, but the head's translation still moved the camera ("the camera still follows our world position"), and it
  was specialized code where a toggle does. Removed.
- Not handled: a scripted camera that doesn't take look away keeps head tracking.

## Still open

- Whether the 2026-11-09 stall was the game's own (a checkpoint soft-lock) or something of the mod's. With fixes 1 and
  2, a repeat would show it.
- A crash handler (the faulting module and offset, a minidump through dbghelp loaded at runtime), more than one
  previous session's logs, and the level's name in a log would make the next report much easier to read.
