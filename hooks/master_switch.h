#pragma once
// Master switch (2026-10-15): the mod only acts with a headset. At startup nothing is hooked; a background thread waits
// for SteamVR to be running (it never launches it) and then tries the VR connection, which only succeeds with a
// headset connected (VR_IsHmdPresent alone said yes with none). Connected: installHooks runs, once, and everything
// stays on for the rest of the session. Otherwise the game runs completely unmodified and the check repeats every few
// seconds, so a headset connected later switches the mod on then. Only a game restart without a headset leaves it off.

namespace mohw {

// dllmain, once, after the module/build checks and LoadSettings. installHooks: installs every hook and starts the
// mod's threads (called on the switch's thread, at most once, with the VR connection already up).
void StartMasterSwitch(void (*installHooks)());

// Any thread: whether the mod has switched on.
bool IsModActive();

} // namespace mohw
