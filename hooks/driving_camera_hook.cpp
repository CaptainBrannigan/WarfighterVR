#include "driving_camera_hook.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"
#include "../sdk/vr_math.h"
#include "../sdk/mohw_offsets.h"
#include "aiming_controller_hook.h"
#include "companion_bridge.h"
#include "alternating_eye.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_drivingcamera.log";
// MOHWD.exe (image base 0x400000), from the signature match of MOHW.exe's UpdateMatrices (FUN_00707CB0).
constexpr uintptr_t kUpdateMatricesStatic = 0x70CC60;
// MOHWD.exe's CommitViewTransform (FUN_00926dd0, 2026-10-14) -- see CAMERA SOURCE below.
constexpr uintptr_t kCommitViewStatic = 0x926DD0;

// Camera object layout (floats), the same as MOHW.exe's (projection_aspect_hook.cpp): 0-2 left, 4-6 up, 8-10
// forward, 12-14 position, int at 0x10 = projection type (0 = perspective), 0x12 = field of view (radians).
using UpdateMatricesFn = void(__fastcall*)(float* self, void* edx);
UpdateMatricesFn g_original = nullptr;

// CAMERA SOURCE (2026-10-14): MOHW.exe's working hook point, CommitViewTransform, carried over after all -- found from
// the caller chains of the views' rebuilds: the HUD icon code (FUN_012cb580) copies the renderer singleton's
// (DAT_02781840, vtable 0x238B430) view at renderer+0x20 (vfunc +0x6C returns it; FUN_0070ab60 copies 0x28 dwords) into
// a view of its own, and the one function writing renderer+0x20 is FUN_00926dd0: __thiscall(renderer, view), a 0x290
// dword copy of the new view, then the same near/far/fov setters as MOHW.exe's FUN_008ab580 (which copies 0x394 dwords
// into renderer+0x50). Called once, from renderer vfunc +0x14 (FUN_0092d580). The view there has the camera layout
// above. Every view the game draws the frame with is derived from it -- so the head rotation, eye shift and eye frustum
// go in HERE, on a copy (the game's own view is never written), and the views downstream are consistent by
// construction. Patching those views in UpdateMatrices instead (the first approach) raced the render thread's copies
// and flickered; it remains only for the manual range (debugging).
using CommitViewFn = void(__fastcall*)(void* renderer, void* edx, const float* view);
CommitViewFn g_originalCommit = nullptr;
constexpr int kViewDwords = 0x290;

struct CameraStats
{
    float* camera = nullptr;
    unsigned calls = 0; // this window
    float fov = 0.0f;
    float pos[3] = {};
    float forward[3] = {};
    int type = 0;
    unsigned long tid = 0;          // thread of its latest rebuild
    unsigned presentThreadCalls = 0; // rebuilds this window on the Present (render) thread
};

// The thread that calls Present (the render thread, where the alternating eye flips) -- set by DrivingStereoOnPresent.
std::atomic<unsigned long> g_presentTid{0};
bool OnPresentThread()
{
    return GetCurrentThreadId() == g_presentTid.load(std::memory_order_relaxed);
}

std::mutex g_mutex;                    // guards everything below
std::vector<CameraStats> g_window;      // cameras seen this window
std::vector<CameraStats> g_ranked;      // the last full window, busiest first
unsigned long long g_windowStartMs = 0;
int g_rankLogs = 0;

// A real 3D scene (2026-10-14): the loading screen / menu render through ONE camera (fov 55, 16:9); driving scenes ran
// 27-41 cameras a window. Head look and stereo only apply in a scene -- in menus the frame stays the whole-frame
// floating screen.
constexpr size_t kMinSceneCameras = 8;
std::atomic<unsigned> g_sceneCameras{0}; // cameras in the last full window

// The head's rotation relative to a level straight-ahead, recentred on RequestRecenter.
float g_referenceYaw = 0.0f;
bool g_haveReference = false;
unsigned g_referenceGeneration = 0;

bool ReadCamera(const float* self, CameraStats* out)
{
    __try
    {
        out->type = *reinterpret_cast<const int*>(self + 0x10);
        out->fov = self[0x12];
        for (int i = 0; i < 3; ++i)
        {
            out->pos[i] = self[12 + i];
            out->forward[i] = self[8 + i];
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// A perspective camera with a normal field of view and a real position: the main view (not a shadow cascade, a
// camera-relative copy at the origin, or the 60-degree mirror view).
bool IsMainViewCandidate(const CameraStats& c)
{
    bool normalFov = c.fov > 0.5f && c.fov < 1.8f; // ~29-103 degrees
    bool realPosition = fabsf(c.pos[0]) + fabsf(c.pos[1]) + fabsf(c.pos[2]) > 0.01f;
    return c.type == 0 && normalFov && realPosition;
}

// Source diagnostics (guarded by g_mutex), logged once a window: how the views derived from the committed view look --
// do they keep the eye frustum written at the source, or does the game set its own downstream (in MOHW.exe, an aspect
// written at CommitViewTransform did nothing)?
unsigned g_diagCommits = 0;          // commits this window
unsigned g_diagCommitsTurned = 0;    // of them, turned / given stereo
unsigned long g_diagCommitTid = 0;
unsigned g_diagSourceViews = 0;      // rebuilds of views carrying the committed field of view
unsigned g_diagFrustumKept = 0;      // ... with the committed aspect and viewport offset
float g_diagLostAspect = 0.0f, g_diagLostOffX = 0.0f, g_diagLostScale = 0.0f; // a view that lost them, for the log
int g_diagLogs = 0;

void EndWindowLocked(unsigned long long now)
{
    // Busiest first; ties by address so the order stays put.
    std::sort(g_window.begin(), g_window.end(), [](const CameraStats& a, const CameraStats& b) {
        return a.calls != b.calls ? a.calls > b.calls : a.camera < b.camera;
    });
    unsigned previousScene = g_sceneCameras.load(std::memory_order_relaxed);
    g_sceneCameras.store(static_cast<unsigned>(g_window.size()), std::memory_order_relaxed);
    bool sceneChanged = (previousScene >= kMinSceneCameras) != (g_window.size() >= kMinSceneCameras);
    g_ranked = g_window;
    g_window.clear();
    g_windowStartMs = now;

    if (g_diagCommits > 0 && g_diagLogs < 120)
    {
        ++g_diagLogs;
        MOHW_LOG(kLogFile, "camera source: %u commits/s (%u turned, tid %lu, Present tid %lu); views carrying the committed fov: %u rebuilds, %u arrived with the eye frustum (the rest get its aspect set)",
                  g_diagCommits, g_diagCommitsTurned, g_diagCommitTid, g_presentTid.load(), g_diagSourceViews,
                  g_diagFrustumKept);
        if (g_diagSourceViews > g_diagFrustumKept)
            MOHW_LOG(kLogFile, "  one as it arrived: aspect %.3f, viewport offset x %.3f, scale %.3f", g_diagLostAspect,
                      g_diagLostOffX, g_diagLostScale);
    }
    g_diagCommits = g_diagCommitsTurned = g_diagSourceViews = g_diagFrustumKept = 0;

    // The ranking, when a scene starts or ends and whenever the manual range changes, with its views marked.
    static int loggedFrom = -1, loggedTo = -1;
    int from = GetDrivingCameraIndex(), to = GetDrivingCameraIndexTo();
    if (to < from)
        to = from;
    bool rangeChanged = from != loggedFrom || to != loggedTo;
    loggedFrom = from;
    loggedTo = to;
    if ((sceneChanged || (rangeChanged && from >= 1)) && g_rankLogs < 80)
    {
        ++g_rankLogs;
        MOHW_LOG(kLogFile, "views (%u cameras, manual range %d-%d):", static_cast<unsigned>(g_ranked.size()), from, to);
        for (size_t i = 0; i < g_ranked.size() && i < 48; ++i)
        {
            const CameraStats& c = g_ranked[i];
            int rank = static_cast<int>(i + 1);
            bool inRange = from >= 1 && rank >= from && rank <= to;
            MOHW_LOG(kLogFile, "  #%u %p: %u calls/s (%u on the render thread, last tid %lu), type %d, fov %.1f deg, pos (%.1f, %.1f, %.1f), forward (%.2f, %.2f, %.2f)%s",
                      static_cast<unsigned>(rank), static_cast<void*>(c.camera), c.calls, c.presentThreadCalls, c.tid, c.type,
                      c.fov * 57.2958f, c.pos[0], c.pos[1], c.pos[2], c.forward[0], c.forward[1], c.forward[2],
                      inRange ? "  [in manual range]" : "");
        }
    }
}

// Records the call; true if this camera is in the manual range (debugging). base = the view's own (game) values -- not
// what this hook left in it (see PERSISTENT in Hooked).
bool TrackAndCheckSelected(float* self, const float* base)
{
    unsigned long long now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    CameraStats current{};
    current.camera = self;
    if (!ReadCamera(base, &current))
        return false;
    CameraStats* entry = nullptr;
    for (CameraStats& c : g_window)
        if (c.camera == self)
            entry = &c;
    if (!entry && g_window.size() < 64)
    {
        g_window.push_back(CameraStats{});
        entry = &g_window.back();
        entry->camera = self;
    }
    if (entry)
    {
        unsigned calls = entry->calls + 1;
        unsigned onPresentThread = entry->presentThreadCalls + (OnPresentThread() ? 1u : 0u);
        *entry = current;
        entry->calls = calls;
        entry->presentThreadCalls = onPresentThread;
        entry->tid = GetCurrentThreadId();
    }
    if (g_windowStartMs == 0)
        g_windowStartMs = now;
    if (now - g_windowStartMs >= 1000)
        EndWindowLocked(now);

    // Manual range, for debugging (2026-10-14): every view ranked from..to (busiest first, ties by address). Off (0) =
    // the camera source does it all.
    int manual = GetDrivingCameraIndex();
    if (manual < 1)
        return false;
    int to = GetDrivingCameraIndexTo();
    if (to < manual)
        to = manual;
    for (int rank = manual; rank <= to && rank <= static_cast<int>(g_ranked.size()); ++rank)
        if (g_ranked[rank - 1].camera == self)
            return true;
    return false;
}

// The head relative to a level straight-ahead (recentred on RequestRecenter), split into a yaw about world up (the
// heading of the head's forward) and the rest -- pitch and roll in the head's own frame: relative = yaw * rest. Called
// from the game's threads (the camera hooks) and its input thread (xinput_hook.cpp), hence the mutex.
std::mutex g_headMutex; // guards the reference
bool SplitHead(float* relativeYaw, Quat* pitchRoll)
{
    mohwvr::ipc::HeadPoseBlock pose{};
    if (!GetHeadPose(&pose))
        return false;
    Quat head{pose.orientationX, pose.orientationY, pose.orientationZ, pose.orientationW};
    Vec3 back{};
    back.z = -1.0f;
    Vec3 f = QuatRotateVector(head, back); // head forward (OpenVR -z), tracking space
    float yaw = atan2f(-f.x, -f.z);         // rotation about +y taking (0,0,-1) to f's heading
    float reference = 0.0f;
    {
        std::lock_guard<std::mutex> lock(g_headMutex);
        unsigned generation = GetRecenterGeneration();
        if (!g_haveReference || generation != g_referenceGeneration)
        {
            g_referenceYaw = yaw;
            g_haveReference = true;
            g_referenceGeneration = generation;
            MOHW_LOG(kLogFile, "head reference set (yaw %.1f deg)", yaw * 57.2958f);
        }
        reference = g_referenceYaw;
    }
    float rel = WrapAngleSigned(yaw - reference);
    Quat yawOnly{0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
    *relativeYaw = rel;
    *pitchRoll = QuatMultiply(QuatConjugate(yawOnly), head); // what's left after the heading: pitch, then roll
    return true;
}

// The head rotation the view gets, in its local frame. Live 2026-10-14: the head quaternion's components carry over as
// they are (an earlier x/z flip gave correct yaw but inverted pitch and roll). With a stick look range set
// (DrivingStickLookRange), yaw goes to the game as right-stick look instead (GetDrivingStickLook) -- so the game camera,
// and its culling, really turn -- and the render only adds pitch and roll.
bool HeadRotationInCameraFrame(Quat* out)
{
    float relYaw = 0.0f;
    Quat pitchRoll{};
    if (!SplitHead(&relYaw, &pitchRoll))
        return false;
    Quat q = pitchRoll;
    if (GetDrivingStickLookRangeDeg() <= 0.0f) // render-only yaw as well
        q = QuatMultiply(Quat{0.0f, sinf(relYaw * 0.5f), 0.0f, cosf(relYaw * 0.5f)}, pitchRoll);
    if (GetDrivingHeadLookInvert())
        q = QuatConjugate(q);
    *out = q;
    return true;
}

// SEH-safe float copies.
bool CopyFloats(float* dst, const float* src, int n)
{
    __try
    {
        for (int i = 0; i < n; ++i)
            dst[i] = src[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Turns a view's left/up/forward rows (floats 0-2, 4-6, 8-10) by q, given in the view's own frame.
void TurnRows(float* view, const Quat& q)
{
    Vec3 left{}, up{}, forward{};
    left.x = view[0]; left.y = view[1]; left.z = view[2];
    up.x = view[4]; up.y = view[5]; up.z = view[6];
    forward.x = view[8]; forward.y = view[9]; forward.z = view[10];
    Vec3 axes[3] = {};
    axes[0].x = 1.0f;
    axes[1].y = 1.0f;
    axes[2].z = 1.0f;
    for (int row = 0; row < 3; ++row)
    {
        Vec3 local = QuatRotateVector(q, axes[row]); // the rotated local axis, in the camera's own frame
        Vec3 world = VecAdd(VecAdd(VecScale(left, local.x), VecScale(up, local.y)), VecScale(forward, local.z));
        view[row * 4 + 0] = world.x;
        view[row * 4 + 1] = world.y;
        view[row * 4 + 2] = world.z;
    }
}

// STEREO (2026-10-14, DrivingStereo): each frame renders one eye (alternating-eye, hooks/alternating_eye.h), so the view
// gets (1) a sideways shift of half the scaled IPD along its left row for that eye (alternating_eye's
// GetActiveEyeOffsetAlongRow0, the same offset MOHW.exe's ApplyEyeOffset adds), and (2) that eye's real asymmetric
// frustum -- the same math and fields as MOHW.exe's true frustum (eye_matched_fov.cpp + projection_aspect_hook.cpp
// WriteFrustumProjection) -- but at THIS build's field offsets: its camera class has four more floats after far (+0x54),
// found by comparing the two builds' UpdateMatrices -> projection builder calls (MOHW FUN_00704C70 vs MOHWD
// FUN_00706FC0, 2026-10-14). MOHW.exe: aspect 0x16, viewport offset ptr 0x1B, scale ptr 0x1D; MOHWD.exe: aspect 0x1A,
// offset ptr 0x1F, scale ptr 0x23 (fov 0x12, near/far 0x14/0x15 unchanged). Writing MOHW.exe's offsets here (first try)
// landed on other fields and displaced the view.
constexpr int kFovIdx = 0x12, kAspectIdx = 0x1A, kOffsetIdx = 0x1F, kScaleIdx = 0x23; // float indices, MOHWD.exe
constexpr int kSavedFloats = kScaleIdx + 2; // transform + projection fields through the viewport scale
// Eye trace (DrivingStereoOnPresent): stereo views per eye since the last Present -- commits at the source, and manual
// range rebuilds.
std::atomic<unsigned> g_traceCommit[2] = {{0}, {0}};
std::atomic<unsigned> g_traceManual[2] = {{0}, {0}};
std::atomic<unsigned long long> g_lastStereoMs{0};
// The eye frustum last written at the source (for the source diagnostics).
std::atomic<uint32_t> g_committedFovBits{0};
std::atomic<float> g_committedAspect{0.0f};
std::atomic<float> g_committedOffX{0.0f};

bool ComputeEyeFrustum(int eye, float* fovY, float* aspect, float* offX, float* offY)
{
    mohwvr::ipc::HmdViewBlock hmd{};
    if (!GetHmdView(&hmd) || hmd.viewMode != mohwvr::ipc::kViewModeTrueFrustum)
        return false;
    float k = GetFrustumScale();
    if (!(k > 0.2f && k < 4.0f))
        k = 1.0f;
    float tL = tanf(hmd.angleLeft[eye]) * k, tR = tanf(hmd.angleRight[eye]) * k;
    float tU = tanf(hmd.angleUp[eye]) * k, tD = tanf(hmd.angleDown[eye]) * k;
    float T = (tU - tD) * 0.5f, width = tR - tL;
    if (!(T > 0.05f && width > 0.05f))
        return false;
    *aspect = width / (2.0f * T);
    *offX = ((tL + tR) * 0.5f) / (*aspect * T * 2.0f);
    *offY = ((tU + tD) * 0.5f) / (T * 2.0f);
    *fovY = 2.0f * atanf(T);
    if (*fovY > 3.05f)
        *fovY = 3.05f;
    return true;
}

// Gives view (transform already turned) the eye's shift and frustum. otherEye: the eye after the active one (the
// offsets of the two eyes are mirror images). False if the frustum isn't available.
bool ApplyEye(float* view, int eye, bool otherEye, float* fovOut, float* aspectOut, float* offXOut)
{
    float fovY = 0.0f, aspect = 0.0f, offX = 0.0f, offY = 0.0f;
    if (!ComputeEyeFrustum(eye, &fovY, &aspect, &offX, &offY))
        return false;
    // Eye shift along the turned left row (the eyes sit across the head, wherever it looks).
    float e = GetActiveEyeOffsetAlongRow0() * (otherEye ? -1.0f : 1.0f);
    for (int i = 0; i < 3; ++i)
        view[12 + i] += view[i] * e;
    view[kFovIdx] = fovY;
    view[kAspectIdx] = aspect;
    view[kOffsetIdx] = offX;
    view[kOffsetIdx + 1] = offY;
    view[kScaleIdx] = 1.0f;
    view[kScaleIdx + 1] = 1.0f;
    g_lastStereoMs.store(GetTickCount64(), std::memory_order_relaxed);
    *fovOut = fovY;
    *aspectOut = aspect;
    *offXOut = offX;
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1, std::memory_order_relaxed) < 4)
        MOHW_LOG(kLogFile, "stereo eye %d: offset %.4f m, fovY %.1f deg, aspect %.3f, viewport offset (%.3f, %.3f)", eye, e,
                  fovY * 57.2958f, aspect, offX, offY);
    return true;
}

// The source: turns a copy of the committed view by the head and gives it this frame's eye. False = pass the game's
// view through untouched (head look off, the manual range in use, or not a 3D scene).
bool PrepareSourceView(const float* view, float* out, int* eyeOut)
{
    if (!GetDrivingHeadLookAuto() || GetDrivingCameraIndex() >= 1)
        return false;
    if (g_sceneCameras.load(std::memory_order_relaxed) < kMinSceneCameras)
        return false;
    if (!CopyFloats(out, view, kViewDwords))
        return false;
    CameraStats c{};
    if (!ReadCamera(out, &c) || !IsMainViewCandidate(c))
        return false;
    Quat q{};
    if (!HeadRotationInCameraFrame(&q))
        return false;
    TurnRows(out, q);
    *eyeOut = -1;
    if (GetDrivingStereo())
    {
        // The commit runs on the game's main thread; the eye flips at Present on the render thread. If the main thread
        // works a frame ahead, the view committed now is presented as the NEXT eye (DrivingGameThreadNextEye).
        bool next = GetDrivingLateNextEye();
        int eye = (IsRightEyeActive() ? 1 : 0) ^ (next ? 1 : 0);
        float fov = 0.0f, aspect = 0.0f, offX = 0.0f;
        if (ApplyEye(out, eye, next, &fov, &aspect, &offX))
        {
            *eyeOut = eye;
            uint32_t bits = 0;
            memcpy(&bits, &fov, sizeof(bits));
            g_committedFovBits.store(bits, std::memory_order_relaxed);
            g_committedAspect.store(aspect, std::memory_order_relaxed);
            g_committedOffX.store(offX, std::memory_order_relaxed);
        }
    }
    return true;
}

// PAUSE (2026-10-14): MOHW.exe tells its in-game menu apart by the simulation stopping (no AimingController update for
// 300 ms, hooks/draw_trace_diag.cpp IsSimulationPaused). This build has no hook there, so its signal is the game's own
// committed view: in play the vehicle camera moves with every simulation tick; paused, it is frozen bit for bit (the
// head's turn is only added to our copy). Main thread; the time of the last change, for any thread.
std::atomic<unsigned long long> g_viewChangedMs{0};

void NoteViewChange(const float* view)
{
    float transform[16];
    if (!view || !CopyFloats(transform, view, 16))
        return;
    static float last[16];
    static bool haveLast = false;
    if (haveLast && memcmp(transform, last, sizeof(last)) == 0)
        return;
    memcpy(last, transform, sizeof(last));
    haveLast = true;
    g_viewChangedMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void __fastcall HookedCommit(void* renderer, void* edx, const float* view)
{
    NoteViewChange(view);
    float local[kViewDwords];
    int eye = -1;
    bool turned = view != nullptr && PrepareSourceView(view, local, &eye);
    if (eye >= 0)
        g_traceCommit[eye].fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_diagCommits;
        if (turned)
            ++g_diagCommitsTurned;
        g_diagCommitTid = GetCurrentThreadId();
    }
    static std::atomic<int> logs{0};
    if (logs.load(std::memory_order_relaxed) < 3 && view)
    {
        CameraStats c{};
        if (ReadCamera(view, &c) && IsMainViewCandidate(c))
        {
            logs.fetch_add(1, std::memory_order_relaxed);
            MOHW_LOG(kLogFile, "camera source commit (renderer %p, view %p, tid %lu): fov %.1f deg, pos (%.1f, %.1f, %.1f), forward (%.2f, %.2f, %.2f); %s",
                      renderer, static_cast<const void*>(view), GetCurrentThreadId(), c.fov * 57.2958f, c.pos[0], c.pos[1],
                      c.pos[2], c.forward[0], c.forward[1], c.forward[2], turned ? "turned" : "passed through");
        }
    }
    g_originalCommit(renderer, edx, turned ? local : view);
}

// SOURCE ASPECT (2026-10-14): live, the views built from the committed view kept its fov and viewport offsets but every
// one had aspect 1.778 again (the game sets it downstream -- in MOHW.exe too, aspect written at CommitViewTransform did
// nothing; projection_aspect_hook.cpp sets it in UpdateMatrices). So a view rebuilt with the committed fov (bit for bit)
// gets the eye frustum's aspect here, right before its matrices are built. Aspect is the same for both eyes, so unlike
// the per-view stereo patching this can't pick up the wrong eye. Also the diagnostics: whether it was needed.
void FixSourceView(float* self, const float* current)
{
    if (GetDrivingCameraIndex() >= 1)
        return; // the manual range does its own
    uint32_t bits = 0;
    memcpy(&bits, &current[kFovIdx], sizeof(bits));
    uint32_t committed = g_committedFovBits.load(std::memory_order_relaxed);
    if (committed == 0 || bits != committed)
        return;
    float aspect = g_committedAspect.load(std::memory_order_relaxed);
    float offX = g_committedOffX.load(std::memory_order_relaxed);
    bool kept = current[kAspectIdx] == aspect && (current[kOffsetIdx] == offX || current[kOffsetIdx] == -offX);
    if (current[kAspectIdx] != aspect)
        CopyFloats(self + kAspectIdx, &aspect, 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_diagSourceViews;
    if (kept)
    {
        ++g_diagFrustumKept;
    }
    else
    {
        g_diagLostAspect = current[kAspectIdx];
        g_diagLostOffX = current[kOffsetIdx];
        g_diagLostScale = current[kScaleIdx];
    }
}

// MANUAL RANGE (debugging) -- PERSISTENT (2026-10-14): the stereo values used to be written for the rebuild and the
// game's put back right after, but the render thread copies these views to draw from, and copies made inside that window
// carried our values while others didn't: the flicker. So they stay in the view. The game's own values are kept per
// view, FIELD BY FIELD (the game rewrites a view's transform each frame but not its projection fields): a field still
// holding what this hook wrote is replaced by its stored original; one the game rewrote is taken as is. A view that
// drops out of the range gets its own values back.
struct ViewState
{
    float base[kSavedFloats];
    float written[kSavedFloats];
};
std::mutex g_viewsMutex;
std::unordered_map<float*, ViewState> g_views; // guarded by g_viewsMutex

// True if the view had stored values (it was turned last time).
bool TakeBase(float* self, const float* current, float* base)
{
    std::lock_guard<std::mutex> lock(g_viewsMutex);
    auto it = g_views.find(self);
    if (it == g_views.end())
    {
        memcpy(base, current, sizeof(float) * kSavedFloats);
        return false;
    }
    for (int i = 0; i < kSavedFloats; ++i)
        base[i] = current[i] == it->second.written[i] ? it->second.base[i] : current[i];
    return true;
}

void RememberView(float* self, const float* base, const float* written)
{
    std::lock_guard<std::mutex> lock(g_viewsMutex);
    if (g_views.size() > 256 && g_views.find(self) == g_views.end())
        g_views.clear(); // stale views from an earlier scene
    ViewState& s = g_views[self];
    memcpy(s.base, base, sizeof(s.base));
    memcpy(s.written, written, sizeof(s.written));
}

void ForgetView(float* self)
{
    std::lock_guard<std::mutex> lock(g_viewsMutex);
    g_views.erase(self);
}

void __fastcall Hooked(float* self, void* edx)
{
    float current[kSavedFloats];
    if (!self || !CopyFloats(current, self, kSavedFloats))
    {
        g_original(self, edx);
        return;
    }
    FixSourceView(self, current);
    float saved[kSavedFloats];
    bool heldOurs = TakeBase(self, current, saved);
    Quat q{};
    bool selected = TrackAndCheckSelected(self, saved);
    if (!selected || !HeadRotationInCameraFrame(&q))
    {
        if (heldOurs) // no longer turned: give it its own values back
        {
            CopyFloats(self, saved, kSavedFloats);
            ForgetView(self);
        }
        g_original(self, edx);
        return;
    }
    float changed[kSavedFloats];
    memcpy(changed, saved, sizeof(changed));
    TurnRows(changed, q);
    if (GetDrivingStereo())
    {
        // Rebuilt off the render thread (the game's main thread, likely a frame ahead): the next frame's eye, with
        // DrivingGameThreadNextEye on.
        bool next = GetDrivingLateNextEye() && !OnPresentThread();
        int eye = (IsRightEyeActive() ? 1 : 0) ^ (next ? 1 : 0);
        float fov = 0.0f, aspect = 0.0f, offX = 0.0f;
        if (ApplyEye(changed, eye, next, &fov, &aspect, &offX))
            g_traceManual[eye].fetch_add(1, std::memory_order_relaxed);
    }
    CopyFloats(self, changed, kSavedFloats);
    g_original(self, edx);
    float after[kSavedFloats];
    if (CopyFloats(after, self, kSavedFloats))
        RememberView(self, saved, after);
}

template <typename Fn>
bool InstallOne(uintptr_t staticAddress, Fn detour, Fn* original, const char* name)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    void* target = reinterpret_cast<void*>(base - 0x400000 + staticAddress);
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    if (s == MH_OK)
        s = MH_EnableHook(target);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "hooking %s @ %p FAILED: %s", name, target, MH_StatusToString(s));
        return false;
    }
    MOHW_LOG(kLogFile, "hooked %s @ %p", name, target);
    return true;
}

} // namespace

void DrivingStereoOnPresent(bool presentedRightEye)
{
    g_presentTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    unsigned commit0 = g_traceCommit[0].exchange(0), commit1 = g_traceCommit[1].exchange(0);
    unsigned manual0 = g_traceManual[0].exchange(0), manual1 = g_traceManual[1].exchange(0);
    if (!g_drivingExe || commit0 + commit1 + manual0 + manual1 == 0)
        return;
    // Frames whose stereo views don't line up one-to-one with the presented eye: no commit, more than one, or the
    // other eye's -- each a candidate for a frame shown with the wrong eye's view.
    static unsigned frames = 0, odd = 0, logs = 0;
    ++frames;
    unsigned right = presentedRightEye ? commit1 : commit0;
    unsigned wrong = presentedRightEye ? commit0 : commit1;
    bool isOdd = manual0 + manual1 == 0 && (right != 1 || wrong != 0);
    if (isOdd)
        ++odd;
    if (isOdd && logs++ < 200)
        MOHW_LOG(kLogFile, "frame %u presented as eye %d: commits since last Present eye0 %u eye1 %u", frames,
                  presentedRightEye ? 1 : 0, commit0, commit1);
    if (frames % 600 == 0)
        MOHW_LOG(kLogFile, "eye trace: %u of %u frames without exactly one commit for the presented eye", odd, frames);
}

bool DrivingStereoActive()
{
    unsigned long long last = g_lastStereoMs.load(std::memory_order_relaxed);
    return last != 0 && GetTickCount64() - last < 300;
}

unsigned long long GetDrivingViewChangedMs()
{
    return g_viewChangedMs.load(std::memory_order_relaxed);
}

bool DrivingSimulationPaused()
{
    unsigned long long last = g_viewChangedMs.load(std::memory_order_relaxed);
    return g_drivingExe && last != 0 && GetTickCount64() - last > 300;
}

bool GetDrivingStickLook(float* stickX)
{
    float range = GetDrivingStickLookRangeDeg();
    if (!g_drivingExe || range <= 0.0f || (!GetDrivingHeadLookAuto() && GetDrivingCameraIndex() < 1))
        return false;
    if (DrivingSimulationPaused())
        return false; // the menu: no head yaw on the right stick
    float relYaw = 0.0f;
    Quat pitchRoll{};
    if (!SplitHead(&relYaw, &pitchRoll))
        return false;
    // Head turned right = negative yaw (rotation about +y); stick right = positive X.
    float x = -relYaw / (range * 3.14159265f / 180.0f);
    if (x > 1.0f)
        x = 1.0f;
    if (x < -1.0f)
        x = -1.0f;
    // The game ignores small deflections: anything past a hair starts just above its deadzone.
    float deadzone = GetDrivingStickDeadzone();
    if (fabsf(x) > 0.01f && deadzone > 0.0f && deadzone < 0.9f)
        x = (x > 0.0f ? 1.0f : -1.0f) * (deadzone + (1.0f - deadzone) * fabsf(x));
    *stickX = x;
    return true;
}

bool InstallDrivingCameraHook()
{
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(s));
        return false;
    }
    bool matrices = InstallOne(kUpdateMatricesStatic, &Hooked, &g_original, "UpdateMatrices");
    bool commit = InstallOne(kCommitViewStatic, &HookedCommit, &g_originalCommit, "CommitViewTransform");
    return matrices && commit;
}

} // namespace mohw
