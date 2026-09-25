#pragma once
// Task: "make the controller trigger act as a left mouse input for the
// time being" -- a stand-in fire control while motion-controller aiming is
// being tested, ahead of a real in-game binding. Polls the right
// controller's trigger analog value (shared/ipc_protocol.h's
// ControllerPoseBlock::triggerValue, published by
// companion/main.cpp's SyncAndPublishControllerPose) once per frame and
// synthesizes a real OS-level left mouse button down/up via SendInput on
// each threshold crossing -- not held down continuously every frame, so it
// behaves like a normal single click-and-hold rather than spamming
// mousedown events.

namespace mohw {

// Safe to call every frame (e.g. from present_hook.cpp's Present hook,
// alongside EnsureCompanionProcess()); no-ops if the companion isn't
// running or hasn't published a controller pose yet.
void UpdateControllerTriggerMouseInput();

} // namespace mohw
