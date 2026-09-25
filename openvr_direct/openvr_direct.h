#pragma once
// Direct in-process 32-bit OpenVR connection (2026-09-22), replacing the retired 64-bit companion-process
// architecture now that SteamVR 2.17 supports 32-bit clients from within a real, Steam-launched game (see project
// memory: project_mohw_steamvr_32bit_direct_connection.md). Everything else (bone-hide, aiming, input, etc.) stays
// shared/reused from hooks/.
//
// CRITICAL: never statically link openvr_api.lib -- doing so previously made MOHW's own DRM refuse to launch the
// game at all ("unusual imports" in a dxgi.dll inside a DRM-protected process). Every OpenVR call here goes
// through LoadLibrary/GetProcAddress (C-ABI functions) or raw vtable indexing (C++ interfaces like IVRCompositor)
// instead. Re-verify with `dumpbin /imports` on the built dxgi.dll after any change here -- it must stay exactly
// d3d11.dll/USER32.dll/KERNEL32.dll, nothing else.
//
// THREADING: VR_Init is done ONCE on a background thread (ConnectThreadProc), because it's confirmed (live,
// repeatedly) to sometimes hang indefinitely -- must never block the game's render thread. WaitGetPoses and Submit
// are called together, paired, on a SEPARATE dedicated thread (SubmitThreadProc) that owns its own independent
// D3D11 device (g_ownDevice), never the game's. The game thread only CopyResource's each eye's fresh content into
// a shared texture (D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX); SubmitThreadProc opens that same texture via
// OpenSharedResource on its own device and submits it, paced by its own WaitGetPoses calls. This design was
// arrived at after two earlier, simpler attempts both failed: (1) running Submit() on a background thread but
// still against the GAME's device caused a DXGI_ERROR_DEVICE_REMOVED crash near a ResizeBuffers call; (2) keeping
// Submit() on the game thread but moving only WaitGetPoses to a background thread broke OpenVR's required
// WaitGetPoses+Submit pairing (alternating success/VRCompositorError_IndexOutOfRange, then a hang). A genuinely
// separate device on a dedicated thread is the only architecture that avoids both failure modes at once.

struct ID3D11Device;
struct ID3D11Texture2D;

namespace mohw::openvr_direct {

// Call once per Present, with the SAME per-eye textures present_hook.cpp already computed for UpdateCompanionEyes
// (companion_bridge.h) -- leftEye/rightEye, either of which may be nullptr meaning "this eye didn't render this
// frame, resubmit whatever this eye last had" (alternating-eye's temporal-stereo semantics; UpdateCompanionEyes
// treats null the same way). No-op until the background connection attempt succeeds. Safe to call every frame
// regardless of connection state.
void UpdateOpenVrDirect(ID3D11Device* device, ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye);

bool IsOpenVrDirectConnected();

} // namespace mohw::openvr_direct
