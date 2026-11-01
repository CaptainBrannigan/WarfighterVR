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
#include "first_person_rig.h"
#include "laser_raycast.h"
#include "../sdk/rigid.h"
#include "../openvr_direct/vr_hands.h"
#include "../openvr_direct/vr_reload.h"

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

// 0 = use the index selector; 1 = weapon batches only (IsBodyBatch false);
// 2 = body batch only. (Was count != 3 / == 3, confirmed 2026-09-19 for the
// first rifle and pistol; other levels broke it, see IsBodyBatch.)
std::atomic<int> g_roleMode{1}; // default: weapon only

using DrawItemSubmitFn = void(__cdecl*)(void* item, void* drawState);
DrawItemSubmitFn g_originalSubmit = nullptr;
void* g_submitAddress = nullptr;

// ---- Controller weapon drive (v3, 2026-09-26) ---------------------------------------------------------------
// Draws the weapon batches (count != 3) as if the first-person rig were rigidly held by the right controller.
// Numpad . toggles it; the gun snaps straight onto the controller at a fixed grip point (WeaponGripRight/Up/Back in
// the ini), wherever the hand happens to be when the key is pressed.
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

// The weapon hand's controller (the "right" slot), or the off hand's.
bool ControllerFrame(const float m[16], bool offHand, Rigid* out)
{
    mohwvr::ipc::ControllerPoseBlock controller{};
    mohwvr::ipc::HeadPoseBlock head{};
    if (!(offHand ? GetLeftControllerPose(&controller) : GetRightControllerPose(&controller)) || !GetHeadPose(&head))
        return false;

    Quat q{controller.orientationX, controller.orientationY, controller.orientationZ, controller.orientationW};
    for (int i = 0; i < 3; ++i)
    {
        Vec3 e{};
        e.x = i == 0 ? 1.0f : 0.0f;
        e.y = i == 1 ? 1.0f : 0.0f;
        e.z = i == 2 ? 1.0f : 0.0f;
        Vec3 axis = QuatRotateVector(q, e);
        if (!TrackingDirectionToGameWorld(axis.x, axis.y, axis.z, out->R[i]))
            return false;
    }

    float offset[3];
    if (!TrackingOffsetToGameWorld(controller.positionX - head.positionX, controller.positionY - head.positionY,
                                   controller.positionZ - head.positionZ, offset))
        return false;
    float eyeOffset = GetActiveEyeOffsetAlongRow0();
    for (int k = 0; k < 3; ++k)
        out->t[k] = m[12 + k] - m[k] * eyeOffset + offset[k];
    return true;
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
    GetWeaponGripOffset(j.t);
    if (mirrored)
        j.t[0] = -j.t[0];
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

// THE RIG'S OWN CAMERA (2026-11-01): the rig's bones are posed at the game's 30 Hz tick, against the game's own camera,
// but the camera it's drawn from is the mod's smoothed one, updated every frame. Head still, they agree; turning, the
// rig sits behind and then catches up each tick, which reads as jitter on everything attached to it, and only on head
// turns (the 2026-08 smoothing fixed the world, and left this). So what's "camera-relative" in the rig is measured from
// the game's camera: rotation only, as the two share a position bar the head offset. A big gap means it isn't this
// camera's frame (a cutscene, a transition): left alone.
constexpr float kRigCameraMaxDegrees = 25.0f;
std::atomic<float> g_rigGapDegrees{0.0f};
std::atomic<unsigned long long> g_rigGapLogMs{0};

void AlignRigCamera(Rigid* rigCamera)
{
    float rows[3][3];
    if (!GetStabilizeRig() || !GetGameCameraRows(rows))
        return;
    float back = 0.0f, up = 0.0f;
    for (int k = 0; k < 3; ++k)
    {
        back += rows[2][k] * rigCamera->R[2][k];
        up += rows[1][k] * rigCamera->R[1][k];
    }
    const float lowest = back < up ? back : up;
    const float gap = acosf(lowest > 1.0f ? 1.0f : lowest < -1.0f ? -1.0f : lowest) * 57.29578f;
    if (gap > g_rigGapDegrees.load(std::memory_order_relaxed))
        g_rigGapDegrees.store(gap, std::memory_order_relaxed);
    unsigned long long now = GetTickCount64(), due = g_rigGapLogMs.load(std::memory_order_relaxed);
    if (now >= due && g_rigGapLogMs.compare_exchange_strong(due, now + 2000))
        MOHW_LOG(kLogFile, "rig camera: largest gap to the drawn camera %.2f deg in the last 2 s%s",
                 g_rigGapDegrees.exchange(0.0f), gap > kRigCameraMaxDegrees ? " (over the limit: not used)" : "");
    if (gap > kRigCameraMaxDegrees)
        return;
    memcpy(rigCamera->R, rows, sizeof(rows));
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
    const float up[3] = {0.0f, 1.0f, 0.0f};
    float offset[3];
    if (!TrackingDirectionToGameWorld(view.bodyRight[0], view.bodyRight[1], view.bodyRight[2], out->R[0]) ||
        !TrackingDirectionToGameWorld(up[0], up[1], up[2], out->R[1]) ||
        !TrackingDirectionToGameWorld(view.bodyBack[0], view.bodyBack[1], view.bodyBack[2], out->R[2]) ||
        !TrackingOffsetToGameWorld(view.pouchFromHead[0], view.pouchFromHead[1], view.pouchFromHead[2], offset))
        return false;
    float eyeOffset = GetActiveEyeOffsetAlongRow0();
    for (int k = 0; k < 3; ++k)
        out->t[k] = m[12 + k] - m[k] * eyeOffset + offset[k];
    return true;
}

float BlendStep(float from, float to, float x)
{
    float t = (x - from) / (to - from);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

float Length3(const float v[3])
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// MANUAL RELOAD (2026-10-31, openvr_direct/vr_reload.h): a magazine part's batch, drawn where the magazine is now. Any
// part drawn with M' = M * W^-1 shows at P * W, so W is "the move this part takes". Rest = the gun's move, as the weapon
// drive. Held = the bone's point on the off hand's controller, turned with it as the gun turns with its own.
// In range of the seat (the weapon's MagazineProfile, 2026-11-01), the magazine stays where the hand has it and only
// turns: its top, the line from the seat to the aim point at rest, turns to point at the aim point from wherever it is,
// by alpha, 0 at the range's edge and 1 at the lock range. There the point is on that line and the turn is the rest
// pose. At alpha 1 the horizontal locks: it can only move along the line, and not past the seat. kLockEaseMetres eases
// the last stretch in, so reaching the lock doesn't jump. False = draw it with the gun.
constexpr float kLockEaseMetres = 0.03f;
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
    const Rigid toController = Compose(camInverse, Inverse(GripInCamera()));
    const Rigid rest = Compose(toController, c);
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
    Rigid holder{};
    const bool held = view.state == openvr_direct::MagazineState::Held;
    if (!(held ? ControllerFrame(m, true, &holder) : PouchFrame(m, view, &holder)))
        return false;
    Rigid grip = RigidIdentity();
    for (int k = 0; k < 3; ++k)
        grip.t[k] = inCamera[k];
    Rigid w = Compose(Compose(camInverse, Inverse(grip)), holder);
    if (held)
    {
        float atRest[3], atHand[3], aimPoint[3], fromSeat[3], axis[3];
        PointTimes(bone, rest, atRest);
        PointTimes(bone, w, atHand);
        PointTimes(aimInCamera, Compose(gripInverse, c), aimPoint);
        for (int k = 0; k < 3; ++k)
        {
            fromSeat[k] = atHand[k] - atRest[k];
            axis[k] = aimPoint[k] - atRest[k];
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
                position[k] = atHand[k] + (atRest[k] + axis[k] * along - atHand[k]) * lock;
                toAim[k] = aimPoint[k] - position[k];
            }
            const float toAimLength = Length3(toAim);
            Rigid pointed = rest;
            if (toAimLength > 1e-3f)
            {
                for (int k = 0; k < 3; ++k)
                    toAim[k] /= toAimLength;
                if (RotationBetween(axis, toAim, turn))
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            pointed.R[i][j] =
                                rest.R[i][0] * turn[0][j] + rest.R[i][1] * turn[1][j] + rest.R[i][2] * turn[2][j];
            }
            Rigid blended{};
            BlendRotation(w.R, pointed.R, alpha, blended.R);
            float turned[3];
            PointTimes(bone, blended, turned);
            for (int k = 0; k < 3; ++k)
                blended.t[k] = position[k] - turned[k];
            w = blended;
        }
    }
    ToMatrix(Compose(cam, Inverse(w)), m);
    return true;
}

// ARMS (2026-10-31): the body batch's arms onto the controllers, hooks/first_person_rig.h. The weapon hand goes with the
// gun, so it needs no calibration of its own: wherever the gun's grip offset puts the gun, the game's hand is on it.
void DriveArms(const unsigned char* batch, const float original[16], const float drawn[16], const Rigid& rigCamera)
{
    if (!GetArmIkEnabled() || !GetWeaponDriveEnabled() || !IsPlayerAimActive())
        return;
    Rigid c{}, off{};
    if (!ControllerFrame(original, false, &c))
        return;
    const bool haveOff = ControllerFrame(original, true, &off);
    const Rigid gun = Compose(Compose(Inverse(rigCamera), Inverse(GripInCamera())), c);
    ApplyArmIk(batch, FromMatrix(original), rigCamera, FromMatrix(drawn), gun, off, haveOff,
               openvr_direct::IsWeaponTwoHanded(), openvr_direct::IsWeaponHandLeft());
}

// THE BODY (2026-11-01): moves the body batch (legs, torso, arms) as a whole. Shown from M' it appears at P * M'^-1 * M;
// to take a further move B there it's drawn from M * B^-1 * M^-1 * M'. B: the BodyOffset settings along the level body
// frame, and with BodyUpright the camera's tilt taken out, about the camera. The arms' IK works through the camera drawn
// here, so the hands stay on the controllers and only the shoulders move.
void MoveBody(float drawn[16], const float original[16])
{
    float offset[3];
    GetBodyOffset(offset);
    const bool upright = GetBodyUpright();
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
    PointTimes(cam.t, b, turned);
    for (int k = 0; k < 3; ++k)
        b.t[k] = cam.t[k] - turned[k] + right[k] / rightLength * offset[0] + (k == 1 ? offset[1] : 0.0f) +
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
    uintptr_t expectedRet = reinterpret_cast<uintptr_t>(Offset<void*>(OFFSET_CAMERAMATRIX_CALLER_VIEWMODEL));
    if (ret != expectedRet || matrix == nullptr || batch == nullptr)
    {
        g_original(thisPtr, matrix);
        return;
    }

    g_lastViewmodelMatrixTick.store(GetTickCount(), std::memory_order_relaxed);
    PollHotkeys();
    t_pendingViewmodelCamera = thisPtr;
    memcpy(&t_pendingViewmodelFovBits, static_cast<const unsigned char*>(thisPtr) + 0x48, sizeof(uint32_t));

    const unsigned char* b = reinterpret_cast<const unsigned char*>(batch);
    uint32_t count = *reinterpret_cast<const uint32_t*>(b + 0x7C);
    const float* transl = reinterpret_cast<const float*>(b + 0x40);
    bool isBody = IsBodyBatch(b);

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
    float m[16];
    memcpy(m, matrix, sizeof(m));
    // Weapon batches follow the controller when the drive is on (parallax included). Everything else in the rig is
    // anchored to the head centre: the body+hands batch without the head's roll, the weapon with the drive off with
    // it (it's the view's gun then, and should roll with the view).
    Rigid rigCamera = FromMatrix(m);
    AlignRigCamera(&rigCamera);
    bool driven = false;
    if (patchThis)
    {
        uintptr_t mesh0 = 0, second0 = 0, vtable0 = 0;
        if (!isBody && ReadInstance(b, 0, &mesh0, &second0, &vtable0) && IsMagazineMesh(mesh0))
            driven = DriveMagazine(m, rigCamera);
        driven = driven || DriveWeaponFromController(m, rigCamera);
    }
    if (!driven)
        AnchorRigToHead(m, rigCamera, isBody);
    if (isBody)
    {
        NoteBodyBatch(b);
        MoveBody(m, matrix);
        DriveArms(b, matrix, m, rigCamera);
    }

    ++g_patched;
    int lastLogged = g_lastLoggedSelected.load();
    if (lastLogged != sel && g_lastLoggedSelected.compare_exchange_strong(lastLogged, sel))
        MOHW_LOG(kLogFile, "PATCHING selector=%d: first hit seq=%d batch=%p count=%u transl=(%.3f,%.3f,%.3f) camera=%p", sel,
                   t_seq, batch, count, transl[0], transl[1], transl[2], thisPtr);

    g_original(thisPtr, m);
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
    if (GetWeaponDriveEnabled() &&
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
            float grip[3];
            GetWeaponGripOffset(grip);
            g_weaponGripLogRequested = true;
            MOHW_LOG(kLogFile, "Numpad . -- weapon drive ON, gun snapped to the controller at grip (%.3f, %.3f, %.3f)",
                     grip[0], grip[1], grip[2]);
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

bool IsPlayerSkeletonLoaded()
{
    constexpr DWORD kRecentMs = 500;
    DWORD last = g_lastViewmodelMatrixTick.load(std::memory_order_relaxed);
    if (last == 0)
        return false;
    return (GetTickCount() - last) <= kRecentMs; // unsigned subtraction wraps correctly across GetTickCount() rollover
}

} // namespace mohw
