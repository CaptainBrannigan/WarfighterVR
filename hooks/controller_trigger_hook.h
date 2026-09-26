#pragma once
// Turns the SteamVR Input "Fire" action (openvr_direct/vr_input.h's VrActionState::fire, default binding: right
// trigger) into a real OS-level left mouse button down/up via SendInput, sent only on press/release edges so it
// behaves like a normal click-and-hold.

namespace mohw {

// Safe to call every frame (present_hook.cpp's Present hook); no-ops until SteamVR Input is running.
void UpdateControllerTriggerMouseInput();

} // namespace mohw
