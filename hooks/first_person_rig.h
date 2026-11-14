#pragma once
// The first-person skeleton (2026-10-31): the arms on the motion controllers (two-bone IK, ported from the AvP mod's
// hooks/first_person.cpp) and which weapon parts are the magazine. Render side only.
//
// HOW THE GAME DRAWS THE RIG (FUN_008BB050, per soldier, on a job thread; live-independent, from the code): each batch
// (hooks/camera_matrix_test_hook.cpp) is a 0xF0-byte "worldRenderPrimitive". +0x50 = its "soldierBoneTransforms", a
// frame-pool copy of the skeleton's skin matrices, +0x70 = how many. FUN_00723800 -> FUN_00719930 copies a section's
// bones from there into "shaderState/bones" when it submits the draw, so a rewrite in the camera hook (which runs before
// the instance loop) needs no D3D hook and only touches the first-person group -- unlike AvP's buffer rewrite.
//
// A skin matrix is float[12], 3 rows of 4: v' = B * (v, 1), translation in [3] [7] [11], in the batch's space (the
// world minus the batch's origin, the same space as the batch camera M). The skeleton gives what AvP had to derive from
// the vertices: bone names (FUN_0067BF40's table) and each bone's bind pose (FUN_00683580), so a joint now = B * bind.
//
// Weapon parts: FUN_00A37160 places one part on a skeleton bone (entry[0] in first person) and appends its meshes to the
// rigid list, whose records become the count==1 weapon batches, in order. Noting the meshes it adds per bone is what
// tells the magazine from the rest of the gun on any weapon, by the bone's name.

#include "../sdk/rigid.h"

#include <cstdint>

namespace mohw {

bool InstallFirstPersonRigHooks();
void RemoveFirstPersonRigHooks();

// Every first-person batch, from the camera hook: rigCamera, the batch camera on the way in, turned to the camera the
// rig's bones were posed against, read from the skeleton's camera bone. Logs how far the rig wobbles against it.
// batchRows: the frame bones' rotations from the batch's own bones (ReadBatchFrameRows), else the ones the game last
// returned (the shared read, which with two views can be the other view's, a tick newer).
void AlignRigToBones(Rigid* rigCamera, const float batchRows[3][3][3] = nullptr);

// NATIVE TWO-VIEW BODY (2026-11-08, "body is flickering to a different ghost like frame somewhat frequently"): each
// view's body batch is its own copy of the player's bones, made by that view's job, so a game tick landing between the
// two copies gave the eyes different poses, and the rig's alignment (AlignRigToBones) read the shared bones, which
// could be the other copy's. For every body batch in two-view mode, before anything reads or moves its bones: the first
// of a frame (frame = NativeStereoEyeForCamera's) keeps its pose as the frame's, the other eye's batch gets it. Logs
// how often the two really differed (mohwvr_rig.log).
void SyncTwoViewBodyPose(const unsigned char* batch, unsigned frame);
// A body batch's own frame-bone rotations, for AlignRigToBones. False if the skeleton isn't known yet.
bool ReadBatchFrameRows(const unsigned char* batch, float rows[3][3][3]);
// THE GUN AT ITS HAND SOCKET (2026-11-08): where the equipped gun's IK_Joint_RightHand (the point the game's own
// animation holds the right hand on, the pistol grip) is in this body batch's pose, in rigCamera's frame (right, up,
// back): the frame the weapon drive's grip is in. And the right hand's centre (the point the arm IK puts on the
// controller), same pose and frame. Before the arm IK moves anything. False if the skeleton or the bone isn't known.
bool ReadGunSocketInCamera(const unsigned char* batch, const Rigid& rigCamera, float out[3], float handCentre[3]);

// THE WEAPON SOCKET VIEW (2026-11-09, ShowWeaponSockets): every weapon bone of the first-person skeleton (Wep_*,
// IK_Joint_*) and each hand's weapon bone (RightWeapon, LeftWeapon), with its parent (the skeleton asset's hierarchy:
// the sockets hang off Wep_Root, Wep_Root off SpineX), in this body batch's own pose, before the arm IK: each joint in
// rigCamera's frame (right, up, back). A hand bone's is also given in its hand's controller frame as the arm IK holds
// that hand (handGrip: the point through the hand's calibration, RightHandIk / LeftHandIk), since the IK moves the hands
// but not the gun. False on a pass after the IK, or without the skeleton.
constexpr int kMaxSocketBones = 32;
struct WeaponSocketBones
{
    int count = 0;
    char names[kMaxSocketBones][24] = {};
    int parent[kMaxSocketBones] = {};       // index into this list, -1: none of these (Wep_Root: its parent is SpineX)
    int hand[kMaxSocketBones] = {};         // 0 a gun bone, 1 on the right hand, 2 on the left hand
    float joint[kMaxSocketBones][3] = {};   // rigCamera's frame
    float handGrip[kMaxSocketBones][3] = {}; // hand bones: in their hand's controller frame (right, up, back)
};
bool ReadWeaponSocketBones(const unsigned char* batch, const Rigid& rigCamera, bool gunInLeftHand, WeaponSocketBones* out);

// Render threads, every weapon batch (hooks/camera_matrix_test_hook.cpp): logs once per mesh whether it's skinned to
// Wep_Mag -- a magazine inside the gun's own mesh, as the pistols have (mohwvr_rig.log).
void NoteWeaponMesh(uintptr_t mesh);

// Submit thread: the WeaponFiring whose reloads take no time, 0 for none (openvr_direct/vr_reload.cpp), and the firing
// data (+0x94) of any other firing that reloads in no time just now, 0 for none: the server's copy, around a seat.
void SetInstantReload(uintptr_t firing, uintptr_t otherFiringsData);
// The other firing that last took a reload that way, 0 for none since the data was cleared.
uintptr_t GetOtherInstantReloadFiring();

// Every first-person body batch, from the camera hook, before anything rewrites it: settles which skeleton is the
// first-person one, by matching the batch's bones against what the game read for each soldier.
void NoteBodyBatch(const unsigned char* batch);

// For a first-person body batch, from the camera hook. cam = the batch camera M; rigCamera = the camera the rig was
// posed against (the camera hook's AlignRigCamera); drawn = the camera the batch is really drawn with (the head anchor);
// gun = where the gun moved (world, rest pose -> on the controller); weaponHand / offHand = the two hands' controller
// frames. Each hand goes on its own controller with its calibration (RightHandIk* for the weapon hand, LeftHandIk* for
// the other; 2026-11-09, the weapon hand used to ride the gun); a two-handed off hand, or one with no controller pose,
// stays on the gun. gunInLeftHand: the hands trade poses first (MirrorHands in the .cpp). Rewrites the batch's bones
// once (several views share a batch). True while the arms are on the controllers.
bool ApplyArmIk(const unsigned char* batch, const Rigid& cam, const Rigid& rigCamera, const Rigid& drawn, const Rigid& gun,
                const Rigid& weaponHand, const Rigid& offHand, bool haveOffHand, bool twoHanded, bool gunInLeftHand);

// Whether a body part's mesh is skinned to the arms, while ApplyArmIk is running: it's drawn then, whatever
// HideBodyPartN says (the part order changes between levels).
bool ShouldDrawArmMesh(uintptr_t mesh);

// Whether this batch's mesh is a magazine part of the weapon drawn now.
bool IsMagazineMesh(uintptr_t mesh);
// The magazine bone's point in the rig's space, now. False if this weapon has none.
bool GetMagazineBonePoint(float out[3]);

} // namespace mohw
