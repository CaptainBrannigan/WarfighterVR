#pragma once
// SteamVR dashboard tab ("MOHW VR") listing every adjustable setting (sdk/settings.h's menu table), driven by
// SteamVR's own laser pointer, plus the sight dots. IVROverlay_028 through the raw vtable, like the rest of
// openvr_direct. The menu is drawn with GDI into a memory bitmap and uploaded with SetOverlayRaw only when it changes,
// on its own thread, so it never touches the game's D3D device or stalls eye submission.

#include "../sdk/vr_math.h"

namespace mohw::openvr_direct {

// Connect thread, once. overlay = the IVROverlay_028 interface pointer; trackingUniverse = the compositor's tracking
// space (for the sight dots). Starts the overlay threads; returns false if the dashboard overlay couldn't be created
// (the game runs on without the menu).
bool InitVrOverlay(void* overlay, int trackingUniverse);

// Submit thread, every frame: places the optic dot (the weapon hand's forward turned by the optic zero, at
// OpticDistance) and the aim ray dot (straight along it, at SightZeroDistance), facing the head, all tracking space;
// or hides them (visible false, or each dot's own setting off).
void UpdateSightDot(bool visible, const float origin[3], const Quat& aimOrientation, const float headPos[3]);

} // namespace mohw::openvr_direct
