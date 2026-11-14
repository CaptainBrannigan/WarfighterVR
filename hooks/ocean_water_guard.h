#pragma once
// THE OCEAN WATER WITH TWO VIEWS (2026-11-08). The water render module (vtable 027F881C, created by FUN_00CF9080 at a
// level's load) kicks its jobs once PER VIEW (FUN_00CF2070, vtable +0x1C): oceanWaterBuild, oceanWaterDraw,
// oceanWaterPreDraw and three oceanWaterCull slices. They all work on the module's single set of buffers: the culls
// write each patch's LOD and the per-LOD counts, the pre-draw zeroes and refills the LOD ranges (module +0x830) and the
// patch transforms (a 64000-byte buffer, 1000 patches), and the draw reads both to build the view's water draw lists.
// With one view that chain runs in order. With the native two-view stereo (hooks/native_stereo.h) the second view's
// culls and pre-draw run while the first view's draw is reading, so a LOD's range can come out backwards and its copy
// (FUN_00743F70, count * 64 bytes) runs off the end of memory: the three crashes of 2026-11-08, all in memcpy, counts
// -8 and -202.
//
// This serializes them while NativeStereo is on: culls and pre-draws take a lock exclusively, draws share it, so a
// view's draw never sees the buffers half rewritten (it may see the other eye's results, which are the same patches
// 6 cm apart). And every LOD draw's range is checked against the transform buffer: a bad one draws nothing instead of
// copying (mohwvr_oceanwater.log counts both).

namespace mohw {

bool InstallOceanWaterGuard();
void RemoveOceanWaterGuard();

} // namespace mohw
