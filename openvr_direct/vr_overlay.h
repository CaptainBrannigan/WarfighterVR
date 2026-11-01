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

// The grab zone view (ShowGrabZones): one overlay per slot, tracking space. A billboard faces the head at pos; otherwise
// m is its 3x4 (columns = its axes, +Z its face). active: a hand is in it, drawn green.
constexpr int kDebugZoneFirst = 0;  // the seven holster zones, in vr_hands.cpp's kZones order
constexpr int kDebugPouch = 7;
constexpr int kDebugBoxFirst = 8;   // the magazine grab box's six faces
constexpr int kDebugTurnRange = 14;
constexpr int kDebugLockRange = 15;
constexpr int kDebugSeat = 16;
constexpr int kDebugAimPoint = 17;
constexpr int kDebugTwoHand = 18;
constexpr int kGrabDebugSlotCount = 19;
struct GrabDebugShape
{
    bool visible = false;
    bool billboard = false;
    bool active = false;
    float pos[3] = {};
    float m[12] = {};
    float width = 0.0f, height = 0.0f; // metres
};
// Submit thread, every frame while the view is on, and once to clear it.
void UpdateGrabDebug(const GrabDebugShape shapes[kGrabDebugSlotCount], const float headPos[3]);

// Submit thread, every frame: places the optic dot (the weapon hand's forward turned by the optic zero, at
// OpticDistance) and the aim ray dot (straight along it, at SightZeroDistance), facing the head, all tracking space;
// or hides them (visible false, or each dot's own setting off).
void UpdateSightDot(bool visible, const float origin[3], const Quat& aimOrientation, const float headPos[3]);

// Submit thread, every input update in the driving missions: the steering wheel (openvr_direct/vr_steering.cpp) -- a
// ring of radius and thickness (metres) at centre, in the plane of right / up (tracking space, unit vectors, up = to
// its top), turned by angle (radians, + = clockwise as the driver sees it), drawn at opacity (0..1); or hidden.
void UpdateWheelOverlay(bool visible, const Vec3& centre, const Vec3& right, const Vec3& up, float angle, float radius,
                        float thickness, float opacity);

} // namespace mohw::openvr_direct
