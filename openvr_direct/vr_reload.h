#pragma once
// Manual reload, first draft (2026-10-31). Runs on the submit thread from vr_hands.cpp; the drawing is in
// hooks/camera_matrix_test_hook.cpp. No second mesh is needed: only one magazine shows at a time, so the gun's own
// magazine part is the mesh, drawn wherever the state says.
//
//   In the gun -> off-hand grip near the magwell -> held (it follows the hand, firing stops)
//   Held, grip released away from the gun -> out: the magazine waits at the pouch (a holster-style zone on the chest)
//   Out -> off-hand grip in the pouch -> held, now a new magazine
//   Held near the magwell -> its top turns to the aim point, its horizontal locks, and pushing it up seats it -> in the gun
//
// A new magazine seating presses the game's own Reload and cuts it short (WeaponFiring's reload states 0xB / 0xC wait
// on its timer at +0xC4), with its start never announced to the animation (FUN_008373E0, hooks/first_person_rig.cpp), so
// the ammo counts stay the game's and nothing plays. A reload the game starts itself (the Reload button, the auto-reload
// when empty) goes the same way, and counts as the magazine dropping out.

#include "vr_hands.h"

namespace mohw::openvr_direct {

enum class MagazineState : int
{
    InGun,
    Held, // old or new, in the off hand
    Out,  // at the pouch
};

// Render side. pouchFromHead: the pouch's centre, tracking space, from the head. Axes: the body's right / up / back.
struct MagazineView
{
    MagazineState state = MagazineState::InGun;
    float pouchFromHead[3] = {};
    float bodyRight[3] = {1, 0, 0};
    float bodyBack[3] = {0, 0, 1};
};
MagazineView GetMagazineView();
// The camera hook, each frame a magazine part is drawn, all in the weapon controller's frame (right, up, back, m): the
// magazine's seat, its aim point, and the gun's own right / up / back axes.
struct MagazineGeometry
{
    float seat[3];
    float aim[3];
    float axes[3][3];
};
void PublishMagazineGeometry(const MagazineGeometry& geometry);

// vr_hands.cpp, submit thread.
void UpdateManualReload(const HandInput hands[2], int weaponHand, bool haveHead, const float headPos[3],
                        const Vec3& pouchCentre, const Vec3& bodyRight, const Vec3& bodyForward);
// True if the grip press was the magazine's (the rest of the grip logic then skips it).
bool ManualReloadGripPressed(const HandInput& weapon, const HandInput& off, const Vec3& pouchCentre, bool haveHead);
void ManualReloadGripReleased(const HandInput& weapon, const HandInput& off);
// The magazine's setup in tracking space, for the grab zone view. False while manual reload isn't running. inBox: the off
// hand is in the grab box; handDistance: from the off hand to the seat.
struct MagazineDebug
{
    Vec3 seat, aim, boxCentre, axes[3];
    float boxHalf[3];
    float range, lockRange, handDistance;
    bool inBox;
};
bool GetMagazineDebug(const HandInput& weapon, const HandInput& off, MagazineDebug* out);
bool ManualReloadBlocksFire();
// Any thread: whether this WeaponFiring's reload shouldn't reach the animation: the player's, with manual reload running.
bool ManualReloadCancelsAnimation(void* firing);

} // namespace mohw::openvr_direct
