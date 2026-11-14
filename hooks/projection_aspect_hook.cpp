#include "projection_aspect_hook.h"
#include "camera_matrix_test_hook.h"
#include "native_stereo.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/logging.h"

#include <windows.h>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>


namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_fovscale.log";

using UpdateMatricesFn = void(__fastcall*)(float* self, void* edx);
UpdateMatricesFn g_original = nullptr;
void* g_target = nullptr;

std::atomic<uint32_t> g_commitFovBits{0};
std::atomic<float> g_trim{0.0f}; // 0 = inactive
std::atomic<long long> g_calls{0}, g_matched{0};
std::atomic<uint32_t> g_frustumFovBits{0};
std::atomic<float> g_frustumAspect{0.0f}, g_frustumOffX{0.0f}, g_frustumOffY{0.0f};
std::atomic<bool> g_frustumActive{false};
std::atomic<int> g_frustumEyeIdx{-1}; // which eye (0/1) the values above currently represent -- set by eye_matched_fov.cpp
// When the world camera last got this eye's true frustum (GetTickCount64 ms): the game is rendering the 3D world, as
// opposed to a pre-rendered movie, a menu or a loading screen. See WorldRenderedRecently.
std::atomic<unsigned long long> g_lastWorldFrustumMs{0};

// MIXED-EYE BATCH diagnostic (2026-09-24): user-reported visual signature (a ghost on BOTH sides of a mesh
// silhouette, not a directional trail following head motion) points at a geometric MISALIGNMENT between
// passes rather than compositor/reprojection lag (already ruled out this session: render-pose-stamp lag is
// symmetric between eyes, submit order doesn't matter, submit eye-index doesn't matter -- the ghost follows
// the physical right eye's own rendered content regardless). Theory: this engine dispatches UpdateMatrices
// from multiple worker threads (confirmed live elsewhere, mohwvr_cameramatrix.log's multi-tid SEQ lines); if
// a worker thread's UpdateMatrices call for one eye's frame is still trailing in by the time eye_matched_fov.cpp
// has already overwritten g_frustumAspect/offX/offY for the NEXT eye, some candidates in what should be one
// eye's multi-object batch (8-20 objects, confirmed cycling within sub-millisecond windows) would get stamped
// with the WRONG eye's off-axis projection -- exactly a double-sided edge mismatch, not a directional smear.
// High-resolution (QueryPerformanceCounter) gap detection groups writes into batches; a batch that contains
// BOTH eye0 and eye1 writes is logged. GetTickCount64 (~15ms resolution) is too coarse for this -- the whole
// batch cycles within under 1ms per the timestamps of the (since removed) "TRUE FRUSTUM MATCH CHANGED" log.
std::atomic<long long> g_lastWriteQpc{0};
std::atomic<int> g_batchEyeMask{0}; // bit0 = eye0 seen (confirmed) this batch, bit1 = eye1 seen (confirmed)
// QueryPerformanceFrequency is a cheap, thread-safe call on modern Windows (no caching needed -- avoids any
// init-once race given this hook fires from multiple worker threads, confirmed live elsewhere in this project).

// DEBOUNCE state for the reject gate (2026-09-24, fix for a confirmed-live false-positive cascade): the first
// version rejected EVERY write whose eye disagreed with the already-established batch, no matter how many such
// writes followed. Confirmed live this incorrectly nuked a whole LEGITIMATE new batch (7 different objects, all
// wanting the other eye, all rejected in the same instant) whenever a real eye-switch happened to land slightly
// earlier than the 2ms gap threshold expected -- every one of those objects then rendered with a stale
// projection for that entire frame, matching the user's reported "single frame of very stale, incorrect image".
// Fix: only reject the FIRST mismatched write (the real rare-corruption case); if a SECOND consecutive write for
// that same "new" eye follows immediately, treat it as a genuine batch transition instead and accept it,
// resetting the established mask to the new eye rather than keep rejecting a whole legitimate frame.
std::atomic<int> g_pendingEyeIdx{-1};

// The matched cameras this window, for the periodic frustum summary. (A per-change "TRUE FRUSTUM MATCH CHANGED" line,
// a 2026-09-21 ghosting diagnostic, was removed 2026-11-10: in scenes with many matching cameras it wrote over a
// thousand lines a second.)
std::atomic<int> g_distinctMatchedThisWindow{0};
std::mutex g_matchedSetMutex;
void* g_matchedSet[8] = {};
int g_matchedSetCount = 0;

// REVERTED (2026-09-21, at the user's request): a per-frame "first match wins" claim, and later a sticky version that
// locked onto one object across frames, were both tried here to stop the multi-object hijack the log below can reveal.
// The sticky version instead produced a DETERMINISTIC misalignment (same wrong result every time, not the flapping the
// fix assumed), so both are backed out; matching is unconditional again -- every object whose fov bits match gets
// written, same as before either fix. The diagnostic logging (identity-change / distinct-count) is kept.

// MANUAL CAMERA SELECTOR (2026-09-21): rather than have the mod GUESS which of the fov-matching objects is the real
// world camera, let the user pick, live, from whatever the game has actually shown us this true-frustum session. This
// list only ever grows (reset on mode deactivate) and never reorders, so a given index keeps meaning the same physical
// object for as long as the user is cycling through them with ',' '.' in the companion.
constexpr int kMaxCandidates = 16;
std::mutex g_candidateMutex;
void* g_candidates[kMaxCandidates] = {};
int g_candidateCount = 0;
std::atomic<int> g_candidateIndex{-1}; // -1 = unfiltered (write to every match)

void ResetCandidateList()
{
    std::lock_guard<std::mutex> lock(g_candidateMutex);
    g_candidateCount = 0;
}

// Returns the candidate list index for `self` (adding it if new and there's room), or -1 if the list is full and
// `self` isn't already in it. Also fills *outCount with the current list size, for modding the user's raw index.
int RecordCandidate(void* self, int* outCount)
{
    std::lock_guard<std::mutex> lock(g_candidateMutex);
    for (int i = 0; i < g_candidateCount; ++i)
    {
        if (g_candidates[i] == self)
        {
            *outCount = g_candidateCount;
            return i;
        }
    }
    int idx = -1;
    if (g_candidateCount < kMaxCandidates)
    {
        idx = g_candidateCount;
        g_candidates[g_candidateCount++] = self;
    }
    *outCount = g_candidateCount;
    return idx;
}

uint32_t FloatBits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

// DIAGNOSTIC (2026-09-21): which camera configurations does the engine rebuild matrices for? The HUD/UI needs to be found (it is
// drawn at the frame centre, which in the true-frustum mode is not the eye's forward direction -> double image). Keeps up to 12
// distinct (type, fov, aspect, orthoW/H, viewport offset/scale) signatures with call counts and dumps them every 3 s.
struct CamSig
{
    int type;
    float v[10]; // fov, aspect, orthoW, orthoH, near, far, offX, offY, scaleX, scaleY
    long long count;
};
CamSig g_sigs[12];
int g_sigCount = 0;
std::mutex g_sigMutex; // UpdateMatrices runs on several threads
std::atomic<unsigned long long> g_nextSigLogMs{0};

bool ReadCameraSignature(float* self, CamSig* out)
{
    __try
    {
        out->type = *reinterpret_cast<int*>(self + 0x10);
        out->v[0] = self[0x12];
        out->v[1] = self[0x16];
        out->v[2] = self[0x17];
        out->v[3] = self[0x18];
        out->v[4] = self[0x14];
        out->v[5] = self[0x15];
        out->v[6] = self[0x1b];
        out->v[7] = self[0x1c];
        out->v[8] = self[0x1d];
        out->v[9] = self[0x1e];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void RecordCameraSignature(float* self)
{
    CamSig s{};
    if (!ReadCameraSignature(self, &s))
        return;
    std::lock_guard<std::mutex> lock(g_sigMutex);
    for (int i = 0; i < g_sigCount; ++i)
    {
        if (g_sigs[i].type == s.type && memcmp(g_sigs[i].v, s.v, sizeof(s.v)) == 0)
        {
            ++g_sigs[i].count;
            return;
        }
    }
    if (g_sigCount < 12)
    {
        s.count = 1;
        g_sigs[g_sigCount++] = s;
    }
}

void MaybeLogCameraSignatures()
{
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = g_nextSigLogMs.load(std::memory_order_relaxed);
    if (now < allowed || !g_nextSigLogMs.compare_exchange_strong(allowed, now + 3000, std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(g_sigMutex);
    for (int i = 0; i < g_sigCount; ++i)
    {
        const CamSig& s = g_sigs[i];
        MOHW_LOG(kLogFile,
                  "CAM SIG %d: type=%d fov=%.4f aspect=%.4f orthoW=%.1f orthoH=%.1f near=%.3f far=%.1f off=(%.4f,%.4f) scale=(%.4f,%.4f) count=%lld",
                  i, s.type, s.v[0], s.v[1], s.v[2], s.v[3], s.v[4], s.v[5], s.v[6], s.v[7], s.v[8], s.v[9], s.count);
    }
    g_sigCount = 0; // restart the window so counts are per-interval and new configs show up
}


// FIX 3 (2026-09-21): confirmed live -- fov-bit matching alone is far too weak. The log showed a tight, continuously
// REPEATING cycle of ~20 objects matching every frame (not just a brief burst at mode switch), most sitting at
// pos=(0,0,0) or other clearly-non-player positions; near/far pairs like 0.001/14.1 or 14.058/37.4 look like shadow-
// cascade splits, not the world camera. Two positions did repeat consistently with tiny real-time jitter -- exactly
// what a moving player camera looks like -- so position is now used as a SECOND, independent filter: the game's own
// static player-camera anchor (OFFSET_PLAYERCAMERAOBJECT, position at +0x30 -- see debugging_tips_x32dbg.md) gives a
// ground-truth position with NO dependence on this matching scheme at all, to check each fov-matching candidate against.
bool ReadPlayerAnchorPosition(float pos[3])
{
    __try
    {
        float* anchor = Offset<float*>(OFFSET_PLAYERCAMERAOBJECT);
        pos[0] = anchor[12];
        pos[1] = anchor[13];
        pos[2] = anchor[14];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-guarded, no C++ objects in scope (a function with __try/__except can't also contain std::mutex/lock_guard --
// C2712). Read-only: does `self` match the fov true-frustum mode is targeting AND sit near the player (see FIX 3
// above)? Split from the write below so the candidate-list bookkeeping (mutex) can run in between, in the caller,
// which has no __try of its own.
bool ReadFrustumFovMatch(float* self)
{
    __try
    {
        int type = *reinterpret_cast<int*>(self + 0x10);
        if (type != 0 || FloatBits(self[0x12]) != g_frustumFovBits.load(std::memory_order_relaxed))
            return false;
        float anchor[3];
        if (!ReadPlayerAnchorPosition(anchor)) // SEH-inside-SEH is fine, no C++ objects in either
            return true;                        // anchor unreadable -- don't let that block matching entirely
        float dx = self[12] - anchor[0], dy = self[13] - anchor[1], dz = self[14] - anchor[2];
        constexpr float kMaxDistSq = 9.0f; // 3 units, generous for the known eye-vs-feet height offset
        return (dx * dx + dy * dy + dz * dz) <= kMaxDistSq;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-guarded write, same reasoning as ReadFrustumFovMatch. Caller must already have confirmed the match.
bool WriteFrustumProjection(float* self)
{
    __try
    {
        self[0x16] = g_frustumAspect.load(std::memory_order_relaxed); // +0x58 aspect
        self[0x1b] = g_frustumOffX.load(std::memory_order_relaxed);   // +0x6C viewport offset x
        self[0x1c] = g_frustumOffY.load(std::memory_order_relaxed);   // +0x70 viewport offset y
        self[0x1d] = 1.0f;                                             // +0x74 viewport scale x
        self[0x1e] = 1.0f;                                             // +0x78 viewport scale y
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The first-person viewmodel camera (see ConsumeViewmodelCamera): it sits at the batch origin rather than near the
// player and carries its own 55 degree fov, so ReadFrustumFovMatch never picks it up. Gives it the world's fov and
// this eye's frustum so the gun is projected like everything else. SEH-guarded, same reasoning as the write above.
bool WriteViewmodelFrustumProjection(float* self)
{
    __try
    {
        if (*reinterpret_cast<int*>(self + 0x10) != 0) // perspective only
            return false;
        uint32_t fovBits = g_frustumFovBits.load(std::memory_order_relaxed);
        memcpy(&self[0x12], &fovBits, sizeof(fovBits)); // +0x48 fov
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return WriteFrustumProjection(self);
}

// NATIVE TWO-VIEW (hooks/native_stereo.h): the gun's camera gets the frustum of the eye whose view its batch is being
// built for, as that view has it (fovY, aspect, viewport offset). SEH-guarded like the writes above.
bool WriteViewmodelEyeProjection(float* self, const float projection[4])
{
    __try
    {
        if (*reinterpret_cast<int*>(self + 0x10) != 0) // perspective only
            return false;
        self[0x12] = projection[0]; // +0x48 fovY
        self[0x16] = projection[1]; // +0x58 aspect
        self[0x1b] = projection[2]; // +0x6C viewport offset x
        self[0x1c] = projection[3]; // +0x70 viewport offset y
        self[0x1d] = 1.0f;          // +0x74 viewport scale x
        self[0x1e] = 1.0f;          // +0x78 viewport scale y
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-guarded, no C++ objects in scope -- same C2712 reason as TryApplyFrustumMatch above.
bool TryApplyInnerEdgeTrim(float* self, float trim)
{
    __try
    {
        // Object layout per FUN_00707CB0's decompile: int type at +0x40, fovY at +0x48, aspect at +0x58.
        int type = *reinterpret_cast<int*>(self + 0x10);
        float fov = self[0x12];
        if (type != 0 || FloatBits(fov) != g_commitFovBits.load(std::memory_order_relaxed))
            return false;
        float aspect = self[0x16];
        if (!(aspect > 0.2f && aspect < 8.0f))
            return false;
        // If the engine did not rewrite the aspect since our last write, the value read back is our own; treat it as
        // still ours instead of dividing again every call.
        static float lastWritten = 0.0f, lastNative = 0.0f;
        if (lastWritten > 0.0f && fabsf(aspect - lastWritten) < 1e-5f)
            aspect = lastNative;
        else
            lastNative = aspect;
        lastWritten = aspect / trim;
        self[0x16] = lastWritten;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// NATIVE TWO-VIEW DIAGNOSTIC (2026-11-08, the menu showed the IPD moving only the right eye): where the engine really
// draws each eye's world from. A world camera of the two views carries its eye's frustum (viewport offset x negative
// for the left eye, positive for the right) and the view's own near/far (not a shadow cascade's); its position is
// matched against the cameras hooks/native_stereo.cpp wrote over the last frames (NativeStereoClassifyWorldCamera).
// Counted per eye, logged every 5 s.
struct NativeWorldSample
{
    int type;
    float pos[3];
    float offX, nearZ, farZ;
};

bool ReadNativeWorldSample(const float* self, NativeWorldSample* out)
{
    __try
    {
        out->type = *reinterpret_cast<const int*>(self + 0x10);
        out->pos[0] = self[12];
        out->pos[1] = self[13];
        out->pos[2] = self[14];
        out->offX = self[0x1b];
        out->nearZ = self[0x14];
        out->farZ = self[0x15];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// [frustum eye][where: 0 the left eye's camera, 1 the right eye's, 2 the head centre, 3 none of them]
std::atomic<long> g_nativeWorldSeen[2][4] = {};
std::atomic<unsigned long long> g_nextNativeWorldLogMs{0};

void NoteNativeWorldCamera(const float* self)
{
    NativeWorldSample s{};
    if (!ReadNativeWorldSample(self, &s) || s.type != 0 || !(s.nearZ < 1.0f) || !(s.farZ > 100.0f) ||
        !(fabsf(s.offX) > 0.01f))
        return;
    const int eye = s.offX < 0.0f ? 0 : 1;
    const int where = NativeStereoClassifyWorldCamera(s.pos);
    g_nativeWorldSeen[eye][where >= 0 ? where : 3].fetch_add(1, std::memory_order_relaxed);

    const unsigned long long now = GetTickCount64();
    unsigned long long due = g_nextNativeWorldLogMs.load(std::memory_order_relaxed);
    if (now < due || !g_nextNativeWorldLogMs.compare_exchange_strong(due, now + 5000, std::memory_order_relaxed))
        return;
    long n[2][4];
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 4; ++k)
            n[e][k] = g_nativeWorldSeen[e][k].exchange(0, std::memory_order_relaxed);
    MOHW_LOG(kLogFile,
             "NATIVE WORLD CAMERAS, last 5 s (each eye's frustum, and where its camera was): left-eye frustum at the "
             "left eye %ld, at the right eye %ld, at the head centre %ld, elsewhere %ld; right-eye frustum at the left "
             "eye %ld, at the right eye %ld, at the head centre %ld, elsewhere %ld",
             n[0][0], n[0][1], n[0][2], n[0][3], n[1][0], n[1][1], n[1][2], n[1][3]);
}

void __fastcall Hooked_UpdateMatrices(float* self, void* edx)
{
    ++g_calls;
    if (self)
    {
        RecordCameraSignature(self);
        MaybeLogCameraSignatures();
    }
    if (self && IsNativeStereoActive() && ConsumeViewmodelCamera(self))
    {
        float projection[4];
        const int eye = LastViewmodelEye();
        static std::atomic<long long> nativeWrites{0};
        if (eye >= 0 && GetNativeEyeProjection(eye, projection) && WriteViewmodelEyeProjection(self, projection))
        {
            long long n = ++nativeWrites;
            if (n == 1 || n % 5000 == 0)
                MOHW_LOG(kLogFile,
                         "VIEWMODEL NATIVE TWO-VIEW: camera %p given eye %d's frustum (fov %.1f deg, aspect %.4f, offset "
                         "%.4f, %.4f) (count=%lld)",
                         static_cast<void*>(self), eye, projection[0] * 57.29578f, projection[1], projection[2],
                         projection[3], n);
        }
        g_original(self, edx);
        return;
    }
    if (self && IsNativeStereoActive())
        NoteNativeWorldCamera(self);
    if (g_frustumActive.load(std::memory_order_relaxed) && self && ConsumeViewmodelCamera(self))
    {
        static std::atomic<long long> viewmodelWrites{0};
        if (WriteViewmodelFrustumProjection(self))
        {
            long long n = ++viewmodelWrites;
            if (n == 1 || n % 5000 == 0)
                MOHW_LOG(kLogFile, "VIEWMODEL TRUE FRUSTUM: camera %p given aspect %.4f offset (%.4f, %.4f) (count=%lld)",
                          static_cast<void*>(self), g_frustumAspect.load(), g_frustumOffX.load(), g_frustumOffY.load(), n);
        }
        g_original(self, edx);
        return;
    }
    if (g_frustumActive.load(std::memory_order_relaxed) && self)
    {
        if (ReadFrustumFovMatch(self)) // SEH-guarded read-only check -- see its own comment
        {
            int candidateCount = 0;
            int candidateIdx = RecordCandidate(self, &candidateCount); // -1 only if the list is full and self is new
            int selector = g_candidateIndex.load(std::memory_order_relaxed);
            // Resolved index the selector actually maps to right now, or -1 if unfiltered / nothing discovered yet.
            int resolved = (selector >= 0 && candidateCount > 0) ? ((selector % candidateCount) + candidateCount) % candidateCount : -1;
            bool shouldWrite = (selector < 0) || (resolved == candidateIdx);

            static std::atomic<int> lastLoggedSelector{-2}, lastLoggedResolved{-2};
            if (lastLoggedSelector.exchange(selector, std::memory_order_relaxed) != selector ||
                lastLoggedResolved.exchange(resolved, std::memory_order_relaxed) != resolved)
            {
                char resolvedBuf[16] = "none yet";
                if (resolved >= 0)
                    snprintf(resolvedBuf, sizeof(resolvedBuf), "%d", resolved);
                MOHW_LOG(kLogFile, "TRUE FRUSTUM SELECTOR: raw=%d -> %s (of %d known object(s))", selector,
                          selector < 0 ? "ALL (unfiltered)" : resolvedBuf, candidateCount);
            }

            // MIXED-EYE BATCH GATE (2026-09-24): moved BEFORE the write itself, and now REJECTS instead of just
            // logging -- see g_frustumEyeIdx's declaration comment for the full race theory and the live-captured
            // evidence (mohwvr_fovscale.log's DETAIL capture, 2026-09-24) that batches are otherwise extremely
            // regular (clean runs of ~30 writes alternating eye0/eye1) with RARE (~1 in ~130 batches) corruption --
            // low enough that discarding just the corrupting write, rather than the whole batch or object, costs
            // essentially nothing real while eliminating the wrong-eye stamp that produces the double-edged ghost.
            // High-resolution (QueryPerformanceCounter) gap detection groups writes into batches; GetTickCount64
            // (~15ms resolution) is too coarse -- the whole batch cycles within under 1ms per prior diagnostics.
            //
            // DEBOUNCED (2026-09-24, fix for a confirmed-live false-positive cascade) -- see g_pendingEyeIdx's
            // declaration comment: only the FIRST mismatched write is rejected; a SECOND consecutive one for the
            // same "new" eye is accepted as a genuine batch transition instead of rejected too.
            bool allowWrite = shouldWrite;
            int eyeIdxForThisWrite = -1;
            long long nowUs = 0;
            if (shouldWrite)
            {
                LARGE_INTEGER freq{}, now{};
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&now);
                constexpr long long kBatchGapUs = 2000; // 2ms -- comfortably above intra-batch cycling (sub-1ms,
                                                          // confirmed live), comfortably below inter-frame gaps
                                                          // (>=11ms even at 90fps)
                nowUs = (now.QuadPart * 1000000) / (freq.QuadPart > 0 ? freq.QuadPart : 1);
                long long lastUs = g_lastWriteQpc.exchange(nowUs, std::memory_order_relaxed);
                bool newBatch = (nowUs - lastUs > kBatchGapUs);
                int mask = newBatch ? 0 : g_batchEyeMask.load(std::memory_order_relaxed);
                if (newBatch)
                    g_pendingEyeIdx.store(-1, std::memory_order_relaxed); // fresh batch -- no debounce state carries over
                eyeIdxForThisWrite = g_frustumEyeIdx.load(std::memory_order_relaxed);
                int thisBit = (eyeIdxForThisWrite == 0) ? 1 : (eyeIdxForThisWrite == 1 ? 2 : 0);
                if (thisBit != 0 && mask != 0 && (mask & thisBit) == 0)
                {
                    // Eye disagrees with the established batch. Debounce: is this the SECOND consecutive write
                    // wanting this same "new" eye (a real transition), or the first (possibly just a stray)?
                    int pending = g_pendingEyeIdx.exchange(eyeIdxForThisWrite, std::memory_order_relaxed);
                    if (pending == eyeIdxForThisWrite)
                    {
                        // Confirmed transition -- accept it, and let this eye become the batch's new established
                        // one (discard the old mask entirely rather than OR into it, since the old eye's batch is
                        // over).
                        mask = thisBit;
                        g_batchEyeMask.store(mask, std::memory_order_relaxed);
                        g_pendingEyeIdx.store(-1, std::memory_order_relaxed); // consumed -- next mismatch starts fresh
                    }
                    else
                    {
                        // First mismatch seen -- reject as a precaution, but don't touch the established mask, so a
                        // return to the ORIGINAL eye on the next write is still accepted normally.
                        allowWrite = false;
                        static std::atomic<long long> rejectedCount{0};
                        long long n = ++rejectedCount;
                        if (n <= 30 || n % 500 == 0)
                            MOHW_LOG(kLogFile,
                                      "MIXED-EYE BATCH REJECTED: object=%p wanted eye%d, batch already established as "
                                      "eye-mask=%d -- write discarded, object keeps its previous projection (count=%lld)",
                                      self, eyeIdxForThisWrite, mask, n);
                    }
                }
                else
                {
                    // Write is consistent with the established batch (or starts a fresh one) -- accept normally,
                    // and clear any pending debounce state since the anomaly (if any) didn't repeat.
                    mask |= thisBit;
                    g_batchEyeMask.store(mask, std::memory_order_relaxed);
                    g_pendingEyeIdx.store(-1, std::memory_order_relaxed);
                }
            }

            if (allowWrite && WriteFrustumProjection(self)) // SEH-guarded write -- see its own comment
            {
                ++g_matched;
                g_lastWorldFrustumMs.store(GetTickCount64(), std::memory_order_relaxed); // see WorldRenderedRecently
                std::lock_guard<std::mutex> lock(g_matchedSetMutex);
                bool seen = false;
                for (int i = 0; i < g_matchedSetCount; ++i)
                    if (g_matchedSet[i] == self)
                        seen = true;
                if (!seen && g_matchedSetCount < 8)
                    g_matchedSet[g_matchedSetCount++] = self;
            }
        }
        static std::atomic<unsigned long long> nextFrustumLogMs{0};
        unsigned long long nowF = GetTickCount64();
        unsigned long long allowedF = nextFrustumLogMs.load(std::memory_order_relaxed);
        if (nowF >= allowedF && nextFrustumLogMs.compare_exchange_strong(allowedF, nowF + 2000, std::memory_order_relaxed))
        {
            int distinctCount;
            {
                std::lock_guard<std::mutex> lock(g_matchedSetMutex);
                distinctCount = g_matchedSetCount;
                g_matchedSetCount = 0; // reset window
            }
            MOHW_LOG(kLogFile,
                      "TRUE FRUSTUM: aspect %.4f offset (%.4f, %.4f), matched %lld of %lld UpdateMatrices calls, %d distinct "
                      "object(s) matched this window%s",
                      g_frustumAspect.load(), g_frustumOffX.load(), g_frustumOffY.load(), g_matched.load(), g_calls.load(), distinctCount,
                      distinctCount > 1 ? " -- MORE THAN ONE OBJECT MATCHED, likely wrong-camera hijack" : "");
        }
        g_original(self, edx);
        return;
    }
    float trim = g_trim.load(std::memory_order_relaxed);
    if (trim > 0.0f && self)
    {
        if (TryApplyInnerEdgeTrim(self, trim))
            ++g_matched;
    }

    static std::atomic<unsigned long long> nextLogMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogMs.load(std::memory_order_relaxed);
    if (now >= allowed && nextLogMs.compare_exchange_strong(allowed, now + 2000, std::memory_order_relaxed))
        MOHW_LOG(kLogFile, "PROJECTION ASPECT: trim %.3f, matched %lld of %lld UpdateMatrices calls (commit fov bits %08X)", trim,
                   g_matched.load(), g_calls.load(), g_commitFovBits.load());

    g_original(self, edx);
}

} // namespace

// Whether the 3D world is being rendered, from how busy the camera matrix rebuild is (live 2026-09-28): real world
// rendering rebuilds ~13,000+ camera matrices a second (shadows, reflections, the view...), while during a
// pre-rendered movie the engine only keeps ~5 camera objects ticking, ~150 rebuilds a second, all of them still
// matching the world camera -- so "was the world camera updated" can't tell the two apart, but the rate can.
// Measured over >= withinMs windows (called per draw, render thread). Threshold 2,500 (was 1,000 until 2026-09-30):
// a movie with the next level streaming in behind it ran at 1,105/s and counted as 3D, while gameplay that session
// ran at 4,800-5,600/s.
bool WorldRenderedRecently(unsigned withinMs)
{
    constexpr long long kWorldRebuildsPerSecond = 2500;
    static unsigned long long windowStartMs = 0;
    static long long windowStartCalls = 0;
    static bool rendering = true;
    unsigned long long now = GetTickCount64();
    long long calls = g_calls.load(std::memory_order_relaxed);
    if (windowStartMs == 0)
    {
        windowStartMs = now;
        windowStartCalls = calls;
        return rendering;
    }
    unsigned long long elapsed = now - windowStartMs;
    if (elapsed >= withinMs)
    {
        long long perSecond = (calls - windowStartCalls) * 1000 / static_cast<long long>(elapsed);
        bool now3d = perSecond >= kWorldRebuildsPerSecond;
        if (now3d != rendering)
            MOHW_LOG(kLogFile, "frame content: %s (%lld camera rebuilds/s)", now3d ? "3D world" : "2D only (movie/menu/loading)",
                      perSecond);
        rendering = now3d;
        windowStartMs = now;
        windowStartCalls = calls;
    }
    return rendering;
}

void SetTrueFrustumProjection(float commitFovRad, float aspect, float offX, float offY, int eyeIdx)
{
    bool wasActive = g_frustumActive.load(std::memory_order_relaxed);
    if (commitFovRad > 0.0f && aspect > 0.0f)
    {
        g_frustumFovBits.store(FloatBits(commitFovRad), std::memory_order_relaxed);
        g_frustumAspect.store(aspect, std::memory_order_relaxed);
        g_frustumOffX.store(offX, std::memory_order_relaxed);
        g_frustumOffY.store(offY, std::memory_order_relaxed);
        g_frustumEyeIdx.store(eyeIdx, std::memory_order_relaxed);
        g_frustumActive.store(true, std::memory_order_relaxed);
    }
    else
    {
        g_frustumActive.store(false, std::memory_order_relaxed);
        // Falling edge only -- this is also reached every frame the inner-edge path is the active one, which must NOT
        // keep wiping the candidate list every single frame.
        if (wasActive)
            ResetCandidateList();
    }
}

void SetFrustumCandidateIndex(int index)
{
    g_candidateIndex.store(index, std::memory_order_relaxed);
}

void SetInnerEdgeProjection(float commitFovRad, float trim)
{
    if (commitFovRad > 0.0f && trim > 0.0f)
    {
        g_commitFovBits.store(FloatBits(commitFovRad), std::memory_order_relaxed);
        g_trim.store(trim, std::memory_order_relaxed);
    }
    else
    {
        g_trim.store(0.0f, std::memory_order_relaxed);
    }
}

bool InstallProjectionAspectHook()
{
    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "InstallProjectionAspectHook: MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }
    g_target = Offset<void*>(OFFSET_UPDATEMATRICES);
    MH_STATUS s = MH_CreateHook(g_target, &Hooked_UpdateMatrices, reinterpret_cast<void**>(&g_original));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(UpdateMatrices @ %p) FAILED: %s", g_target, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(g_target);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook(UpdateMatrices) FAILED: %s", MH_StatusToString(s));
        return false;
    }
    MOHW_LOG(kLogFile, "ProjectionAspect hook installed @ %p", g_target);
    return true;
}

void RemoveProjectionAspectHook()
{
    if (g_target)
    {
        MH_DisableHook(g_target);
        MH_RemoveHook(g_target);
        g_target = nullptr;
    }
}

} // namespace mohw
