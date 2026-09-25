#pragma once
// In-process pose/view data hub. Originally the producer side of a companion-process bridge (a separate 64-bit
// OpenXR client); that process is retired -- openvr_direct.cpp is now the sole real producer, publishing directly
// into the overrides below instead of over cross-process shared memory. This file's remaining job is just those
// overrides plus the one still-live file-mapping read (the right controller pose, which has no in-process producer
// yet).

#include "../shared/ipc_protocol.h"

namespace mohw {

// Reads the current head pose. Returns false if no producer has published one yet.
bool GetHeadPose(mohwvr::ipc::HeadPoseBlock* out);

// Lets the in-process producer (openvr_direct.cpp) supply the real head pose every frame. Takes priority
// whenever set (which is effectively always, once connected).
void SetHeadPoseOverride(const mohwvr::ipc::HeadPoseBlock& block);

// The right motion controller's live "aim" pose (see shared/ipc_protocol.h's ControllerPoseBlock), read from a
// file-mapping a producer would publish. No in-process producer publishes this yet, so this currently always
// returns false -- kept because hooks/controller_trigger_hook.cpp and hooks/camera_matrix_test_hook.cpp both
// call it for a real feature (trigger-as-mouse-click) that would need a publisher wired up to work again.
bool GetRightControllerPose(mohwvr::ipc::ControllerPoseBlock* out);

// The headset's real per-eye FOV half-angles, IPD and swapchain size (see shared/ipc_protocol.h's HmdViewBlock).
// Returns false until an override has been set.
bool GetHmdView(mohwvr::ipc::HmdViewBlock* out);

// Lets the in-process producer (openvr_direct.cpp) supply the real per-eye HMD frustum. HMD frustum geometry is
// fixed per device/session, so the caller only needs to call this once after connecting, not every frame.
void SetHmdViewOverride(const mohwvr::ipc::HmdViewBlock& block);

void ShutdownCompanionBridge();

} // namespace mohw
