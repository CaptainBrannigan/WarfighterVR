#pragma once
// EXPERIMENTAL (2026-09-21): real head translation -> in-game camera translation ("room-scale lean/step/crouch").
//
// ApplyHeadPosition is called once per game frame from the CommitViewTransform hook. It takes the HMD position delta since the
// last recenter (F3, same anchor as head-aim; XR local space, meters), rotates it into the game world with the SAME yaw
// alignment the head-aim mapping establishes, scales it (HeadPositionScale, 1.0 = 1 m -> 1 game unit) and adds it to the
// camera transform's translation. Only the render camera moves; the character body/collision do not (yet).
// Toggle: Pause/Break (saved to mohwvr_settings.ini as HeadPositionEnabled).

namespace mohw {

void ApplyHeadPosition(void* transformPtr);

// The head position (XR local space, meters) most recently APPLIED to the camera, if head-position tracking is on and worked
// this frame. Used by the render-pose stamp so the compositor knows where the head was when the frame was rendered.
bool GetLastAppliedHeadPosition(float out[3]);

} // namespace mohw
