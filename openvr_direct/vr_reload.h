#pragma once
// Manual reload (first draft 2026-10-31; sticky magazines 2026-11-14). Runs on the submit thread from vr_hands.cpp; the
// drawing is in hooks/camera_matrix_test_hook.cpp. No second mesh is needed: only one magazine shows at a time, so the
// gun's own magazine part is the mesh, drawn wherever the state says.
//
//   In the gun -> off-hand grip in the grab box -> on the path, the hand stuck to the magazine where it grabbed
//   On the path, coming out: nothing moves until the magazine release is pressed (the platform's needsRelease; the
//     SteamVR action "Magazine release", only while a magazine is gripped), then the controller pulls it straight down
//     the magwell: it moves once the pull passes the platform's pull force, follows that far behind, and at the end of
//     the path it's free -> held, staying where the hand had it
//   Held (it follows the hand at the offset it was grabbed with; firing stops) -> grip released -> out: it waits at
//     the pouch (a holster-style zone on the chest), turned by the pouch rotation
//   Out -> off-hand grip in the pouch -> held, now a new magazine, kept where it sat relative to the hand
//   Held, after it's been away from the gun, its insertion point (its Wep_Mag point) within the weapon profile's lock
//     range of the seat and up to "seats within" below it -> in the gun. The drawing turns it onto the magwell's line
//     from the profile's range and holds it on the line inside the lock range. (2026-11-18: going in is back to the
//     first version, the user's "very smooth" one -- no path, no push against a pull force; the AK's hinge and the
//     per-platform in-hand offsets are gone.)
// The hand comes back to its controller when the magazine leaves the path, or when the controller gets more than
// MagazineGripBreak from it (the grip lets go).
//
// A new magazine seating presses the game's own Reload and takes no time (its reload states 0xB / 0xC wait on a time
// that reads as 0, FUN_00831080, and on the timer at +0xC4 for the rest), with its start never announced to the animation
// (FUN_008373E0, both hooks/first_person_rig.cpp), so
// the ammo counts stay the game's and nothing plays. A reload the game starts itself (the Reload button, the auto-reload
// when empty) goes the same way, and counts as the magazine dropping out.

#include "vr_hands.h"
#include "../sdk/rigid.h"

namespace mohw::openvr_direct {

enum class MagazineState : int
{
    InGun,
    Held,   // old or new, in the off hand
    Out,    // at the pouch
    OnPath, // gripped on the gun: seated and held, coming out, or going in
};

// Render side. pouchFromHead: the pouch's centre, tracking space, from the head. Axes: the body's right / up / back.
// Frames named ...Gun are gun-local (vr_magazine_path.h): the gun's frame at the magazine's seat.
struct MagazineView
{
    MagazineState state = MagazineState::InGun;
    float pouchFromHead[3] = {};
    float bodyRight[3] = {1, 0, 0};
    float bodyBack[3] = {0, 0, 1};
    // OnPath: where along the path (0 seated .. 1 clear), and the off hand stuck to the magazine: its controller frame
    // in the magazine's own frame, so it's at handInMag * PathDisplacement(s) in gun-local.
    float s = 0.0f;
    Rigid handInMag = RigidIdentity();
    // Held: the magazine in the off hand, mag-local -> that controller's frame (the offset it was grabbed with).
    Rigid magInHand = RigidIdentity();
    // A change that leaves the hand or the magazine somewhere else (it left the path, seated, the grip let go): both
    // glide from where they were, gun-local, over kMagazineBlendMs from blendStartMs.
    unsigned long long blendStartMs = 0;
    bool blendHand = false, blendMag = false;
    Rigid blendHandFromGun = RigidIdentity();
    Rigid blendMagFromGun = RigidIdentity(); // mag-local -> gun-local
};
constexpr unsigned long long kMagazineBlendMs = 160;
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
// The latest of those (any thread); false before the first, or when no magazine has been drawn for a while.
bool GetMagazineGeometry(MagazineGeometry* out);

// vr_hands.cpp, submit thread.
void UpdateManualReload(const HandInput hands[2], int weaponHand, bool haveHead, const float headPos[3],
                        const Vec3& pouchCentre, const Vec3& bodyRight, const Vec3& bodyForward);
// True if the grip press was the magazine's (the rest of the grip logic then skips it).
bool ManualReloadGripPressed(const HandInput& weapon, const HandInput& off, const Vec3& pouchCentre, bool haveHead);
void ManualReloadGripReleased(const HandInput& weapon, const HandInput& off);
// vr_input.cpp, submit thread: the hand that grips a magazine now (on the gun or held), or -1 -- the "Magazine" action
// set is active for that hand only -- and the magazine release's state, read from it before UpdateHands.
int GetMagazineGripHand();
void SetMagazineReleaseInput(bool down);
// The magazine's setup in tracking space, for the grab zone view. False while manual reload isn't running. inBox: the off
// hand is in the grab box; handDistance: from the held magazine's insertion point to the seat; entry: the path's far end.
struct MagazineDebug
{
    Vec3 seat, aim, boxCentre, axes[3];
    Vec3 entry, insertion;
    float boxHalf[3];
    float range, lockRange, handDistance;
    bool inBox;
};
bool GetMagazineDebug(const HandInput& weapon, const HandInput& off, MagazineDebug* out);
bool ManualReloadBlocksFire();
// Any thread: whether this WeaponFiring's reload shouldn't reach the animation: the player's, with manual reload running.
bool ManualReloadCancelsAnimation(void* firing);

} // namespace mohw::openvr_direct
