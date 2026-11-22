#pragma once
// Steering wheel for the driving missions (game\MOHWD.exe, 2026-10-14) and, since 2026-11-22, the Basilan RHIB's wheel
// seat in MOHW.exe (hooks/boat_seat.h). A virtual wheel fixed in the room: its centre
// is set from the head at the level's first pose and at every recenter (DrivingWheelForward / Down / Right along the
// head's heading then, tilted DrivingWheelTilt; at the RHIB, every Wheel setting here is the Zodiac Sequence profile's
// own, sdk/settings.h GetBoatWheel*) and never follows the head after that. While a hand's grip is held, that
// hand turns the wheel: its angle around the centre, in the wheel's plane. With both hands on, the wheel turns by the
// average of the two hands' turns. Released, the wheel springs back to centre. The turn, over DrivingWheelLock degrees
// for full lock, goes out as left-stick X (hooks/xinput_hook.cpp). The wheel is drawn as a world overlay (rim with an
// up marker, turning with it; vr_overlay.cpp UpdateWheelOverlay) while driving.

#include "vr_hands.h"

namespace mohw::openvr_direct {

// Submit thread, every input update in the driving executable (g_drivingExe) or at the RHIB's wheel. head:
// tracking-space head position and orientation (haveHead false if there's no head pose yet).
void UpdateSteering(const HandInput hands[2], bool haveHead, const float headPos[3], const Quat& headOrientation);
// Submit thread, on leaving the RHIB's wheel: lets go, takes the wheel away, and sets it afresh the next time.
void StopSteering();

// Any thread: the wheel's steering as left-stick X, -1..1 (+ = right). False while no hand holds the wheel (the
// thumbstick steers then) or the wheel is off (DrivingSteeringWheel).
bool GetSteeringWheelStick(float* x);

} // namespace mohw::openvr_direct
