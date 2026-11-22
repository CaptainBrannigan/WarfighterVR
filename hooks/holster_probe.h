#pragma once
// HOLSTER PROBE (2026-11-17, the user: "pull in the meshes of the other hotkey weapons and items and display them in our
// holsters"). The plan is to keep a copy of each weapon's first-person batches while it's in hand, and once it's put away
// submit the copies again through the game's batch draw (FUN_008B2AB0) at the holster. The batch descriptors themselves
// are per-frame ring-buffer temporaries, so it only works if what they point to outlives the holster. This finds out,
// in mohwvr_holsterprobe.log (written whatever the log switch says, sdk/logging.h LogAlways):
//   - while a weapon is in hand, per batch (identified by its first mesh and part count): which pointers change every
//     frame and which stay -- the batch itself, its mesh / data pairs (+0x78), per-part records (+0x80), tables (+0x98,
//     +0xA0) -- and whether the data behind the pairs' second pointer and the records changes (a live pose) or not;
//   - once it's put away, 1, 2, 5, 10, 30 and 60 s later: whether the meshes, the second pointers' data, the records and
//     the tables are still there, unchanged (same type word, same bytes) or gone / reused;
//   - when it's taken out again: whether its batches come back with the same pointers;
//   - once per weapon, raw dumps of a batch, its first pair's data and its records, to read their layout.

#include <cstdint>

namespace mohw {

// hooks/camera_matrix_test_hook.cpp HookImpl, render job threads: every first-person weapon batch (count != 3) of the
// first view (eye <= 0).
void HolsterProbeOnBatch(const unsigned char* batch);

} // namespace mohw
