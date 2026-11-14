#pragma once
// THE SKY'S VIEW RAYS (2026-11-08). The sky (FUN_00FD9020, "SkyMain": the panorama, the two cloud layers, the sun and the
// sky's fog) is a full-screen quad whose four corners carry a view ray each, interpolated across the two triangles and
// normalised per pixel. The game builds those rays from its own copy of the view's camera, from the frustum's corner
// points (the copy's +0x100..+0x17F: near and far point of each corner), then NORMALISES each corner's ray before it
// goes into the vertex. Interpolating unit vectors in screen space isn't perspective-correct: right at the corners
// only. With the game's own 50 deg x 79 deg, symmetric view, the sky is warped a little and nobody can tell. With a
// headset's wide, off-centre eye (Quest 3: 100 deg, the centre 15 deg to the nose side), it's warped a lot, mirrored
// between the eyes: in the 2026-11-08 two-view dump the clouds sat 80 px short of the city's disparity, the right eye's
// up to 60 px higher, and a model of exactly this matched every measured patch to 1-3 px. Hence "two layers of clouds,
// each eye a different one", the clouds moving against the world as the head turns, and the haze near the horizon
// brighter in one eye. The alternating-eye mode has the same warp per eye (the old "skybox/clouds per-eye offset").
//
// The fix: right after the four rays are written (0x00FD9DC9, where the 3D Vision branch joins), they're rewritten
// unnormalised (each corner's far point minus its near point, one common scale), which interpolates exactly. The pixel
// shader normalises the ray itself (it must: the game's own rays come out ~0.7 long mid-screen). The engine's own
// 3D Vision branch, which blends the rays per eye, is left alone. Setting SkyExactRays (View > "Exact sky rays"),
// default on; mohwvr_sky.log reports the first rewrites and any frame whose layout didn't match.

namespace mohw {

bool InstallSkyRaysFix();
void RemoveSkyRaysFix();

} // namespace mohw
