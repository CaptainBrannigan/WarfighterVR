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
void AlignRigToBones(Rigid* rigCamera);

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
// gun = where the gun moved (world, rest pose -> on the controller); offHand = the off
// hand's controller frame. The weapon hand's bones go with the gun, so it stays on the grip whatever the animation does.
// gunInLeftHand: the hands trade poses first (MirrorHands in the .cpp). Rewrites the batch's bones once (several views
// share a batch). True while the arms are on the controllers.
bool ApplyArmIk(const unsigned char* batch, const Rigid& cam, const Rigid& rigCamera, const Rigid& drawn, const Rigid& gun,
                const Rigid& offHand, bool haveOffHand, bool twoHanded, bool gunInLeftHand);

// Whether a body part's mesh is skinned to the arms, while ApplyArmIk is running: it's drawn then, whatever
// HideBodyPartN says (the part order changes between levels).
bool ShouldDrawArmMesh(uintptr_t mesh);

// Whether this batch's mesh is a magazine part of the weapon drawn now.
bool IsMagazineMesh(uintptr_t mesh);
// The magazine bone's point in the rig's space, now. False if this weapon has none.
bool GetMagazineBonePoint(float out[3]);

} // namespace mohw
