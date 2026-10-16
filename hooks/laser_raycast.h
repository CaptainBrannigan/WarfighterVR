#pragma once
// The laser (2026-10-15): the aim-ray dot at the first surface the shot line hits, and a beam from the hand to it
// (openvr_direct/vr_overlay.cpp). The shot line is built exactly like the shots' (sdk/motion_controller_aim.h): from the
// game's camera position plus the hand's offset, along the controller's aim (with the aim offset), the game camera's
// forward being the reference "where the view points". Cast with the game's own GameWorld::RayCast the way a bullet
// does it (BulletEntity updateTransformSync, FUN_0091cd60: ~11.6 m segments, test 0, 5 hits, 0x01000000 / 0),
// excluding the player's soldier, weapon physics and own capsule -- every LaserRaycastEveryFrames rendered frames, on
// the main thread (from the CommitViewTransform hook). The hit distance is handed to the overlay in tracking-space meters.

namespace mohw {

// hooks/fov_scale_hook.cpp's CommitViewTransform hook, every frame, twice: first with the game's own camera transform
// BEFORE the mod changes it (the soldier's eye -- for telling the player's own collision capsule apart), then with the
// camera as rendered for the centre eye (after head roll and head position, before the eye offset): the laser's
// world ray is the drawn beam carried through that camera (RENDER MAPPING in the .cpp). Rows right/up/back at floats
// 0-2/4-6/8-10 (the view looks down -row 2), position 12-14.
void LaserCaptureGameCamera(const float* gameTransform);
void LaserOnCommit(const float* renderTransform);

// hooks/fire_candidate_redirect_hook.cpp, every call: the direction it was handed, compared with the camera's view
// direction (-row 2) and logged only (an automatic flip on disagreement turned the laser around -- that hook also sees calls that
// aren't the player's shots).
void LaserNoteShotDirection(float x, float y, float z);

// hooks/bullet_raycast_redirect_hook.cpp, after each of the player's bullet segment raycasts that returned hits:
// logs what the game's own bullet hit (its first hits, with their types) next to the laser's current point -- ground
// truth for where the laser should be stopping.
void LaserNoteBulletHits(const void* hits, unsigned int count, const float* segmentStart, const float* segmentEnd);

// Any thread: the laser's point in the game world -- where it hits, or its end (the max range) when nothing's hit.
// The shots and tracers aim at it (hooks/fire_candidate_redirect_hook.cpp, bullet_raycast_redirect_hook.cpp), so they
// land on the dot. False when the laser isn't running (no recent cast): they keep the controller's direction then.
bool GetLaserAimPoint(float worldPoint[3]);

// Any thread: the distance along the controller's aim to the laser's hit, tracking-space meters (the game's world
// distance over HeadPositionScale). False when there's no recent hit (nothing within range, or the laser isn't
// running): the dot goes at the max range then.
bool GetLaserHitDistance(float* trackingMeters);

} // namespace mohw
