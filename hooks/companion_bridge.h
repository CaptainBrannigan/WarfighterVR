#pragma once
// In-process pose/view data hub. Originally the producer side of a companion-process bridge (a separate 64-bit
// OpenXR client); that process is retired -- openvr_direct.cpp (head/view) and openvr_direct/vr_input.cpp
// (controllers) are now the only producers, publishing directly into the overrides below.

#include "../shared/ipc_protocol.h"

namespace mohw {

// Reads the current head pose. Returns false if no producer has published one yet.
bool GetHeadPose(mohwvr::ipc::HeadPoseBlock* out);

// Lets the in-process producer (openvr_direct.cpp) supply the real head pose every frame. Takes priority
// whenever set (which is effectively always, once connected).
void SetHeadPoseOverride(const mohwvr::ipc::HeadPoseBlock& block);

// NUMPAD5 one-shot: arms a 300-call unconditional (unrated) burst of GetHeadPose staleness logging to
// mohwvr_headpose_staleness.log, instead of the normal 200ms-sampled rate -- see companion_bridge.cpp's
// LogStalenessIfDue comment.
void CheckHeadPoseStalenessBurstHotkey();

// Each motion controller's live aim pose (see shared/ipc_protocol.h's ControllerPoseBlock), published every
// submit-loop iteration by openvr_direct/vr_input.cpp. Get* returns false until the first publish, and whenever
// that hand isn't currently tracked.
bool GetRightControllerPose(mohwvr::ipc::ControllerPoseBlock* out);
bool GetLeftControllerPose(mohwvr::ipc::ControllerPoseBlock* out);
void SetRightControllerPoseOverride(const mohwvr::ipc::ControllerPoseBlock& block);
void SetLeftControllerPoseOverride(const mohwvr::ipc::ControllerPoseBlock& block);

// The headset's real per-eye FOV half-angles, IPD and swapchain size (see shared/ipc_protocol.h's HmdViewBlock).
// Returns false until an override has been set.
bool GetHmdView(mohwvr::ipc::HmdViewBlock* out);

// Lets the in-process producer (openvr_direct.cpp) supply the real per-eye HMD frustum. HMD frustum geometry is
// fixed per device/session, so the caller only needs to call this once after connecting, not every frame.
void SetHmdViewOverride(const mohwvr::ipc::HmdViewBlock& block);

} // namespace mohw
