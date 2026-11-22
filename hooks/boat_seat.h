#pragma once
// THE BASILAN RHIB, in the menu the "Zodiac Sequence": one of the specialized sequences, with its own profile, under
// "Use 6DoF Motion For Specialized Sequences" (2026-11-22, the user: "6dof while locking the ik hands to their default
// position, then ... motion controller aiming of the grenade launcher as well as motion control driving and 6dof"). A
// requested exception to the scripted-scene rule (head and control hooks off, nothing scene-specific): these two seats
// only, and that rule still wins whenever the game takes look away in them.
//
// WHICH SEQUENCE (game data through tools/fb2dump, code through Ghidra). All three have to hold:
//   the level     [[ClientGameContext] +0x10] +0x68, its name, is .../030_Basilan (FUN_0092fc50 copies that string)
//   the vehicle   the player's controlled (or attached) controllable is a ClientVehicleEntity whose data's
//                 ControllableType is MOHW/Vehicles/rhib/RHIB_SP
//   the seat      its entry's data has EntryOrderNumber 0 = the wheel (DriverComponent), 1 = the bow's Mk19 turret
// The level is needed: 049_Bainbridge and 050_Somalia spawn the same RHIB_SP, and Bainbridge fills its seat 0.
//
// IN A SEAT
//   6DoF        the committed camera is the seat's eye point plus the head, over a level base that turns with the boat.
//               Straight ahead is where the seat faced on sitting down, on a recenter, or when look came back. On the
//               Mk19 the eye point is held from the gun's pivot, so the gun turning under the hands doesn't move it.
//   the hands   the game's own animation, on the wheel or the gun: no weapon drive, IK, holsters or reload; the
//               first-person batches are drawn from the head's camera so they stay in the boat.
//   the Mk19    the gripping hands lever the gun about its pivot (hooks/boat_math.h Lever): down and left sends the
//               barrel up and right. The virtual pad's stick is fed until the handles are where the hands put them (the
//               seat's input map: LX / RX yaw, LY / RY pitch). Not gripping, the thumbstick turns it and the view comes
//               round with it. The mesh is never moved: the pivot only says which angle the hands ask for.
//   the shot    not the barrel's line but one parallel to it (the user, 2026-11-22: "what happens to the aiming?").
//               The Mk19's data has ForceSpawnToCamera (with SpawnVisualAtWeaponBone), and FUN_00bb63a0 then takes the
//               DIRECTION from the weapon's own transform (FUN_008350a0) and only the POSITION from the seat's camera
//               component (FUN_00b62570): 0.32 m above the barrel. Not the committed view, which the head turns: that
//               is reached through the renderer's vtable +0x8C, which no weapon code calls directly.
//   the wheel   the driving missions' steering code (openvr_direct/vr_steering.cpp) as left stick X (the seat's input
//               map: LX steer, the triggers throttle and brake).

#include "../sdk/rigid.h"

namespace mohw {

enum class BoatSeat
{
    None = 0,
    Driver,
    Gunner,
};

// Game thread, every Present: which seat, logged on every change (mohwvr_boat.log, always written).
void BoatSeatOnPresent();

// Any thread: the seat the player is in, whatever the settings say.
BoatSeat GetBoatSeatRaw();
// Any thread: the seat while its handling is on (sdk/settings.h GetBoatSixDof: the master and the profile's). Everything
// that steps aside for the boat asks this; only the level's stereo (hooks/draw_trace_diag.cpp) asks the raw one.
BoatSeat GetBoatSeat();
// Any thread: the boat's game camera has stopped changing (the pause menu; it always moves otherwise).
bool IsBoatPaused();

// The submit thread, every input update in a seat: each hand's palm in tracking space, and whether it grips.
void NoteBoatHands(const bool gripping[2], const float position[2][3]);

// The CommitViewTransform hook, on its copy of the game's view (hooks/fov_scale_hook.cpp), after the smoothing and
// before the render-pose stamp: writes the head's camera. gameCamera = that copy's transform before any edit.
// False = not in a seat, or the game has look.
bool ApplyBoatHead(float* view, const float gameCamera[16]);
// Any thread: the head's camera relative to the game's raw one (head camera = delta, then the raw camera), for the
// first-person batches. False while there's none.
bool GetBoatHeadDelta(Rigid* delta);
// The head pose the last committed boat frame was rendered from, for the render-pose stamp. False while there's none.
bool GetBoatRenderPose(float quat[4], float position[3]);

// The input thread: the right stick that turns the turret until it's where the hands put it. False = the thumbstick's.
bool GetBoatGunStick(float* x, float* y);

} // namespace mohw
