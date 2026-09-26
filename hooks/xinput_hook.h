#pragma once
// Presents the VR thumbsticks to the game as XInput controller 0 by hooking XInputGetState (xinput1_3.dll, which the
// game polls from 0x008DCDF0 -- see project memory project_mohw_frostbite.md's gamepad pipeline). The game then
// treats them as genuine gamepad input all the way through its own pipeline: movement, prediction, turning, aim assist.
//   left stick  <- Move (SteamVR Input, left stick by default)
//   right stick <- Turn X, scaled by sdk/settings.h's GetVrTurnSpeed; right-stick Y always 0 (pitch is head-only)
// While a VR stick is centred, a real gamepad's own value for that stick passes through untouched. With no real
// gamepad, controller 0 reports as a connected, idle pad from the moment the hook is in (XInputGetState and
// XInputGetCapabilities both), since the game may only look for controllers before the VR connection is up.

namespace mohw {

bool InstallXInputHook();
void RemoveXInputHook();

} // namespace mohw
