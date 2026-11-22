#include "camera_matrix_test_hook.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/mohw_common.h"
#include "../sdk/vr_math.h"
#include "alternating_eye.h"
#include "companion_bridge.h"
#include "head_position.h"
#include "../sdk/settings.h"
#include "fov_scale_hook.h"
#include "aiming_controller_hook.h"
#include "boat_seat.h"
#include "first_person_rig.h"
#include "render_pose_stamp.h"
#include "native_stereo.h"
#include "holster_probe.h"
#include "perf_probe.h"
#include "scope_view.h"
#include "weapon_identity.h"
#include "../sdk/rigid.h"
#include "../openvr_direct/vr_hands.h"
#include "../openvr_direct/vr_overlay.h"
#include "../openvr_direct/vr_reload.h"
#include "../openvr_direct/vr_magazine_path.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_cameramatrix.log";
constexpr int kGroupSize = 12;      // first-person group: 12 batches per view (cnt4, 9 singles, cnt3)
constexpr int kSelectAll = -1;

using CameraMatrixSetFn = void(__thiscall*)(void* thisPtr, const float* matrix);

// "Player skeleton loaded" signal (2026-09-22) -- see IsPlayerSkeletonLoaded's own comment in the header.
std::atomic<DWORD> g_lastViewmodelMatrixTick{0};

CameraMatrixSetFn g_original = nullptr;
void* g_hookAddress = nullptr;
std::atomic<int> g_patched{0};
std::atomic<int> g_selected{0};
std::atomic<int> g_dumpRemaining{0};
std::atomic<int> g_lastLoggedSelected{-2};
std::atomic<int> g_seqLogLines{0};

// FUN_008B2AB0 hands the 12 first-person batches to this call site in a fixed
// order per view: the count==4 batch first, then 9 singles, then the count==3
// batch. Per-thread because different views are built on different threads.
thread_local uintptr_t t_lastBatch = 0;
thread_local int t_seq = -1;
thread_local uint32_t t_lastCount = 0;

// The viewmodel camera FUN_008B2AB0 just set a matrix on, for ConsumeViewmodelCamera: the builder then runs
// UpdateMatrices on that same (stack-local) camera on this thread. The fov field is recorded too, so a different
// camera that later happens to reuse the stack address can't pick up a leftover.
thread_local const void* t_pendingViewmodelCamera = nullptr;
thread_local uint32_t t_pendingViewmodelFovBits = 0;

// NATIVE TWO-VIEW (hooks/native_stereo.h): FUN_008B2AB0 first sets the view's raw camera on its batch camera
// (OFFSET_CAMERAMATRIX_CALLER_VIEWSTATE, absolute), then the batch-relative copy this hook rewrites. With both eyes in one
// frame, that first camera's position says which eye's view the batch is for. The eye then picks the rig's eye offset
// (SetThreadEyeOverride) and the gun's frustum (projection_aspect_hook.cpp, through LastViewmodelEye).
thread_local float t_viewStatePos[3] = {};
thread_local bool t_viewStateValid = false;
thread_local int t_viewmodelEye = -1;
// Native two-view: this job thread's last body batch's frame-bone rotations, and which built frame it was.
thread_local float t_viewRows[3][3][3] = {};
thread_local unsigned t_viewRowsFrame = 0;
std::atomic<long> g_nativeEyeBatches[3] = {{0}, {0}, {0}}; // left, right, not matched

void LogNativeEyesIfDue()
{
    static std::atomic<unsigned long long> nextLogMs{0};
    const unsigned long long now = GetTickCount64();
    unsigned long long due = nextLogMs.load(std::memory_order_relaxed);
    if (now < due || !nextLogMs.compare_exchange_strong(due, now + 5000, std::memory_order_relaxed))
        return;
    MOHW_LOG(kLogFile,
             "native two-view, last 5 s: first-person batch cameras for the left eye %ld, the right eye %ld, not matched "
             "%ld (those keep the frame's eye, the left)",
             g_nativeEyeBatches[0].exchange(0), g_nativeEyeBatches[1].exchange(0), g_nativeEyeBatches[2].exchange(0));
}

// 0 = use the index selector; 1 = weapon batches only (IsBodyBatch false);
// 2 = body batch only. (Was count != 3 / == 3, confirmed 2026-09-19 for the
// first rifle and pistol; other levels broke it, see IsBodyBatch.)
std::atomic<int> g_roleMode{1}; // default: weapon only

using DrawItemSubmitFn = void(__cdecl*)(void* item, void* drawState);
DrawItemSubmitFn g_originalSubmit = nullptr;
void* g_submitAddress = nullptr;

// ---- Controller weapon drive (v3, 2026-09-26) ---------------------------------------------------------------
// Draws the weapon batches (count != 3) as if the first-person rig were rigidly held by the right controller.
// Numpad . toggles it; the gun snaps straight onto the controller at its grip J (GripInCamera: since 2026-11-09 the
// gun's own hand socket where the arm IK holds the hand; a per-weapon grip point before), wherever the hand happens to
// be when the key is pressed.
//
// M (the matrix this hook sees) is the local CAMERA the batch is drawn from, not the gun (the translation test showed
// moving it by d moves the gun by -d). The gun itself is camera-attached, G = L * M. Drawn with camera M' and seen
// from the real camera M, the gun appears at G * M'^-1 * M. The wanted pose is the gun's rest pose carried by the
// controller, with the controller at the grip J (its pose relative to the camera when the gun is at rest). Solving:
// M' = M * C^-1 * J * M. J's rotation is identity (controller pointing along the view = gun pointing along the view)
// and its translation is the grip point, so the gun lands on the hand instead of keeping the hand's offset from it.
//
// C = the controller's pose in the same batch-origin-relative space as M: axes = its tracking-space axes through
// TrackingDirectionToGameWorld (the proven head-position mapping); position = the head-centre camera (M's position
// minus this eye's IPD offset, see GetActiveEyeOffsetAlongRow0) + TrackingOffsetToGameWorld(controller - head).
// Row-vector convention throughout: world = local * R + t, and "A * B" means apply A, then B.
// Replaces v2 (2026-09-20), which derived its own XR->game alignment at calibration and auto-guessed axis signs.
// The drive itself is the WeaponDriveEnabled setting (default on, so it applies from the first weapon draw after a
// character loads); Numpad . toggles and saves it.
std::atomic<bool> g_weaponGripLogRequested{true}; // log the hand's pose in camera space once, on the next weapon draw

// WHERE THE FRAME IS SHOWN FROM (2026-11-01): M here is the game's raw 30 Hz camera, but the world is drawn from the
// mod's smoothed one, and SteamVR is told so by the render-pose stamp. A frame through M for "the world" put the gun on
// that staircase, so on a head turn it jumped against the smooth world (live: the two cameras' rows matched to 0.04 deg
// for M, so it wasn't the other way round). So a controller is taken relative to the head pose the frame is shown from,
// which is the frame the view's own axes are in (x right, y up, z back, as the laser's RENDER MAPPING), then carried
// through M only so the formulas above cancel it: M * C^-1 = H^-1, with H the pose in the view.
bool ShownHeadPose(Quat* orientation, float position[3])
{
    mohwvr::ipc::HeadPoseBlock head{};
    if (!GetHeadPose(&head))
        return false;
    *orientation = Quat{head.orientationX, head.orientationY, head.orientationZ, head.orientationW};
    position[0] = head.positionX;
    position[1] = head.positionY;
    position[2] = head.positionZ;
    float stampQ[4], stampP[3];
    bool stampPosValid = false;
    if (GetCommittedFrameRenderPose(stampQ, stampP, &stampPosValid))
    {
        *orientation = Quat{stampQ[0], stampQ[1], stampQ[2], stampQ[3]};
        if (stampPosValid)
            memcpy(position, stampP, sizeof(stampP));
    }
    return true;
}

// A tracking-space frame (axes, position) as a frame like M: in the view of this eye, then through m.
bool FrameInView(const float m[16], const Vec3 axes[3], const Vec3& position, Rigid* out)
{
    Quat headQ{};
    float headP[3];
    if (!ShownHeadPose(&headQ, headP))
        return false;
    Quat headInverse = QuatConjugate(headQ);
    Rigid inView{};
    for (int i = 0; i < 3; ++i)
    {
        Vec3 axis = QuatRotateVector(headInverse, axes[i]);
        inView.R[i][0] = axis.x;
        inView.R[i][1] = axis.y;
        inView.R[i][2] = axis.z;
    }
    Vec3 relative;
    relative.x = position.x - headP[0];
    relative.y = position.y - headP[1];
    relative.z = position.z - headP[2];
    Vec3 local = QuatRotateVector(headInverse, relative);
    float scale = GetHeadPositionScale();
    if (!(scale > 0.01f))
        scale = 1.0f;
    inView.t[0] = local.x * scale - GetActiveEyeOffsetAlongRow0();
    inView.t[1] = local.y * scale;
    inView.t[2] = local.z * scale;
    *out = Compose(inView, FromMatrix(m));
    return true;
}

// The weapon hand's controller (the "right" slot), or the off hand's, in tracking space: its axes (right, up, back)
// and position.
bool ControllerInTracking(bool offHand, Vec3 axes[3], Vec3* position)
{
    mohwvr::ipc::ControllerPoseBlock controller{};
    if (!(offHand ? GetLeftControllerPose(&controller) : GetRightControllerPose(&controller)))
        return false;

    Quat q{controller.orientationX, controller.orientationY, controller.orientationZ, controller.orientationW};
    for (int i = 0; i < 3; ++i)
    {
        Vec3 e{};
        e.x = i == 0 ? 1.0f : 0.0f;
        e.y = i == 1 ? 1.0f : 0.0f;
        e.z = i == 2 ? 1.0f : 0.0f;
        axes[i] = QuatRotateVector(q, e);
    }
    position->x = controller.positionX;
    position->y = controller.positionY;
    position->z = controller.positionZ;
    return true;
}

// The same controller as a frame like M (FrameInView).
bool ControllerFrame(const float m[16], bool offHand, Rigid* out)
{
    Vec3 axes[3], position{};
    return ControllerInTracking(offHand, axes, &position) && FrameInView(m, axes, position, out);
}

// THE GUN AT ITS HAND SOCKET (2026-11-08): where the equipped gun's IK_Joint_RightHand is in the rig's camera frame,
// read from the body batch's first pass of each frame, before the arm IK moves it (first_person_rig.h
// ReadGunSocketInCamera), and used by the weapon batches: the latest one. (It used to prefer this thread's own
// reading, which could be a later pass's, taken after the IK; and both views read the same values now, one pose
// against the head centre.) Before the first reading (the skeleton not settled yet at a level's start), the OBR's.
// [0] the socket, [1] the right hand's centre, same pose.
constexpr float kFallbackSocket[2][3] = {{0.135f, -0.177f, -0.129f}, {0.104f, -0.178f, -0.193f}};
std::mutex g_socketMutex;
float g_socket[2][3] = {};          // guarded by g_socketMutex
unsigned long long g_socketMs = 0;  // likewise
Rigid g_gunNow{};                   // likewise: the gun bone against the camera, same reading (HELD POSE)
bool g_haveGunNow = false;          // likewise

void NoteGunSocket(const float s[3], const float hand[3], const Rigid* gun)
{
    const unsigned long long now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_socketMutex);
    memcpy(g_socket[0], s, sizeof(g_socket[0]));
    memcpy(g_socket[1], hand, sizeof(g_socket[1]));
    g_socketMs = now;
    g_haveGunNow = gun != nullptr;
    if (gun)
        g_gunNow = *gun;
}

// The latest reading's gun bone against the camera. False without one.
bool CurrentGunBone(Rigid* out)
{
    std::lock_guard<std::mutex> lock(g_socketMutex);
    if (!g_haveGunNow)
        return false;
    *out = g_gunNow;
    return true;
}

// Once per equipped weapon: the socket, the game's hand centre it's measured from (the game's own pose: the same in
// every frame of a steady hold, whatever the head and controller do), and the grip it gives.
void LogSocketGripIfNew(const float s[3], const float h[3], const float grip[3])
{
    // 1.5 s after the weapon changes, as the socket log (first_person_rig.cpp LogWeaponSockets): the draw animation over.
    static std::mutex mutex;
    static unsigned seen = ~0u, logged = ~0u;
    static unsigned long long changedAt = 0;
    const unsigned generation = GetEquippedWeaponGeneration();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (generation == logged)
            return;
        const unsigned long long now = GetTickCount64();
        if (generation != seen)
        {
            seen = generation;
            changedAt = now;
            return;
        }
        if (now - changedAt < 1500)
            return;
        logged = generation;
    }
    char key[160] = "", display[64] = "";
    GetEquippedWeapon(key, sizeof(key), display, sizeof(display));
    MOHW_LOG(kLogFile,
             "GUN AT SOCKET: %s: IK_Joint_RightHand in the camera (%.3f, %.3f, %.3f), the game's right hand centre (%.3f, "
             "%.3f, %.3f) -> grip (%.3f, %.3f, %.3f)",
             display[0] ? display : "(unknown weapon)", s[0], s[1], s[2], h[0], h[1], h[2], grip[0], grip[1], grip[2]);
}

// The latest reading; false (the OBR's) before the first.
bool CurrentGunSocket(float s[3], float hand[3])
{
    std::lock_guard<std::mutex> lock(g_socketMutex);
    const bool read = g_socketMs != 0;
    memcpy(s, read ? g_socket[0] : kFallbackSocket[0], sizeof(g_socket[0]));
    memcpy(hand, read ? g_socket[1] : kFallbackSocket[1], sizeof(g_socket[1]));
    return read;
}

// Rotation = roll (about back), then pitch (about right), then yaw (about up), as the grips and the hands' calibrations
// (first_person_rig.cpp AnglesRotation).
Rigid RotationFromDeg(const float deg[3])
{
    const float kRad = 3.14159265f / 180.0f;
    float cp = cosf(deg[0] * kRad), sp = sinf(deg[0] * kRad);
    float cy = cosf(deg[1] * kRad), sy = sinf(deg[1] * kRad);
    float cr = cosf(deg[2] * kRad), sr = sinf(deg[2] * kRad);
    Rigid roll{{{cr, sr, 0}, {-sr, cr, 0}, {0, 0, 1}}, {0, 0, 0}};
    Rigid pitch{{{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}}, {0, 0, 0}};
    Rigid yaw{{{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}}, {0, 0, 0}};
    return Compose(Compose(roll, pitch), yaw);
}

Rigid GripInCamera()
{
    // Rotation = roll (about back), then pitch (about right), then yaw (about up), all in the gun's rest view.
    float deg[3];
    GetWeaponGripRotationDeg(deg);
    // Weapon in the left hand (left-handed mode or a left-hand holster grab): the grip is mirrored across the view's
    // vertical plane -- offset right negated, yaw and roll reversed.
    bool mirrored = openvr_direct::IsWeaponHandLeft();
    if (mirrored)
    {
        deg[1] = -deg[1];
        deg[2] = -deg[2];
    }
    const float kRad = 3.14159265f / 180.0f;
    float cp = cosf(deg[0] * kRad), sp = sinf(deg[0] * kRad);
    float cy = cosf(deg[1] * kRad), sy = sinf(deg[1] * kRad);
    float cr = cosf(deg[2] * kRad), sr = sinf(deg[2] * kRad);
    Rigid roll{{{cr, sr, 0}, {-sr, cr, 0}, {0, 0, 1}}, {0, 0, 0}};
    Rigid pitch{{{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}}, {0, 0, 0}};
    Rigid yaw{{{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}}, {0, 0, 0}};
    Rigid j = Compose(Compose(roll, pitch), yaw);
    // THE GUN AT ITS HAND SOCKET: the gun follows the arm IK's hand (2026-11-09: one calibration, the hand's on its
    // controller; the only way since the same day, when the profiles' grip positions went).
    // J is the controller's pose against the camera with the gun as it is now, so a gun point p shows at
    // (p - J.t) * J.R^T in the controller's frame; the socket s at the point o of that frame takes J.t = s - o * J.R.
    // The arm IK puts the hand's centre h at `off` of that frame (RightHandIk right / up / forward), turned by its
    // calibration R_h (RightHandIk pitch / yaw / roll; first_person_rig.cpp WeaponHandGrip), carrying the hand's pose:
    // so the socket, where the game holds the hand on the gun, rides the hand to off + (s - h) * R_h^T. Plus the gun's
    // own fine offset in the hand (GunInHandOffset*, default 0). The profile's grip rotation still turns the gun about
    // the socket. Read from the bones every frame, so the grip stays in the hand whatever the animation does with the
    // gun. With the gun in the left hand the offsets and the socket's side of the hand are mirrored.
    float s[3], h[3];
    const bool read = CurrentGunSocket(s, h);
    // HELD POSE (2026-11-21, first_person_rig.h GetHeldGunPose): while the player has control, the gun is placed from the
    // pose its weapon settled in -- that socket and hand centre -- and then carried from that pose to the gun's
    // animated one now (heldGun^-1 * gunNow), so nothing the game animates on the gun moves it in the hand.
    float heldSocket[3], heldCentre[3];
    Rigid heldGun{}, gunNow{};
    const bool held = GetPlayerAimDriveWeight() >= 1.0f && GetHeldGunPose(heldSocket, heldCentre, &heldGun) &&
                      CurrentGunBone(&gunNow);
    if (held)
    {
        memcpy(s, heldSocket, sizeof(s));
        memcpy(h, heldCentre, sizeof(h));
    }
    const ArmIkSettings arm = GetArmIkSettings();
    float handDeg[3] = {arm.rightHandDeg[0], arm.rightHandDeg[1], arm.rightHandDeg[2]};
    float off[3] = {arm.rightHandPos[0], arm.rightHandPos[1], -arm.rightHandPos[2]};
    float fine[3];
    GetGunSocketOffset(fine);
    fine[2] = -fine[2];
    float d[3] = {s[0] - h[0], s[1] - h[1], s[2] - h[2]};
    if (mirrored)
    {
        handDeg[1] = -handDeg[1];
        handDeg[2] = -handDeg[2];
        off[0] = -off[0];
        fine[0] = -fine[0];
        d[0] = -d[0];
    }
    const Rigid handR = RotationFromDeg(handDeg);
    float o[3];
    for (int k = 0; k < 3; ++k)
        o[k] = off[k] + fine[k] + d[0] * handR.R[k][0] + d[1] * handR.R[k][1] + d[2] * handR.R[k][2];
    for (int k = 0; k < 3; ++k)
        j.t[k] = s[k] - (o[0] * j.R[0][k] + o[1] * j.R[1][k] + o[2] * j.R[2][k]);
    if (read)
        LogSocketGripIfNew(s, h, j.t);
    // A gun point p of the animated pose (p = q * gunNow) is drawn at p * J'^-1 in the controller's frame; with
    // J' = J * heldGun^-1 * gunNow that is q * heldGun * J^-1: where the held pose's point goes with J.
    if (held)
        j = Compose(Compose(j, Inverse(heldGun)), gunNow);
    return j;
}

// RIG ANCHOR (2026-09-26): the first-person rig is attached to the batch camera M, which is this eye's camera, so the
// rig moved with each eye -- no stereo parallax, so double vision against the world. And head roll is added to the
// world camera only (fov_scale_hook.cpp's ApplyHeadRoll, after this camera is built), so the rig stayed fixed to the
// screen and rolled with the head. Drawn with M' and seen from M, a camera-attached point L ends up at L * M * M'^-1 in
// the view; the world is seen through Rq * M (Rq = that roll) from an eye e along the view's right of the head centre,
// so the rig should be at L * Rq^-1 * T(-e), giving M' = T(e) * Rq * M. The weapon drive builds the same head-centre
// correction into its controller frame instead (ControllerFrame subtracts the eye offset).
void AnchorRigToHead(float m[16], const Rigid& rigCamera, bool addRoll)
{
    // A boat seat (hooks/boat_seat.h): every batch drawn from the head's camera, so the game's pose stays in the boat.
    Rigid head{};
    if (GetBoatHeadDelta(&head))
    {
        Rigid shift = RigidIdentity();
        shift.t[0] = GetActiveEyeOffsetAlongRow0();
        ToMatrix(Compose(Compose(shift, head), rigCamera), m);
        return;
    }
    Rigid shiftAndRoll{};
    for (int i = 0; i < 3; ++i)
        shiftAndRoll.R[i][i] = 1.0f;
    float roll = addRoll ? GetAppliedHeadRoll() : 0.0f;
    if (roll != 0.0f)
    {
        // Same rotation ApplyHeadRoll builds: rows = the local axes turned by a quaternion about local Z.
        Quat q{0.0f, 0.0f, sinf(roll * 0.5f), cosf(roll * 0.5f)};
        for (int i = 0; i < 3; ++i)
        {
            Vec3 axis{};
            axis.x = i == 0 ? 1.0f : 0.0f;
            axis.y = i == 1 ? 1.0f : 0.0f;
            axis.z = i == 2 ? 1.0f : 0.0f;
            Vec3 r = QuatRotateVector(q, axis);
            shiftAndRoll.R[i][0] = r.x;
            shiftAndRoll.R[i][1] = r.y;
            shiftAndRoll.R[i][2] = r.z;
        }
    }
    shiftAndRoll.t[0] = GetActiveEyeOffsetAlongRow0(); // T(e) then Rq: the shift is along the unrolled right
    ToMatrix(Compose(shiftAndRoll, rigCamera), m);
}

// From the game's pose (a) to the driven one (b) by u, 0..1: the rotation blended the short way, the position lerped.
// For scripted stretches (GetPlayerAimDriveWeight), so the gun glides between the controller and the game's animation.
void BlendCameras(const float a[16], const float b[16], float u, float out[16])
{
    const Rigid ra = FromMatrix(a), rb = FromMatrix(b);
    Rigid r = rb;
    BlendRotation(ra.R, rb.R, u, r.R);
    for (int i = 0; i < 3; ++i)
        r.t[i] = ra.t[i] + (rb.t[i] - ra.t[i]) * u;
    ToMatrix(r, out);
}

// Rewrites m (a weapon batch's local camera matrix) so the gun follows the controller. Returns false, leaving m
// untouched, while the drive is off or a pose is unavailable.
bool DriveWeaponFromController(float m[16], const Rigid& rigCamera)
{
    if (!GetWeaponDriveEnabled())
        return false;
    Rigid c{};
    if (!ControllerFrame(m, false, &c))
        return false;
    Rigid cam = FromMatrix(m);

    if (g_weaponGripLogRequested.exchange(false))
    {
        // Where the hand really is relative to the gun's rest view: for tuning the grip point, and a check on the
        // identity-rotation assumption (R rows should be near the unit axes with the controller pointing straight ahead).
        Rigid h = Compose(c, Inverse(cam));
        MOHW_LOG(kLogFile,
                 "WEAPON DRIVE hand in camera space: t=(%.3f, %.3f, %.3f) R=[(%.2f %.2f %.2f) (%.2f %.2f %.2f) "
                 "(%.2f %.2f %.2f)]",
                 h.t[0], h.t[1], h.t[2], h.R[0][0], h.R[0][1], h.R[0][2], h.R[1][0], h.R[1][1], h.R[1][2], h.R[2][0],
                 h.R[2][1], h.R[2][2]);
    }

    ToMatrix(Compose(Compose(Compose(cam, Inverse(c)), GripInCamera()), rigCamera), m);
    return true;
}

// The pouch as a frame like a controller's (rows: right, up, back): the body's axes at the pouch.
bool PouchFrame(const float m[16], const openvr_direct::MagazineView& view, Rigid* out)
{
    Vec3 axes[3];
    axes[0].x = view.bodyRight[0];
    axes[0].y = view.bodyRight[1];
    axes[0].z = view.bodyRight[2];
    axes[1].x = 0.0f;
    axes[1].y = 1.0f;
    axes[1].z = 0.0f;
    axes[2].x = view.bodyBack[0];
    axes[2].y = view.bodyBack[1];
    axes[2].z = view.bodyBack[2];
    Quat headQ{};
    float headP[3];
    if (!ShownHeadPose(&headQ, headP))
        return false;
    Vec3 position; // the pouch is placed from the live head; only its offset from there matters
    position.x = headP[0] + view.pouchFromHead[0];
    position.y = headP[1] + view.pouchFromHead[1];
    position.z = headP[2] + view.pouchFromHead[2];
    return FrameInView(m, axes, position, out);
}

float Length3(const float v[3])
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// The gun's frame at the magazine's seat (gun-local, openvr_direct/vr_magazine_path.h) in the view, through the weapon
// controller frame c: the seat and the gun's axes as the latest magazine draw published them.
bool GunAtSeatInView(const Rigid& c, Rigid* out)
{
    openvr_direct::MagazineGeometry g{};
    if (!openvr_direct::GetMagazineGeometry(&g))
        return false;
    Rigid gunToController{};
    for (int i = 0; i < 3; ++i)
    {
        for (int k = 0; k < 3; ++k)
            gunToController.R[i][k] = g.axes[i][k];
        gunToController.t[i] = g.seat[i];
    }
    *out = Compose(gunToController, c);
    return true;
}

float BlendProgress(const openvr_direct::MagazineView& view)
{
    const unsigned long long elapsed = GetTickCount64() - view.blendStartMs;
    return openvr_direct::SmoothStep01(static_cast<float>(elapsed) / static_cast<float>(openvr_direct::kMagazineBlendMs));
}

// MANUAL RELOAD (2026-10-31, openvr_direct/vr_reload.h): a magazine part's batch, drawn where the magazine is now. Any
// part drawn with M' = M * W^-1 shows at P * W, so W is "the move this part takes". The magazine's own frame
// (mag-local) is the gun's at its seat, so W = (batch -> mag-local) * (mag-local -> view), the second being:
//   on the path, coming out (2026-11-14): PathDisplacement(s) * the gun at the seat -- s = 0 is the gun's own move;
//   held: on the off hand's controller at the offset it was grabbed with (MagazineView::magInHand, 2026-11-18), turned
//     onto the magwell's line and held on it near the seat (the first version's going in, back 2026-11-18); just off
//     the path, gliding from where the path left it;
//   out: the pouch, turned by the pouch rotation.
// False = draw it with the gun.
constexpr float kLockEaseMetres = 0.03f;

float BlendStep(float from, float to, float x)
{
    float t = (x - from) / (to - from);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

bool DriveMagazine(float m[16], const Rigid& rigCamera)
{
    if (!GetManualReloadEnabled() || !GetWeaponDriveEnabled())
        return false;
    float bone[3];
    Rigid c{};
    if (!GetMagazineBonePoint(bone) || !ControllerFrame(m, false, &c))
        return false;
    const Rigid cam = FromMatrix(m);
    const Rigid camInverse = Inverse(rigCamera);
    const MagazineProfile profile = GetMagazineProfile();
    const float aimOffset[3] = {profile.aimPoint[0], profile.aimPoint[1], -profile.aimPoint[2]}; // right, up, back
    float inCamera[3], aimInCamera[3];
    PointTimes(bone, camInverse, inCamera);
    for (int k = 0; k < 3; ++k)
        aimInCamera[k] = inCamera[k] + aimOffset[k];
    openvr_direct::MagazineGeometry geometry{};
    const Rigid gripInverse = Inverse(GripInCamera());
    PointTimes(inCamera, gripInverse, geometry.seat);
    PointTimes(aimInCamera, gripInverse, geometry.aim);
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            geometry.axes[i][k] = gripInverse.R[i][k];
    openvr_direct::PublishMagazineGeometry(geometry);

    const openvr_direct::MagazineView view = openvr_direct::GetMagazineView();
    if (view.state == openvr_direct::MagazineState::InGun)
        return false;
    // batch -> mag-local: the rest camera, then the seated bone point to the origin.
    Rigid toMagLocal = camInverse;
    for (int k = 0; k < 3; ++k)
        toMagLocal.t[k] -= inCamera[k];
    Rigid gunToController{};
    for (int i = 0; i < 3; ++i)
    {
        for (int k = 0; k < 3; ++k)
            gunToController.R[i][k] = geometry.axes[i][k];
        gunToController.t[i] = geometry.seat[i];
    }
    const Rigid gunInView = Compose(gunToController, c);
    const openvr_direct::MagazinePath path = openvr_direct::CurrentMagazinePath();
    Rigid frame{}; // mag-local -> view
    if (view.state == openvr_direct::MagazineState::OnPath)
    {
        frame = Compose(openvr_direct::PathDisplacement(path, view.s), gunInView);
    }
    else if (view.state == openvr_direct::MagazineState::Held)
    {
        Rigid off{};
        if (!ControllerFrame(m, true, &off))
            return false;
        frame = Compose(view.magInHand, off); // where the grab put it in the hand (2026-11-18: no snap)
        // GOING IN (the first version's, 2026-11-01; back 2026-11-18): within the weapon profile's range of the seat, its
        // insertion point stays where the hand has it and it turns to point at the aim point, by alpha 0 at the range's
        // edge and 1 at the lock range; at the lock range it's held on the seat -> aim line, not past the seat, eased in
        // over kLockEaseMetres so reaching the lock doesn't jump. The state machine seats it from there.
        float aimPoint[3], fromSeat[3], axis[3];
        PointTimes(aimOffset, gunInView, aimPoint); // the aim point is aimOffset gun-local
        for (int k = 0; k < 3; ++k)
        {
            fromSeat[k] = frame.t[k] - gunInView.t[k];
            axis[k] = aimPoint[k] - gunInView.t[k];
        }
        const float axisLength = Length3(axis);
        const float d = Length3(fromSeat);
        const float span = profile.range - profile.lockRange;
        float alpha = span > 1e-4f ? (profile.range - d) / span : (d <= profile.range ? 1.0f : 0.0f);
        alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
        if (alpha > 0.0f && axisLength > 1e-3f)
        {
            float along = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                axis[k] /= axisLength;
                along += fromSeat[k] * axis[k];
            }
            along = along > 0.0f ? 0.0f : along;
            const float lock = BlendStep(profile.lockRange + kLockEaseMetres, profile.lockRange, d);
            float position[3], toAim[3], turn[3][3];
            for (int k = 0; k < 3; ++k)
            {
                position[k] = frame.t[k] + (gunInView.t[k] + axis[k] * along - frame.t[k]) * lock;
                toAim[k] = aimPoint[k] - position[k];
            }
            const float toAimLength = Length3(toAim);
            Rigid pointed = gunInView;
            if (toAimLength > 1e-3f)
            {
                for (int k = 0; k < 3; ++k)
                    toAim[k] /= toAimLength;
                if (RotationBetween(axis, toAim, turn))
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            pointed.R[i][j] = gunInView.R[i][0] * turn[0][j] + gunInView.R[i][1] * turn[1][j] +
                                              gunInView.R[i][2] * turn[2][j];
            }
            float blended[3][3];
            BlendRotation(frame.R, pointed.R, alpha, blended);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    frame.R[i][j] = blended[i][j];
            for (int k = 0; k < 3; ++k)
                frame.t[k] = position[k]; // the insertion point is mag-local's origin
        }
        if (view.blendMag)
        {
            const float u = BlendProgress(view);
            if (u < 1.0f)
                frame = openvr_direct::BlendRigid(Compose(view.blendMagFromGun, gunInView), frame, u);
        }
    }
    else if (PouchFrame(m, view, &frame))
    {
        frame = openvr_direct::PouchMagazine(frame); // turned by the pouch rotation (2026-11-18)
    }
    else
    {
        return false;
    }
    ToMatrix(Compose(cam, Inverse(Compose(toMagLocal, frame))), m);
    return true;
}

// THE HAND ON THE MAGAZINE (2026-11-14, "the ik should stick to where it grabbed until the magazine releases"): while
// the magazine is gripped on the gun, the off hand's controller frame the arm IK reaches for is the one stuck to it
// (MagazineView::handInMag, moved along the path with it); when it leaves the path or seats, or the grip lets go, the
// hand glides back to the controller.
void StickOffHandToMagazine(const Rigid& c, Rigid* off)
{
    if (!GetManualReloadEnabled())
        return;
    const openvr_direct::MagazineView view = openvr_direct::GetMagazineView();
    const bool stuck = view.state == openvr_direct::MagazineState::OnPath;
    if (!stuck && !view.blendHand)
        return;
    Rigid gunInView{};
    if (!GunAtSeatInView(c, &gunInView))
        return;
    if (stuck)
    {
        *off = Compose(Compose(view.handInMag, openvr_direct::PathDisplacement(openvr_direct::CurrentMagazinePath(), view.s)),
                       gunInView);
        return;
    }
    const float u = BlendProgress(view);
    if (u < 1.0f)
        *off = openvr_direct::BlendRigid(Compose(view.blendHandFromGun, gunInView), *off, u);
}

// ARMS (2026-10-31): the body batch's arms onto the controllers, hooks/first_person_rig.h. Each hand on its own
// controller (2026-11-09: the weapon hand used to go with the gun, wherever its grip put it); the gun follows the hand
// with "Gun held at its hand socket".
void DriveArms(const unsigned char* batch, const float original[16], const float drawn[16], const Rigid& rigCamera)
{
    // Not in a scripted stretch, nor while the rig is gliding back from one (GetPlayerAimDriveWeight): the arms are the
    // game's then, with the gun.
    if (!GetArmIkEnabled() || !GetWeaponDriveEnabled() || GetPlayerAimDriveWeight() < 1.0f)
        return;
    Rigid c{}, off{};
    if (!ControllerFrame(original, false, &c))
        return;
    const bool haveOff = ControllerFrame(original, true, &off);
    if (haveOff)
        StickOffHandToMagazine(c, &off);
    const Rigid gun = Compose(Compose(Inverse(rigCamera), Inverse(GripInCamera())), c);
    ApplyArmIk(batch, FromMatrix(original), rigCamera, FromMatrix(drawn), gun, c, off, haveOff,
               openvr_direct::IsWeaponTwoHanded(), openvr_direct::IsWeaponHandLeft());
}

// THE WEAPON SOCKET VIEW (2026-11-09, ShowWeaponSockets; "create a debug listing the weapon sockets and showing a line
// from their start to finish"): every weapon bone of this body batch's own pose (first_person_rig.h
// ReadWeaponSocketBones), as a line from its parent's joint to its own with its name, drawn by SteamVR over the frame
// (openvr_direct/vr_overlay.h) where the game draws that bone. A gun bone goes with the gun: on the weapon hand's
// controller through the grip J while the drive has the gun, else where the game's own pose has it against the head. A
// hand bone goes with its hand: on its controller through the hand's calibration while the arm IK has the arms (the off
// hand rides the gun when two-handed, as in ApplyArmIk), else as the game poses it. FrameInView backwards: a point q in
// a controller's frame (game units) is at position + axes * q / scale, a point p against the head centre at
// head + headQ * p / scale. The full list, with each bone's offset from its parent in the gun's own frame, goes to
// mohwvr_weaponsockets.log once per weapon. Once per frame: the left view's batch in two-view mode.
std::atomic<bool> g_socketViewShown{false};

void SocketColour(const char* name, int hand, float rgb[3])
{
    static const struct
    {
        const char* prefix;
        float r, g, b;
    } kColours[] = {{"IK_Joint_RightHand", 0.2f, 1.0f, 0.3f}, {"IK_Joint_LeftHand", 0.2f, 0.85f, 1.0f},
                    {"Wep_Aim", 1.0f, 0.25f, 0.25f},           {"Wep_Mag", 1.0f, 0.85f, 0.2f},
                    {"Wep_Trigger", 1.0f, 0.55f, 0.1f},        {"Wep_Slide", 1.0f, 0.35f, 1.0f},
                    {"Wep_Root", 1.0f, 1.0f, 1.0f}};
    rgb[0] = hand ? 1.0f : 0.6f;
    rgb[1] = hand ? 1.0f : 0.7f;
    rgb[2] = 1.0f;
    if (hand)
        return;
    for (const auto& c : kColours)
        if (strncmp(name, c.prefix, strlen(c.prefix)) == 0)
        {
            rgb[0] = c.r;
            rgb[1] = c.g;
            rgb[2] = c.b;
            return;
        }
}

void PublishWeaponSockets(const unsigned char* batch, const Rigid& rigCamera)
{
    using openvr_direct::kSocketDebugSlots;
    using openvr_direct::SocketDebugLine;
    if (!GetShowWeaponSockets())
    {
        if (g_socketViewShown.exchange(false))
        {
            SocketDebugLine none[kSocketDebugSlots];
            const float zero[3] = {};
            openvr_direct::UpdateSocketDebug(none, zero);
        }
        return;
    }
    const bool gunInLeftHand = openvr_direct::IsWeaponHandLeft();
    WeaponSocketBones bones;
    if (!ReadWeaponSocketBones(batch, rigCamera, gunInLeftHand, &bones))
        return; // a later pass of this frame (the IK has the hands), or no skeleton yet
    Quat headQ{};
    float headP[3];
    if (!ShownHeadPose(&headQ, headP))
        return;
    float scale = GetHeadPositionScale();
    if (!(scale > 0.01f))
        scale = 1.0f;
    Vec3 weaponAxes[3], offAxes[3], weaponPos{}, offPos{};
    const bool haveWeapon = ControllerInTracking(false, weaponAxes, &weaponPos);
    const bool haveOff = ControllerInTracking(true, offAxes, &offPos);
    const float drive = GetPlayerAimDriveWeight();
    const bool gunDriven = GetWeaponDriveEnabled() && drive >= 0.5f && haveWeapon;
    const bool armsDriven = GetArmIkEnabled() && GetWeaponDriveEnabled() && drive >= 1.0f && haveWeapon;
    const bool twoHanded = openvr_direct::IsWeaponTwoHanded();
    const Rigid gripInverse = Inverse(GripInCamera());
    auto onController = [scale](const Vec3 axes[3], const Vec3& position, const float q[3], float out[3]) {
        out[0] = position.x + (axes[0].x * q[0] + axes[1].x * q[1] + axes[2].x * q[2]) / scale;
        out[1] = position.y + (axes[0].y * q[0] + axes[1].y * q[1] + axes[2].y * q[2]) / scale;
        out[2] = position.z + (axes[0].z * q[0] + axes[1].z * q[1] + axes[2].z * q[2]) / scale;
    };
    auto onHead = [&](const float p[3], float out[3]) {
        Vec3 local{};
        local.x = p[0] / scale;
        local.y = p[1] / scale;
        local.z = p[2] / scale;
        const Vec3 w = QuatRotateVector(headQ, local);
        out[0] = headP[0] + w.x;
        out[1] = headP[1] + w.y;
        out[2] = headP[2] + w.z;
    };
    float where[kMaxSocketBones][3];
    for (int k = 0; k < bones.count; ++k)
    {
        const int hand = bones.hand[k];
        if (hand == 1 && armsDriven)
            onController(weaponAxes, weaponPos, bones.handGrip[k], where[k]);
        else if (hand == 2 && armsDriven && !twoHanded && haveOff)
            onController(offAxes, offPos, bones.handGrip[k], where[k]);
        else if (gunDriven && (hand == 0 || (hand == 2 && armsDriven)))
        {
            float q[3];
            PointTimes(bones.joint[k], gripInverse, q);
            onController(weaponAxes, weaponPos, q, where[k]);
        }
        else
            onHead(bones.joint[k], where[k]);
    }
    SocketDebugLine lines[kSocketDebugSlots];
    for (int k = 0; k < bones.count && k < kSocketDebugSlots; ++k)
    {
        SocketDebugLine& s = lines[k];
        const int p = bones.parent[k];
        memcpy(s.end, where[k], sizeof(s.end));
        memcpy(s.start, p >= 0 ? where[p] : where[k], sizeof(s.start));
        s.line = p >= 0;
        if (s.line)
        {
            const float dx = s.end[0] - s.start[0], dy = s.end[1] - s.start[1], dz = s.end[2] - s.start[2];
            if (dx * dx + dy * dy + dz * dz < 0.003f * 0.003f)
                continue; // on its parent's joint: a part this gun doesn't have
        }
        s.visible = true;
        SocketColour(bones.names[k], bones.hand[k], s.rgb);
        snprintf(s.label, sizeof(s.label), bones.hand[k] ? "%s (hand)" : "%s", bones.names[k]);
    }
    openvr_direct::UpdateSocketDebug(lines, headP);
    g_socketViewShown.store(true);
}

// THE BODY (2026-11-01): moves the body batch (legs, torso, arms) as a whole. Shown from M' it appears at P * M'^-1 * M;
// to take a further move B there it's drawn from M * B^-1 * M^-1 * M'. B: the BodyOffset settings along the level body
// frame, and with BodyUpright the camera's tilt taken out, about the camera. The arms' IK works through the camera drawn
// here, so the hands stay on the controllers and only the shoulders move. pivot = the point the upright turn is about:
// the camera the rig was posed against (the head centre in two-view mode, so both eyes turn the body the same).
void MoveBody(float drawn[16], const float original[16], const float pivot[3])
{
    float offset[3];
    GetBodyOffset(offset);
    bool upright = GetBodyUpright();
    // SCRIPTED SCENES (2026-11-12, the user: "both my head movement and the game's camera adjustment move the body
    // around in ingame cutscenes"): the offset is along the camera's facing, which in a scene is the game's animated
    // camera, so the body swung with it. The game's own placement then, faded with the rig (GetPlayerAimDriveWeight).
    const float weight = GetPlayerAimDriveWeight();
    for (float& o : offset)
        o *= weight;
    if (weight < 0.5f)
        upright = false;
    if (offset[0] == 0.0f && offset[1] == 0.0f && offset[2] == 0.0f && !upright)
        return;
    const Rigid cam = FromMatrix(original);
    float forward[3] = {-cam.R[2][0], 0.0f, -cam.R[2][2]}, right[3] = {cam.R[0][0], 0.0f, cam.R[0][2]};
    const float forwardLength = Length3(forward), rightLength = Length3(right);
    if (forwardLength < 1e-3f || rightLength < 1e-3f)
        return;
    Rigid b = RigidIdentity();
    if (upright)
    {
        const float up[3] = {0.0f, 1.0f, 0.0f};
        RotationBetween(cam.R[1], up, b.R);
    }
    float turned[3];
    PointTimes(pivot, b, turned);
    for (int k = 0; k < 3; ++k)
        b.t[k] = pivot[k] - turned[k] + right[k] / rightLength * offset[0] + (k == 1 ? offset[1] : 0.0f) +
                 forward[k] / forwardLength * offset[2];
    ToMatrix(Compose(Compose(Compose(cam, Inverse(b)), Inverse(cam)), FromMatrix(drawn)), drawn);
}

// DISABLED 2026-09-22 ("get rid of the camera test hook for now"): every hotkey this diagnostic used (top-row/numpad
// +/-, VK_DIVIDE role toggle, Numpad 1-9/0 offset+calibration controls, VK_OEM_5 '\' body-visibility toggle,
// VK_OEM_4/6 '[' ']' instance hide, VK_MULTIPLY dump) is now a no-op, freeing every one of those physical keys for
// other features. The body/instance hide and role filter below are real, working functionality worth resurrecting
// as proper settings-menu controls once one exists. The weapon drive's own key is polled once per frame from
// present_hook.cpp instead (CheckWeaponDriveHotkey).
void PollHotkeys()
{
}

// ---- Batch composition log (2026-09-29) ----------------------------------------------------------------------------
// The weapon/body split (count != 3 = weapon, count == 3 = legs+arms+hands, arms/hands = instances 1/2) was verified on
// the first rifle and the pistol only; live, another level's weapon lost its lower receiver (a count==3 weapon batch
// with instances 1/2 skipped) and the next level showed the hands (a body batch of another count or order). Every
// distinct first-person batch (by count + first mesh) is logged once, with each instance's mesh object's vtable and any
// asset-name strings reachable from it, to find a rule that holds across levels.
//
// Result (2026-09-29): no names, one vtable for every mesh, and the counts vary (that level's main weapon batch had 2
// instances, the first rifle's 4) -- but the flags byte (+0xCC) splits them: the body batch 03 (0B on 2026-09-19), every
// weapon batch 41 / 43 / 49 (43 / 4B for the pistol) -- bit 0x40 set on weapons only. Which of the body's parts are
// hidden is the HideBodyPart1..4 settings.
bool IsBodyBatch(const unsigned char* b)
{
    return (b[0xCC] & 0x40) == 0;
}

// SEH-safe: a NUL-terminated printable string of at least 5 characters at p, copied to out.
bool ReadPrintableString(uintptr_t p, char* out, int outSize)
{
    if (p < 0x10000 || p > 0xFFFF0000u) // large-address-aware: strings can be above 2 GB too
        return false;
    __try
    {
        const char* s = reinterpret_cast<const char*>(p);
        int n = 0;
        while (n < outSize - 1 && n < 120)
        {
            char c = s[n];
            if (c == '\0')
                break;
            if (c < 0x20 || c > 0x7E)
                return false;
            out[n++] = c;
        }
        out[n] = '\0';
        return n >= 5 && s[n] == '\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// SEH-safe: appends strings found at obj's first 32 dwords (and, depth 2, at the objects those point to).
void ProbeNames(uintptr_t obj, int depth, char* out, size_t outSize, int* found)
{
    if (obj < 0x10000 || obj > 0xFFFF0000u || *found >= 6) // large-address-aware: the heap is above 2 GB too
        return;
    for (int i = 0; i < 32 && *found < 6; ++i)
    {
        uintptr_t v = 0;
        __try
        {
            v = reinterpret_cast<const uintptr_t*>(obj)[i];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        char str[128];
        if (ReadPrintableString(v, str, sizeof(str)))
        {
            size_t len = strlen(out);
            snprintf(out + len, outSize - len, " [d%d+0x%X]\"%s\"", depth, i * 4, str);
            ++*found;
        }
        else if (depth < 2)
        {
            ProbeNames(v, depth + 1, out, outSize, found);
        }
    }
}

// THE GAME'S OWN RETICLE (2026-11-14): every optic ships as two meshes, opt_<name>_1p_mesh and opt_<name>_1pzoom_1p_mesh
// (Weapons.sb), and the game draws the "1pzoom" one, which carries the reticle (the mohw_weapons_reticle shaders), only
// while zoomed -- and while zoomed it draws only the gun's body and that optic, not the other attachments (mohwvr_
// holsterprobe.log, 2026-11-17). It was kept in the scoped eye for its reticle until 2026-11-17: seen from the real eye
// through the magnified view it had parallax times the magnification, so the scoped eye draws its own holographic
// crosshair instead (hooks/scope_view.h GetScopeMask). The mesh names were read through a pointer among the mesh
// object's first dwords to an object whose +0x58 is the name (hooks/holster_probe.cpp ReadName does the same).

// SEH-safe: the mesh pair of instance i (mesh ptr, second ptr) and the mesh object's first dword (vtable).
bool ReadInstance(const unsigned char* b, uint32_t i, uintptr_t* mesh, uintptr_t* second, uintptr_t* vtable)
{
    __try
    {
        const uintptr_t* inst = *reinterpret_cast<const uintptr_t* const*>(b + 0x78);
        *mesh = inst[i * 2];
        *second = inst[i * 2 + 1];
        *vtable = *mesh ? *reinterpret_cast<const uintptr_t*>(*mesh) : 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::mutex g_seenBatchMutex;
uintptr_t g_seenBatchKeys[256] = {};
int g_seenBatchCount = 0;

void LogBatchIfNew(const unsigned char* b, uint32_t count, bool classifiedBody)
{
    if (!LogsEnabled())
        return; // only a log (its seen-list costs a lock and a scan per batch change)
    uintptr_t mesh0 = 0, second0 = 0, vt0 = 0;
    if (count == 0 || count > 256 || !ReadInstance(b, 0, &mesh0, &second0, &vt0))
        return;
    uintptr_t key = mesh0 ^ (static_cast<uintptr_t>(count) << 28);
    {
        std::lock_guard<std::mutex> lock(g_seenBatchMutex);
        for (int i = 0; i < g_seenBatchCount; ++i)
            if (g_seenBatchKeys[i] == key)
                return;
        if (g_seenBatchCount >= 256)
            return;
        g_seenBatchKeys[g_seenBatchCount++] = key;
    }
    const float* transl = reinterpret_cast<const float*>(b + 0x40);
    MOHW_LOG(kLogFile, "BATCH NEW seq=%d count=%u flags=%02X fov=%.1f transl=(%.3f,%.3f,%.3f) -> %s", t_seq, count,
              static_cast<unsigned>(b[0xCC]), *reinterpret_cast<const float*>(b + 0xC4), transl[0], transl[1], transl[2],
              classifiedBody ? "BODY (parts hidden per HideBodyPart1..4)" : "WEAPON (follows the controller)");
    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    for (uint32_t i = 0; i < count; ++i)
    {
        uintptr_t mesh = 0, second = 0, vt = 0;
        if (!ReadInstance(b, i, &mesh, &second, &vt))
            break;
        char names[1024] = {};
        int found = 0;
        ProbeNames(mesh, 0, names, sizeof(names), &found);
        ProbeNames(second, 0, names, sizeof(names), &found);
        MOHW_LOG(kLogFile, "  instance %u: mesh=%p vtable=%p (image+0x%X) second=%p names:%s", i,
                  reinterpret_cast<void*>(mesh), reinterpret_cast<void*>(vt),
                  static_cast<unsigned>(vt >= moduleBase ? vt - moduleBase : 0), reinterpret_cast<void*>(second),
                  found ? names : " (none)");
    }
}

void __stdcall HookImpl(void* thisPtr, void* batch, const float* matrix, uintptr_t ret)
{
    PerfScope perf(kPerfBatches); // hooks/perf_probe.h
    if (matrix != nullptr && ret == reinterpret_cast<uintptr_t>(Offset<void*>(OFFSET_CAMERAMATRIX_CALLER_VIEWSTATE)))
    {
        memcpy(t_viewStatePos, matrix + 12, sizeof(t_viewStatePos));
        t_viewStateValid = true;
        g_original(thisPtr, matrix);
        return;
    }
    uintptr_t expectedRet = reinterpret_cast<uintptr_t>(Offset<void*>(OFFSET_CAMERAMATRIX_CALLER_VIEWMODEL));
    if (ret != expectedRet || matrix == nullptr || batch == nullptr)
    {
        g_original(thisPtr, matrix);
        return;
    }

    int eye = -1;
    unsigned frame = 0;
    if (t_viewStateValid && IsNativeStereoActive())
    {
        eye = NativeStereoEyeForCamera(t_viewStatePos, &frame);
        g_nativeEyeBatches[eye >= 0 ? eye : 2].fetch_add(1, std::memory_order_relaxed);
        LogNativeEyesIfDue();
    }
    t_viewStateValid = false;
    t_viewmodelEye = eye;
    SetThreadEyeOverride(eye);

    g_lastViewmodelMatrixTick.store(GetTickCount(), std::memory_order_relaxed);
    PollHotkeys();
    t_pendingViewmodelCamera = thisPtr;
    memcpy(&t_pendingViewmodelFovBits, static_cast<const unsigned char*>(thisPtr) + 0x48, sizeof(uint32_t));

    const unsigned char* b = reinterpret_cast<const unsigned char*>(batch);
    uint32_t count = *reinterpret_cast<const uint32_t*>(b + 0x7C);
    const float* transl = reinterpret_cast<const float*>(b + 0x40);
    bool isBody = IsBodyBatch(b);
    // HOLSTER PROBE (hooks/holster_probe.h): the weapon's batches, once per frame (the left view in two-view).
    if (!isBody && (eye == 0 || (eye < 0 && !IsNativeStereoActive())))
        HolsterProbeOnBatch(b);

    uintptr_t batchAddr = reinterpret_cast<uintptr_t>(batch);
    if (batchAddr != t_lastBatch)
    {
        t_lastBatch = batchAddr;
        bool startOfGroup = (count == 4) || (t_lastCount == 3) || (t_seq < 0);
        t_seq = startOfGroup ? 0 : t_seq + 1;
        t_lastCount = count;

        LogBatchIfNew(b, count, isBody);
        if (isBody)
        {
            // The live part count, for the settings menu's body part rows (the count differs between levels).
            SetBodyPartCountSeen(static_cast<int>(count));
            if (count > static_cast<uint32_t>(kMaxBodyParts))
            {
                static std::atomic<int> tooManyLogs{0};
                if (tooManyLogs.fetch_add(1) < 4)
                    MOHW_LOG(kLogFile, "body batch has %u parts, more than the %d the settings can hide -- parts past that stay "
                              "visible", count, kMaxBodyParts);
            }
        }
        int line = g_seqLogLines.fetch_add(1);
        if (line < 60)
            MOHW_LOG(kLogFile, "SEQ tid=%lu seq=%d batch=%p count=%u transl=(%.3f,%.3f,%.3f)", GetCurrentThreadId(), t_seq,
                       batch, count, transl[0], transl[1], transl[2]);
    }

    if (g_dumpRemaining.load() > 0 && g_dumpRemaining.fetch_sub(1) > 0)
    {
        const uintptr_t* inst = *reinterpret_cast<const uintptr_t* const*>(b + 0x78);
        MOHW_LOG(kLogFile,
                   "DUMP tid=%lu seq=%d batch=%p count=%u transl=(%.3f,%.3f,%.3f) flagsByte=%02X fov=%.1f mesh0=%p",
                   GetCurrentThreadId(), t_seq, batch, count, transl[0], transl[1], transl[2],
                   static_cast<unsigned>(b[0xCC]), *reinterpret_cast<const float*>(b + 0xC4),
                   inst ? reinterpret_cast<const void*>(inst[0]) : nullptr);
    }

    int sel = g_selected.load();
    int roleMode = g_roleMode.load();
    bool patchThis;
    if (roleMode == 1)
        patchThis = !isBody;
    else if (roleMode == 2)
        patchThis = isBody;
    else
        patchThis = (sel == kSelectAll) || (t_seq == sel);
    if (roleMode != 0)
        sel = 1000 + roleMode; // for the once-per-change PATCHING log below
    // 16-byte aligned: the original reads it with aligned SSE loads (movaps at 0x006FC2B2). Unaligned it only worked while
    // this frame's layout happened to put it on a 16-byte boundary; one that didn't crashed on the first gun batch of a
    // level (live 2026-11-14, a read of -1).
    alignas(16) float m[16];
    memcpy(m, matrix, sizeof(m));
    // Weapon batches follow the controller when the drive is on (parallax included). Everything else in the rig is
    // anchored to the head centre: the body+hands batch without the head's roll, the weapon with the drive off with
    // it (it's the view's gun then, and should roll with the view).
    Rigid rigCamera = FromMatrix(m);
    // NATIVE TWO-VIEW (2026-11-08, "the gun still looked tiny, reducing the IPD to 0.5 helped"): M is this eye's camera in
    // both modes, but the rig isn't posed against the same camera in both. One eye per frame, the game's own camera is
    // the eye (CommitViewTransform adds the offset), so the rig comes posed against M and the anchor below moves it back
    // to the head centre. In two-view mode the game's camera is the head centre and only the views are moved to the eyes,
    // so the rig comes posed against the head centre already: taking it as posed against M moved it a further eye offset
    // off, the wrong way in each eye, which doubled its parallax (half as far, so half the size). So here the camera it
    // was posed against is M moved back along its right by this eye's offset; the anchor, the weapon drive, the
    // magazine and the arms all work from that, and the anchor comes out as M itself (without head roll).
    // And each eye is aligned to its own view's body bones (both eyes' given one pose, SyncTwoViewBodyPose), not to
    // whatever the game returned last, which could be the other view's copy a tick newer: that turned one eye's body
    // a few degrees for a frame (the "ghost like frame"). Weapon batches use their view's body rows (same job thread).
    const float(*rows)[3][3] = nullptr;
    if (eye >= 0)
    {
        const float e = GetActiveEyeOffsetAlongRow0();
        for (int k = 0; k < 3; ++k)
            rigCamera.t[k] -= rigCamera.R[0][k] * e;
        if (isBody)
        {
            SyncTwoViewBodyPose(b, frame);
            if (ReadBatchFrameRows(b, t_viewRows))
                t_viewRowsFrame = frame;
        }
        if (frame != 0 && t_viewRowsFrame == frame)
            rows = t_viewRows;
    }
    AlignRigToBones(&rigCamera, rows);
    // The gun's hand socket in this pose, for the weapon batches' grip (GripInCamera), before the arm IK below.
    if (isBody)
    {
        float socket[3], hand[3];
        Rigid gunNow{};
        bool haveGun = false;
        if (ReadGunSocketInCamera(b, rigCamera, socket, hand, &gunNow, &haveGun))
            NoteGunSocket(socket, hand, haveGun ? &gunNow : nullptr);
    }
    // The weapon socket view, from the same pose (and the grip just noted), before the arm IK too.
    if (isBody && eye <= 0)
        PublishWeaponSockets(b, rigCamera);
    // SCRIPTED STRETCHES (2026-11-08): while the soldier's aim isn't running (a breach's kick, a cutscene, a locked
    // camera), the gun is the game's again, as with the drive off, blended to and from it (GetPlayerAimDriveWeight).
    const float drive = GetPlayerAimDriveWeight();
    bool driven = false;
    if (patchThis)
    {
        uintptr_t mesh0 = 0, second0 = 0, vtable0 = 0;
        const bool haveMesh = !isBody && ReadInstance(b, 0, &mesh0, &second0, &vtable0);
        if (haveMesh)
            NoteWeaponMesh(mesh0);
        if (drive > 0.0f)
        {
            float game[16];
            AnchorRigToHead(game, rigCamera, isBody); // the game's pose for this batch, the blend's other end
            if (haveMesh && IsMagazineMesh(mesh0))
                driven = DriveMagazine(m, rigCamera);
            driven = driven || DriveWeaponFromController(m, rigCamera);
            if (driven && drive < 1.0f)
                BlendCameras(game, m, drive, m);
        }
    }
    if (!driven)
        AnchorRigToHead(m, rigCamera, isBody);
    if (isBody)
    {
        NoteBodyBatch(b);
        MoveBody(m, matrix, rigCamera.t);
        DriveArms(b, matrix, m, rigCamera);
    }

    // SCOPED EYE (2026-11-14, hooks/scope_view.h): this eye's view is the scope's, its camera inside the gun's optic, so
    // the first-person batches (gun, magazine, arms) go 50 m behind the eye instead: moving the batch camera forward by d
    // moves what it draws back by d. The game's zoom optic too (2026-11-17; it was kept for its reticle): a mesh on the
    // gun seen from the real eye, so the eye's offset from the sight line came out multiplied by the magnification --
    // the reticle wasn't on the impact point unless the eye was exactly on the line. The drawn holographic crosshair
    // marks the impact point instead (hooks/scope_view.h GetScopeMask).
    if (eye >= 0 && eye == GetScopeHiddenEye())
        for (int k = 0; k < 3; ++k)
            m[12 + k] -= m[8 + k] * 50.0f;

    ++g_patched;
    int lastLogged = g_lastLoggedSelected.load();
    if (lastLogged != sel && g_lastLoggedSelected.compare_exchange_strong(lastLogged, sel))
        MOHW_LOG(kLogFile, "PATCHING selector=%d: first hit seq=%d batch=%p count=%u transl=(%.3f,%.3f,%.3f) camera=%p", sel,
                   t_seq, batch, count, transl[0], transl[1], transl[2], thisPtr);

    g_original(thisPtr, m);
    SetThreadEyeOverride(-1);
}

// SEH-safe (no C++ objects in this frame): resolves which instance of a first-person body batch (IsBodyBatch) this draw
// item is, from the item's mesh ptr and matrix ptr.
bool ResolveBodyInstance(void* item, int* outIndex, void** outMesh)
{
    __try
    {
        uintptr_t matrixPtr = *reinterpret_cast<uintptr_t*>(reinterpret_cast<unsigned char*>(item) + 0x2C);
        unsigned char* batch = reinterpret_cast<unsigned char*>(matrixPtr) - 0x10;
        uint32_t count = *reinterpret_cast<uint32_t*>(batch + 0x7C);
        uint32_t originBits = *reinterpret_cast<uint32_t*>(batch + 0x60);
        if (count == 0 || count > 256 || originBits == 0 || !IsBodyBatch(batch))
            return false;
        uintptr_t* inst = *reinterpret_cast<uintptr_t**>(batch + 0x78);
        uintptr_t mesh = *reinterpret_cast<uintptr_t*>(item);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (inst[i * 2] == mesh)
            {
                *outIndex = static_cast<int>(i);
                *outMesh = reinterpret_cast<void*>(mesh);
                return true;
            }
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void __cdecl HookedSubmit(void* item, void* drawState)
{
    // With the gun on the controller, the game's arms and hands are left reaching for where the gun used to be: the
    // parts picked in the settings (HideBodyPart1..4) are skipped, except the arms while the IK has them on the
    // controllers (DriveArms).
    // Not in a scripted stretch (GetPlayerAimDriveWeight): the gun is in the game's hands then, so they're drawn.
    if (GetWeaponDriveEnabled() && GetPlayerAimDriveWeight() >= 1.0f &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) ==
            reinterpret_cast<uintptr_t>(Offset<void*>(OFFSET_DRAWITEMSUBMIT_CALLER_INSTANCELOOP)))
    {
        int idx = -1;
        void* mesh = nullptr;
        if (ResolveBodyInstance(item, &idx, &mesh) && GetHideBodyPart(idx) &&
            !(GetArmIkEnabled() && ShouldDrawArmMesh(reinterpret_cast<uintptr_t>(mesh))))
        {
            static std::atomic<int> hideLogs{0};
            if (hideLogs.fetch_add(1) < 8)
                MOHW_LOG(kLogFile, "HIDING body part %d (instance %d, mesh=%p)", idx + 1, idx, mesh);
            return;
        }
    }
    g_originalSubmit(item, drawState);
}

// Naked stub so EBX (the caller's batch pointer, param_2 of FUN_008B2AB0) can be
// read before the compiler touches it. Original ABI: __thiscall, ECX = this,
// one stack arg (the 4x4 matrix pointer), callee pops it (RET 4).
__declspec(naked) void Detour()
{
    __asm {
        mov eax, dword ptr [esp]
        push eax
        push dword ptr [esp + 8]
        push ebx
        push ecx
        call HookImpl
        ret 4
    }
}

} // namespace

bool InstallCameraMatrixTestHook()
{
    if (g_moduleBase == 0 || !g_buildVerified)
    {
        MOHW_LOG(kLogFile, "InstallCameraMatrixTestHook: module base not resolved / build not verified yet -- refusing");
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    g_hookAddress = Offset<void*>(OFFSET_CAMERAMATRIXSET);

    MH_STATUS s = MH_CreateHook(g_hookAddress, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(FUN_006FC2A0 @ %p) FAILED: %s", g_hookAddress, MH_StatusToString(s));
        g_hookAddress = nullptr;
        return false;
    }
    s = MH_EnableHook(g_hookAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook FAILED: %s", MH_StatusToString(s));
        return false;
    }

    g_submitAddress = Offset<void*>(OFFSET_DRAWITEMSUBMIT);
    s = MH_CreateHook(g_submitAddress, reinterpret_cast<void*>(&HookedSubmit), reinterpret_cast<void**>(&g_originalSubmit));
    if (s == MH_OK)
        s = MH_EnableHook(g_submitAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "submit hook (FUN_00723800 @ %p) FAILED: %s -- instance-hide test unavailable", g_submitAddress,
                   MH_StatusToString(s));
        g_submitAddress = nullptr;
    }
    else
    {
        MOHW_LOG(kLogFile, "instance-hide hook installed @ %p ('[' / ']' step: -1 off, 0..3 = hide that instance of the count==3 batch)",
                   g_submitAddress);
    }

    MOHW_LOG(kLogFile,
               "Camera matrix hook installed @ %p -- viewmodel caller ret=%p. Numpad . = weapon drive on (gun snapped to the "
               "right controller, arms hidden) / off",
               g_hookAddress, Offset<void*>(OFFSET_CAMERAMATRIX_CALLER_VIEWMODEL));
    return true;
}

void RemoveCameraMatrixTestHook()
{
    if (g_submitAddress)
    {
        MH_DisableHook(g_submitAddress);
        MH_RemoveHook(g_submitAddress);
        g_submitAddress = nullptr;
    }
    if (g_hookAddress)
    {
        MH_DisableHook(g_hookAddress);
        MH_RemoveHook(g_hookAddress);
        g_hookAddress = nullptr;
    }
}

void CheckWeaponDriveHotkey()
{
    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_DECIMAL) & 0x8000) != 0;
    if (down && !wasDown)
    {
        bool on = !GetWeaponDriveEnabled();
        SetWeaponDriveEnabled(on); // saved, so it's how the next character load starts too
        if (on)
        {
            g_weaponGripLogRequested = true;
            MOHW_LOG(kLogFile, "Numpad . -- weapon drive ON, gun held at its hand socket on the controller");
        }
        else
        {
            MOHW_LOG(kLogFile, "Numpad . -- weapon drive OFF (weapon back where the game puts it)");
        }
    }
    wasDown = down;
}

bool ConsumeViewmodelCamera(const void* camera)
{
    if (camera == nullptr || camera != t_pendingViewmodelCamera)
        return false;
    t_pendingViewmodelCamera = nullptr;
    uint32_t fovBits;
    memcpy(&fovBits, static_cast<const unsigned char*>(camera) + 0x48, sizeof(fovBits));
    return fovBits == t_pendingViewmodelFovBits;
}

int LastViewmodelEye()
{
    return t_viewmodelEye;
}

bool IsPlayerSkeletonLoaded()
{
    constexpr DWORD kRecentMs = 500;
    DWORD last = g_lastViewmodelMatrixTick.load(std::memory_order_relaxed);
    if (last == 0)
        return false;
    return (GetTickCount() - last) <= kRecentMs; // unsigned subtraction wraps correctly across GetTickCount() rollover
}

} // namespace mohw
