# The Basilan RHIB: 6DoF, the Mk19 by hand, the wheel (2026-11-22)

Code: `hooks/boat_seat.*`, `hooks/boat_math.h`. Settings: dashboard > Specialized sequences (the master, "Use 6DoF
Motion For Specialized Sequences") and > Sequence profile: Zodiac Sequence (below). Log: `mohwvr_boat.log`, always
written. Tested offline (the seat reading, sticks and shot function on the game's code, the other hooks stubbed); not
yet run in the game.

## Telling the sequence apart

All three must hold.

| What | Where | Value |
|---|---|---|
| Level | `[[0x02AC5FE4] + 0x10] + 0x68` (MOHW.exe+0x26C5FE4), char* | contains `030_Basilan` |
| Vehicle | controlled (player+0x448) or attached (+0x440) controllable, class `ClientVehicleEntity`; data = entity+0x14; ControllableType = data+0x114 | `MOHW/Vehicles/rhib/RHIB_SP` |
| Seat | entry id = player+0x44C / +0x444; component = `[[entity+0x7C] + id*4]`; data = component+4; EntryOrderNumber = data+0xB8 | 0 = wheel, 1 = Mk19 |

The level is needed: `049_Bainbridge` spawns the same blueprint six times and `050_Somalia` four, and Bainbridge fills
seat 0. In Basilan the script puts the player in seat 1, and checks for seats 0 and 1 in the drive.

The vehicle's world transform is entity+0x1C, or with flag bit 20 of entity+0x08 that collection + 0x10 + u8[+6] * 0x50
(as `FUN_00908630` reads it).

## Switched off

The master and the profile's 6DoF meet in `GetBoatSixDof`, so `GetBoatSeat` reports no seat with either off, and every
part of the handling asks that. Two things still read the raw seat: the log, and `NoPlayerForAWhile`, so a checkpoint
that loads into a seat keeps its stereo instead of being taken for the main menu.

A further sequence = its detection, rows in `kSequenceFields` (sdk/settings.cpp), a group of menu rows, and a 6DoF
getter that applies `GetSpecializedSequenceMotion`.

## The seats (RHIB_SP's data)

| | Wheel (order 0) | Mk19 (order 1) |
|---|---|---|
| Steer / yaw | LX | LX and RX |
| Pitch | | LY and RY |
| Throttle, brake / fire | the triggers | the triggers and bumpers |
| Camera | fixed to the boat, stick look +-90 / 25 deg | child of the turret, no look of its own |

The turret's bones `front_swivel` and `front_weapon` share one point (-0.0002, 1.9996, 3.4238 in the boat): one pivot
for both axes. From it, in view axes (right, up, back): the camera (0.007, 0.320, 0.900), tilted 4 degrees down from the
barrel; the middle of the handles (0.044, 0.0885, 0.258).

## What happens to the aiming

The mesh is never moved or re-pivoted. The pivot only converts hand movement into wanted angles, and the virtual stick
turns the game's own turret until its handles are there.

The Mk19's data has `ForceSpawnToCamera` and `SpawnVisualAtWeaponBone` set, `ForceSpawnToCameraUseInitialDirection` not.

- `FUN_00BB63A0` (MOHW.exe+0x7B63A0) takes the rotation from `FUN_008350A0` -> `FUN_00834F90`, the weapon's own
  transform with InitialDirection (0, 0, 1). That this transform is the barrel's is inferred from the component data:
  its writer wasn't found.
- It replaces only the position, with the seat's camera component (`FUN_00BB2D50`, `FUN_00B62570`).
- So the shot should run parallel to the barrel from 0.32 m above it. The visible round starts at the weapon bone and,
  going by VisualConvergeDistance 30, joins it by 30 m (that path wasn't traced). Projectile gravity is -7, speed 200.
- The committed view (which 6DoF turns) is returned by the renderer's vtable +0x8C (`0x008A8B60`: renderer + 0x50). Its
  nine direct callers are HUD / marker, LOD and renderer code, none of them weapon code. This is a static finding: a
  getter through a stored renderer pointer wouldn't show in that search.
- Both of the mod's shot redirects stand aside in a seat (`GetPlayerAimDriveWeight` is 0 there).

## Not proven until it's played

- That the grenades land where the barrel points (the inference above).
- Head look re-zeroing on getting up (`NoteLookTaken`): the test only sees the call, the aim hook is stubbed there.
- Which way the turret's animation graph answers each stick axis. It's learnt in the first degrees of movement and
  logged.
- The turret's speed and deadzone (settings: follow strength, stick deadzone compensation).
- Whether the first-person hands and gun are the batches `AnchorRigToHead` re-bases, or world meshes (then they need
  nothing).
- The wheel's placement: it's a virtual wheel set from the head, not the boat's drawn wheel. The profile has its own
  lock, placement and look, which start from the driving missions'.
