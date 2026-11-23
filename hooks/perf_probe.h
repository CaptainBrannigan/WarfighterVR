#pragma once
// PERF PROBE (2026-11-17, the user: a level's start is slow and settles, "wasn't an issue before the scope update"). The
// time our own code takes, per frame, next to the frame time: a line a second for the first two minutes after a level
// is built (and after launch), every 10 s after that, in mohwvr_perf.log -- written whatever the log switch says
// (sdk/logging.h LogAlways), as the user asked for these probes.
//
//   batches   hooks/camera_matrix_test_hook.cpp HookImpl, every first-person batch (render job threads)
//   eyes      hooks/native_stereo.cpp MakeEyes, the two views and the scoped eye (game thread)
//   commit    hooks/fov_scale_hook.cpp's CommitViewTransform hook, the head, eye offset and the laser casts
//   present   our part of Present, without the game's own (its flip and wait)
//   scope     inside that: the lens pictures and the scoped eye's mask
//   pair      inside that: handing the eyes to SteamVR (the copies into a hand-off slot; it no longer waits)
//   vrinput   openvr_direct/vr_input.cpp UpdateVrInput, our SteamVR submit thread (not a game thread): its call count
//             is the submit loop's rate, the headset's 120 when nothing holds it up
// Counts only (2026-11-17, the hand-off slots in openvr_direct.cpp):
//   submit    the submit thread's two Submit calls (time, and loops)
//   shown     new pairs the submit thread took: game frames that reached the headset
//   late      submit loops where newer pairs existed but the GPU hadn't finished any (the shown pair went out again)
//   dropped   game frames that never reached the headset's queue: overwritten before they were finished and taken,
//             or not handed over at all (no slot, or a slot's lock not free at once)
// Per frame (2026-11-22):
//   world     draws into the G-buffer, the 3D world: under 40 a frame is a whole 2D frame (hooks/draw_trace_diag.cpp)

namespace mohw {

enum PerfSlot
{
    kPerfBatches,
    kPerfMakeEyes,
    kPerfCommit,
    kPerfPresentOurs,
    kPerfScopeDraw,
    kPerfPairCopy,
    kPerfVrInput,
    kPerfSubmit,
    kPerfShown,
    kPerfLate,
    kPerfDropped,
    kPerfWorldDraws, // per frame: draws into the G-buffer, the 3D world (hooks/draw_trace_diag.cpp WorldDrawnRecently)
    kPerfSlotCount
};

long long PerfNow(); // QueryPerformanceCounter ticks
void PerfAdd(PerfSlot slot, long long ticks); // any thread

struct PerfScope
{
    explicit PerfScope(PerfSlot s) : slot(s), start(PerfNow()) {}
    ~PerfScope() { PerfAdd(slot, PerfNow() - start); }
    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;
    PerfSlot slot;
    long long start;
};

// Present (game thread), once per frame: the frame time, and the line when it's due.
void PerfOnPresent();
// hooks/native_stereo.cpp, when a level is built: the per-second lines start again.
void PerfNoteLevelBuilt();

} // namespace mohw
