#pragma once
// EXPERIMENTAL (2026-09-21): render-pose stamping. Smooths head-turn judder by telling the compositor which head orientation
// each game frame was actually rendered for, so it can reproject the (small) difference to the current head pose.
//
//  * ComputeRenderPoseStamp -- call once per game frame from the CommitViewTransform hook AFTER the camera rotation is final
//    (smoothing + roll + pair freeze). Reconstructs the head orientation that rotation corresponds to by inverting the
//    head-aim mapping (aiming_controller_hook.cpp) and rotating the CURRENT HMD pose onto that facing.
//  * PublishRenderPoseForEye -- call from the Present hook, after the frame has been routed to an eye, to publish the pending
//    stamp for that eye through shared memory (shared/ipc_protocol.h RenderPoseBlock).
// The companion applies it only while its own toggle is on (companion/main.cpp, '/' key).

namespace mohw {

void ComputeRenderPoseStamp(void* transformPtr);
void PublishRenderPoseForEye(bool rightEye);

// In-process getter for openvr_direct.cpp (2026-09-23): the original consumer of this stamp was the
// now-retired 64-bit companion, which read it back out of the RenderPoseBlock IPC shared-memory block
// via OpenXR's per-eye-view pose. openvr_direct.cpp runs in-process and uses classic OpenVR instead,
// which has its own equivalent mechanism (VRTextureWithPose_t / Submit_TextureWithPose -- "Allows
// specifying pose used to render provided scene texture (if different from value returned by
// WaitGetPoses)", confirmed present in third_party/openvr/headers/openvr.h), so this reads the SAME
// per-eye stamp PublishRenderPoseForEye already tracks, directly, with no IPC needed at all. Mirrors
// companion/main.cpp's ApplyPoseStamp staleness convention (a stamp older than ~30 calls without a
// fresh publish for that eye is treated as unavailable -- menu/loading/paused). Returns false if no
// fresh stamp exists for this eye; callers should fall back to the plain live HMD pose in that case.
bool GetPendingRenderPoseStamp(bool rightEye, float outQuat[4], float outPos[3], bool* outPosValid);

// Debug toggle (NUMPAD4, default ON) to isolate whether the pose-stamping mechanism itself is the source
// of a live-observed faint single-frame ghost during head turns under openvr_direct's classic-OpenVR
// Submit_TextureWithPose port -- when off, GetPendingRenderPoseStamp always returns false so
// openvr_direct.cpp falls back to plain Submit_Default for both eyes, same as before this port existed.
// Checked unclaimed against hooks/ (2026-09-24).
void CheckRenderPoseStampDebugHotkey();

} // namespace mohw
