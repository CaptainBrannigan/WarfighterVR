#pragma once
// SteamVR dashboard tab ("MOHW VR") listing every adjustable setting (sdk/settings.h's menu table), driven by
// SteamVR's own laser pointer. IVROverlay_028 through the raw vtable, like the rest of openvr_direct. The menu is
// drawn with GDI into a memory bitmap and uploaded with SetOverlayRaw only when it changes, on its own thread, so it
// never touches the game's D3D device or stalls eye submission.

namespace mohw::openvr_direct {

// Connect thread, once. overlay = the IVROverlay_028 interface pointer. Starts the overlay thread; returns false if
// the dashboard overlay couldn't be created (the game runs on without the menu).
bool InitVrOverlay(void* overlay);

} // namespace mohw::openvr_direct
