#include "vr_reload.h"
#include "../hooks/boat_seat.h"
#include "vr_magazine_path.h"

#include "../hooks/aiming_controller_hook.h"
#include "../hooks/first_person_rig.h"
#include "../hooks/weapon_identity.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace mohw::openvr_direct {
namespace {

constexpr const char* kLogFile = "mohwvr_reload.log";

// ClientSoldierWeapon -> its predicted WeaponFiring (FUN_00A23B50), and in that: the state (+0xB0), the state's timer
// (+0xC4, FUN_0083C910 waits on it), the ammo table (+0x134, 0x10 per fire mode: +0 in the magazine, +4 in reserve,
// FUN_00830AC0) and the fire mode (+0x144). The reload (FUN_0083B130): 0xB waits part of the reload time, 0xC adds the
// ammo (FUN_008303C0) and waits the rest. On a weapon that reloads a round at a time (FUN_008303C0 adds one) 0xC goes
// back to 0xB until the magazine is full (FUN_00830260). (2026-11-01: this first cut 0xD / 0xE short, which aren't the
// reload.)
constexpr uint32_t kWeaponFiring = 0x44C;
constexpr uint32_t kFiringData = 0x94;
constexpr uint32_t kFiringState = 0xB0;
constexpr uint32_t kFiringTimer = 0xC4;
constexpr uint32_t kFiringAmmoTable = 0x134;
constexpr uint32_t kFiringMode = 0x144;
constexpr int kStateReloadFirst = 0xB, kStateReloadLast = 0xC;

constexpr unsigned long long kMagazineSeenMs = 1000;   // no magazine drawn for this long: this weapon has none
constexpr unsigned long long kReloadKeyHoldMs = 120;
constexpr unsigned long long kReloadStartMs = 1500;    // how long our Reload press has to show up as a reload
constexpr unsigned long long kAfterSeatMs = 6000;      // after a seat, a reload isn't the magazine leaving, unless empty
// A magazine that just left the path has to get this much further than the lock range from its entry before it can
// engage again (hysteresis: it leaves at the entry).
constexpr float kLeaveMargin = 0.04f;

struct Firing
{
    int state = 0, loaded = 0, reserve = 0;
    uintptr_t object = 0, data = 0;
};

// SEH-safe.
bool ReadFiring(Firing* out)
{
    uintptr_t weapon = GetEquippedSoldierWeapon();
    if (!weapon)
        return false;
    __try
    {
        uintptr_t firing = *reinterpret_cast<const uintptr_t*>(weapon + kWeaponFiring);
        if (!firing)
            return false;
        out->object = firing;
        out->data = *reinterpret_cast<const uintptr_t*>(firing + kFiringData);
        out->state = *reinterpret_cast<const int*>(firing + kFiringState);
        const char* table = *reinterpret_cast<const char* const*>(firing + kFiringAmmoTable);
        const int mode = *reinterpret_cast<const int*>(firing + kFiringMode);
        out->loaded = *reinterpret_cast<const int*>(table + mode * 0x10);
        out->reserve = *reinterpret_cast<const int*>(table + mode * 0x10 + 4);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void EndStateNow(uintptr_t firing)
{
    __try
    {
        *reinterpret_cast<float*>(firing + kFiringTimer) = 0.0f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// The firing's reload now, if it's in one: an empty magazine's reload waits longer, with no reload time of its own.
void EndReloadNow(uintptr_t firing)
{
    __try
    {
        const int state = *reinterpret_cast<const int*>(firing + kFiringState);
        if (state >= kStateReloadFirst && state <= kStateReloadLast)
            *reinterpret_cast<float*>(firing + kFiringTimer) = 0.0f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// The state, written on the submit thread; the render threads read the view, published whole (PublishView) at the end
// of each submit-thread entry point, so a frame never sees a state with another state's path or hand.
std::atomic<int> g_state{static_cast<int>(MagazineState::InGun)};
std::mutex g_viewMutex;
MagazineView g_view; // guarded by g_viewMutex

std::mutex g_geometryMutex;
MagazineGeometry g_geometry{}; // guarded by g_geometryMutex
std::atomic<unsigned long long> g_pointTick{0};

// Submit thread.
bool g_heldIsNew = false;      // what the hand holds is a fresh magazine, not the one just pulled out
bool g_leftTheWell = false;    // the held magazine has been away from the gun (a held one only engages after that)
unsigned g_weaponGeneration = 0;
unsigned long long g_reloadKeyUpAt = 0;
// Our own reload, from the seat until it's over: however long a round-at-a-time reload takes. (live 2026-11-01: a fixed
// 1.5 s window ran out 24 rounds in, and the rest of it was taken for the game's own, dropping the new magazine.)
bool g_ourReload = false;
bool g_ourReloadStarted = false;
unsigned long long g_ourReloadStartBy = 0;
unsigned long long g_seatedAt = 0; // a new magazine's seat
bool g_wasReloading = false;
uintptr_t g_otherFiringLogged = 0;
// On the path, coming out (2026-11-14; going in left the path 2026-11-18).
float g_s = 0.0f;              // 0 seated .. 1 clear
float g_sOffset = 0.0f;        // the projection's offset at the start, so s doesn't jump
bool g_released = false;       // coming out: the magazine release has been pressed during this grip
bool g_movedOut = false;       // coming out: it has left the seat (back at it, it clicks in again)
Rigid g_handInMag = RigidIdentity();
// HELD (2026-11-18, the user: "attach the magazine with the offset of where we grabbed it at, rather than snapping it
// down into the hand"): the magazine in the hand that holds it, mag-local -> that controller's frame, taken at the grab
// -- from the pouch, or where the hand had it on the gun when it came free.
Rigid g_magInHand = RigidIdentity();
Vec3 g_bodyRight{1.0f, 0.0f, 0.0f}, g_bodyForward{0.0f, 0.0f, -1.0f}; // the latest UpdateManualReload's, for the pouch
bool g_releaseDown = false, g_releasePressed = false; // SetMagazineReleaseInput
int g_lastWeaponHand = kRightHand;
// The next glide, for PublishView.
bool g_blendPending = false;
MagazineView g_blend;

const char* StateName(MagazineState state)
{
    switch (state)
    {
    case MagazineState::InGun: return "in the gun";
    case MagazineState::Held: return "in the hand";
    case MagazineState::Out: return "at the pouch";
    case MagazineState::OnPath: return "gripped on the gun";
    }
    return "?";
}

void SetState(MagazineState state, const char* why)
{
    g_state.store(static_cast<int>(state), std::memory_order_relaxed);
    MOHW_LOG(kLogFile, "magazine: %s (%s)", StateName(state), why);
}

MagazineState State()
{
    return static_cast<MagazineState>(g_state.load(std::memory_order_relaxed));
}

// The hand and the magazine glide from where they are now (call before the state's path or hand changes).
void StartBlend(bool hand, bool mag, const MagazinePath& path)
{
    const Rigid d = PathDisplacement(path, g_s);
    g_blend.blendStartMs = GetTickCount64();
    g_blend.blendHand = hand;
    g_blend.blendMag = mag;
    g_blend.blendHandFromGun = Compose(g_handInMag, d);
    g_blend.blendMagFromGun = d;
    g_blendPending = true;
}

void PublishView()
{
    std::lock_guard<std::mutex> lock(g_viewMutex);
    g_view.state = State();
    g_view.s = g_s;
    g_view.handInMag = g_handInMag;
    g_view.magInHand = g_magInHand;
    if (g_blendPending)
    {
        g_view.blendStartMs = g_blend.blendStartMs;
        g_view.blendHand = g_blend.blendHand;
        g_view.blendMag = g_blend.blendMag;
        g_view.blendHandFromGun = g_blend.blendHandFromGun;
        g_view.blendMagFromGun = g_blend.blendMagFromGun;
        g_blendPending = false;
    }
}

struct PublishOnExit
{
    ~PublishOnExit() { PublishView(); }
};

bool Available()
{
    if (!GetManualReloadEnabled() || !GetWeaponDriveEnabled() || !IsPlayerAimActive() || GetBoatSeat() != BoatSeat::None)
        return false;
    return GetTickCount64() - g_pointTick.load(std::memory_order_relaxed) < kMagazineSeenMs;
}

Vec3 Position(const HandInput& hand)
{
    Vec3 p{};
    p.x = hand.pose[0][3];
    p.y = hand.pose[1][3];
    p.z = hand.pose[2][3];
    return p;
}

// A hand's pose as a frame (rows: its right / up / back in tracking space, t: its position).
Rigid FrameOf(const HandInput& hand)
{
    Rigid r{};
    for (int i = 0; i < 3; ++i)
    {
        for (int k = 0; k < 3; ++k)
            r.R[i][k] = hand.pose[k][i];
        r.t[i] = hand.pose[i][3];
    }
    return r;
}

// A vector in the weapon controller's frame, tracking space.
Vec3 ControllerDirection(const HandInput& weapon, const float l[3])
{
    Vec3 v{};
    for (int c = 0; c < 3; ++c)
    {
        v.x += weapon.pose[0][c] * l[c];
        v.y += weapon.pose[1][c] * l[c];
        v.z += weapon.pose[2][c] * l[c];
    }
    return v;
}

// The magazine's seat, aim point and the gun's axes, tracking space.
struct SeatNowResult
{
    Vec3 seat, aim, axes[3];
};
SeatNowResult SeatNow(const HandInput& weapon)
{
    MagazineGeometry g;
    {
        std::lock_guard<std::mutex> lock(g_geometryMutex);
        g = g_geometry;
    }
    SeatNowResult s{};
    s.seat = VecAdd(Position(weapon), ControllerDirection(weapon, g.seat));
    s.aim = VecAdd(Position(weapon), ControllerDirection(weapon, g.aim));
    for (int i = 0; i < 3; ++i)
        s.axes[i] = ControllerDirection(weapon, g.axes[i]);
    return s;
}

// Gun-local (vr_magazine_path.h) -> tracking: the gun's frame at the magazine's seat.
Rigid SeatFrame(const HandInput& weapon)
{
    const SeatNowResult s = SeatNow(weapon);
    Rigid r{};
    for (int i = 0; i < 3; ++i)
    {
        r.R[i][0] = s.axes[i].x;
        r.R[i][1] = s.axes[i].y;
        r.R[i][2] = s.axes[i].z;
    }
    r.t[0] = s.seat.x;
    r.t[1] = s.seat.y;
    r.t[2] = s.seat.z;
    return r;
}

Vec3 ToVec(const float p[3])
{
    Vec3 v{};
    v.x = p[0];
    v.y = p[1];
    v.z = p[2];
    return v;
}

float Distance(const Vec3& a, const Vec3& b)
{
    Vec3 d = VecSub(a, b);
    return sqrtf(VecDot(d, d));
}

float Distance3(const float a[3], const float b[3])
{
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// The hand in the magazine's grab box (MagazineProfile), the box's axes being the gun's.
bool InGrabBox(const HandInput& weapon, const HandInput& off)
{
    const MagazineProfile p = GetMagazineProfile();
    const SeatNowResult s = SeatNow(weapon);
    const Vec3 toHand = VecSub(Position(off), s.seat);
    const float offset[3] = {p.grabOffset[0], p.grabOffset[1], -p.grabOffset[2]}; // right, up, back
    for (int i = 0; i < 3; ++i)
        if (fabsf(VecDot(toHand, s.axes[i]) - offset[i]) > p.grabHalfSize[i])
            return false;
    return true;
}

// The held magazine: mag-local -> tracking, where the grab put it in the off hand.
Rigid HeldMagazine(const HandInput& off)
{
    return Compose(g_magInHand, FrameOf(off));
}

// The magazine waiting in the pouch, mag-local -> tracking: the body's axes at the pouch's centre, turned by the pouch
// rotation (as the drawing's, hooks/camera_matrix_test_hook.cpp PouchFrame).
Rigid PouchMagazineNow(const Vec3& pouchCentre)
{
    Rigid frame{};
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 back = VecScale(g_bodyForward, -1.0f);
    const Vec3* rows[3] = {&g_bodyRight, &up, &back};
    for (int i = 0; i < 3; ++i)
    {
        frame.R[i][0] = rows[i]->x;
        frame.R[i][1] = rows[i]->y;
        frame.R[i][2] = rows[i]->z;
    }
    frame.t[0] = pouchCentre.x;
    frame.t[1] = pouchCentre.y;
    frame.t[2] = pouchCentre.z;
    return PouchMagazine(frame);
}

// How far along the seat -> aim line a point is (below the seat is negative).
float AlongAxis(const SeatNowResult& s, const Vec3& point)
{
    Vec3 axis = VecSub(s.aim, s.seat);
    const float length = sqrtf(VecDot(axis, axis));
    if (length < 1e-3f)
        return 0.0f;
    return VecDot(VecSub(point, s.seat), VecScale(axis, 1.0f / length));
}

void Seat()
{
    SetState(MagazineState::InGun, g_heldIsNew ? "seated, new" : "seated, the same one");
    if (!g_heldIsNew)
        return;
    Firing f{};
    const bool haveFiring = ReadFiring(&f);
    if (haveFiring)
        MOHW_LOG(kLogFile, "ammo at the seat: %d in the magazine, %d in reserve", f.loaded, f.reserve);
    SetInstantReload(haveFiring ? f.object : 0, haveFiring ? f.data : 0);
    int dik = SetGameActionKey("ConceptReload", true);
    if (dik < 0)
    {
        MOHW_LOG(kLogFile, "ConceptReload has no keyboard key in the game's bindings: no ammo added");
        return;
    }
    unsigned long long now = GetTickCount64();
    g_reloadKeyUpAt = now + kReloadKeyHoldMs;
    g_seatedAt = now;
    g_ourReload = true;
    g_ourReloadStarted = false;
    g_ourReloadStartBy = now + kReloadStartMs;
}

// The magazine has come free in the hand (off the end of the path, or the grip pulled it away): it stays where the hand
// had it on the gun -- g_handInMag is the hand in the magazine's frame, so the magazine in the hand is its inverse.
void FreeInHand(const char* why)
{
    g_magInHand = Inverse(g_handInMag);
    g_leftTheWell = false;
    g_heldIsNew = false;
    SetState(MagazineState::Held, why);
}

// ON THE PATH, COMING OUT: the controller's pull, projected onto the path, moves the magazine once it's more than the
// platform's pull force ahead or behind (a dead band: the magazine follows that far behind the hand). The hand is drawn
// stuck to the magazine (handInMag); it lets go past MagazineGripBreak.
void UpdateOnPath(const HandInput& weapon, const HandInput& off, bool releasePressed)
{
    const MagazinePath path = CurrentMagazinePath();
    const MagazinePlatform plat = GetMagazinePlatform();
    const Rigid seat = SeatFrame(weapon);
    const Rigid offFrame = FrameOf(off);
    float q[3]; // the controller, gun-local
    PointTimes(offFrame.t, Inverse(seat), q);
    const float* grab = g_handInMag.t;
    const float band = plat.pullForce / PathLength(path);
    if (releasePressed && !g_released)
    {
        g_released = true;
        MOHW_LOG(kLogFile, "magazine release pressed: it can come out now");
    }
    const bool locked = plat.needsRelease && !g_released;
    const float target = ProjectOnPath(path, grab, q) - g_sOffset;
    if (!locked)
    {
        if (target > g_s + band)
            g_s = target - band;
        else if (target < g_s - band)
            g_s = target + band;
    }
    if (g_s < 0.0f)
        g_s = 0.0f;

    if (g_s >= 1.0f)
    {
        // Clear of the magwell: free in the hand, which comes back to its controller.
        g_s = 1.0f;
        StartBlend(true, true, path);
        FreeInHand("out of the magwell");
        return;
    }
    if (g_s > 0.2f)
        g_movedOut = true;
    if (g_movedOut && g_s <= 0.02f)
    {
        // Pushed back home: it clicks in again, and needs the release again.
        g_movedOut = false;
        g_released = !plat.needsRelease;
        g_s = 0.0f;
        MOHW_LOG(kLogFile, "magazine pushed back in: it clicks into place");
    }
    // The grip lets go once the controller is too far from the hand on the magazine.
    float stuck[3];
    PointTimes(grab, Compose(PathDisplacement(path, g_s), seat), stuck);
    if (Distance3(offFrame.t, stuck) > GetMagazineGripBreak())
    {
        const bool comesAlong = g_s >= 0.5f;
        StartBlend(true, comesAlong, path);
        if (comesAlong)
        {
            FreeInHand("the hand pulled away: the magazine comes with it");
        }
        else
        {
            g_s = 0.0f;
            SetState(MagazineState::InGun, "the hand pulled away: the magazine stays in");
        }
    }
}

// HELD, GOING IN (2026-11-18: back to the first version, which the user found "very smooth"; the path's engage-and-push
// with its pull force is gone): once it's been away from the gun, the magazine's insertion point (its Wep_Mag point,
// where the grab put it in the hand) within the weapon profile's lock range of the seat, and no more than "seats
// within" below the seat along the magwell's line, seats it. The drawing turns it onto that line from the profile's range
// and holds it on the line inside the lock range (hooks/camera_matrix_test_hook.cpp DriveMagazine).
void UpdateHeld(const HandInput& weapon, const HandInput& off)
{
    const MagazineProfile prof = GetMagazineProfile();
    const SeatNowResult s = SeatNow(weapon);
    const Vec3 point = ToVec(HeldMagazine(off).t);
    const float d = Distance(point, s.seat);
    if (d > prof.lockRange)
        g_leftTheWell = true;
    if (g_leftTheWell && d <= prof.lockRange && AlongAxis(s, point) >= -prof.seatRange)
        Seat();
}

} // namespace

MagazineView GetMagazineView()
{
    std::lock_guard<std::mutex> lock(g_viewMutex);
    return g_view;
}

void PublishMagazineGeometry(const MagazineGeometry& geometry)
{
    {
        std::lock_guard<std::mutex> lock(g_geometryMutex);
        g_geometry = geometry;
    }
    g_pointTick.store(GetTickCount64(), std::memory_order_relaxed);
}

bool GetMagazineGeometry(MagazineGeometry* out)
{
    if (GetTickCount64() - g_pointTick.load(std::memory_order_relaxed) >= kMagazineSeenMs)
        return false;
    std::lock_guard<std::mutex> lock(g_geometryMutex);
    *out = g_geometry;
    return true;
}

void UpdateManualReload(const HandInput hands[2], int weaponHand, bool haveHead, const float headPos[3],
                        const Vec3& pouchCentre, const Vec3& bodyRight, const Vec3& bodyForward)
{
    PublishOnExit publish;
    unsigned long long now = GetTickCount64();
    if (g_reloadKeyUpAt != 0 && now >= g_reloadKeyUpAt)
    {
        SetGameActionKey("ConceptReload", false);
        g_reloadKeyUpAt = 0;
    }
    if (haveHead)
    {
        g_bodyRight = bodyRight;
        g_bodyForward = bodyForward;
        std::lock_guard<std::mutex> lock(g_viewMutex);
        g_view.pouchFromHead[0] = pouchCentre.x - headPos[0];
        g_view.pouchFromHead[1] = pouchCentre.y - headPos[1];
        g_view.pouchFromHead[2] = pouchCentre.z - headPos[2];
        g_view.bodyRight[0] = bodyRight.x;
        g_view.bodyRight[1] = bodyRight.y;
        g_view.bodyRight[2] = bodyRight.z;
        g_view.bodyBack[0] = -bodyForward.x;
        g_view.bodyBack[1] = -bodyForward.y;
        g_view.bodyBack[2] = -bodyForward.z;
    }
    const bool releasePressed = g_releasePressed;
    g_releasePressed = false;

    MagazineState state = State();
    unsigned generation = GetEquippedWeaponGeneration();
    bool available = Available();
    if ((generation != g_weaponGeneration || !available) && state != MagazineState::InGun)
        SetState(MagazineState::InGun, generation != g_weaponGeneration ? "weapon changed" : "manual reload off for now");
    if (weaponHand != g_lastWeaponHand && State() != MagazineState::InGun)
        SetState(MagazineState::InGun, "the weapon changed hands");
    g_weaponGeneration = generation;
    g_lastWeaponHand = weaponHand;
    Firing firing{};
    const bool haveFiring = available && ReadFiring(&firing);
    SetInstantReload(haveFiring ? firing.object : 0, haveFiring && now < g_ourReloadStartBy ? firing.data : 0);
    const uintptr_t otherFiring = GetOtherInstantReloadFiring();
    if (otherFiring)
    {
        EndReloadNow(otherFiring);
        if (otherFiring != g_otherFiringLogged)
            MOHW_LOG(kLogFile, "the server's firing %p takes the reload too, %llu ms after the seat",
                     reinterpret_cast<void*>(otherFiring), now - g_seatedAt);
    }
    g_otherFiringLogged = otherFiring;
    if (!available)
        return;
    state = State();

    // Any reload in the game's state machine ends at once: that's the animation cancelled. One we didn't ask for means
    // the magazine is gone (the Reload button, or the auto-reload when empty).
    const Firing& f = firing;
    const bool reloading = haveFiring && f.state >= kStateReloadFirst && f.state <= kStateReloadLast;
    if (reloading)
    {
        if (g_ourReload)
        {
            g_ourReloadStarted = true;
        }
        else if (state == MagazineState::InGun)
        {
            // A reload showed up 1.5-2.6 s after the seat with no input (live 2026-11-01), while the server's copy of the
            // weapon was on its full-length one, the HUD's ammo changing a second later: not the magazine going anywhere.
            if (!g_wasReloading)
                MOHW_LOG(kLogFile, "reload not ours: %d in the magazine, %d in reserve, %llu ms after the seat", f.loaded,
                         f.reserve, now - g_seatedAt);
            if (now - g_seatedAt >= kAfterSeatMs || f.loaded <= 0)
                SetState(MagazineState::Out, "the game started a reload");
        }
        EndStateNow(f.object);
    }
    else if (g_ourReload && (g_ourReloadStarted || now >= g_ourReloadStartBy))
    {
        // Over, or it never began (the magazine was already full).
        g_ourReload = false;
    }
    g_wasReloading = reloading;

    const HandInput& weapon = hands[weaponHand];
    const HandInput& off = hands[1 - weaponHand];
    if (!weapon.tracked || !off.tracked)
        return;
    state = State();
    if (state == MagazineState::OnPath)
        UpdateOnPath(weapon, off, releasePressed);
    else if (state == MagazineState::Held)
        UpdateHeld(weapon, off);
}

bool ManualReloadGripPressed(const HandInput& weapon, const HandInput& off, const Vec3& pouchCentre, bool haveHead)
{
    if (!Available() || !weapon.tracked || !off.tracked)
        return false;
    PublishOnExit publish;
    MagazineState state = State();
    if (state == MagazineState::InGun && InGrabBox(weapon, off))
    {
        // The hand stays where it grabbed, on the magazine, which stays seated until it's released and pulled.
        const MagazinePath path = CurrentMagazinePath();
        const bool needsRelease = GetMagazinePlatform().needsRelease;
        const Rigid seat = SeatFrame(weapon);
        const Rigid offFrame = FrameOf(off);
        g_s = 0.0f;
        g_movedOut = false;
        g_released = !needsRelease;
        g_releasePressed = false; // a press from before the grip doesn't count
        g_heldIsNew = false;
        g_leftTheWell = false;
        g_handInMag = Compose(offFrame, Inverse(seat));
        float q[3];
        PointTimes(offFrame.t, Inverse(seat), q);
        g_sOffset = ProjectOnPath(path, g_handInMag.t, q);
        SetState(MagazineState::OnPath,
                 needsRelease ? "gripped in the gun: press the magazine release, then pull" : "gripped in the gun");
        return true;
    }
    if (state == MagazineState::Out)
    {
        // A grip anywhere else stays with the holsters and the two-handed grab.
        if (haveHead && Distance(Position(off), pouchCentre) <= GetMagazinePouchRadius())
        {
            // Where it sat in the pouch relative to the hand (2026-11-18), or snapped into the hand at the platform's
            // hold, with or without the grip's height on the magazine (MagazinePouchGrab, 2026-11-21).
            const Rigid grabbed = Compose(PouchMagazineNow(pouchCentre), Inverse(FrameOf(off)));
            g_magInHand = PouchGrabInHand(grabbed, GetMagazinePlatform(), g_lastWeaponHand == kLeftHand);
            const int mode = GetMagazinePouchGrab();
            MOHW_LOG(kLogFile,
                     "pouch grab (%s): the hand %.3f m up the magazine at the grip, %.3f m now; its top at (%.3f, %.3f, "
                     "%.3f) m in the hand (right, up, back)",
                     mode == kPouchGrabAsGrabbed ? "as grabbed"
                     : mode == kPouchGrabSnapKeepVertical ? "snapped, vertical from the grip"
                                                          : "snapped",
                     Inverse(grabbed).t[1], Inverse(g_magInHand).t[1], g_magInHand.t[0], g_magInHand.t[1], g_magInHand.t[2]);
            g_heldIsNew = true;
            g_leftTheWell = true;
            SetState(MagazineState::Held, "taken from the pouch");
            return true;
        }
    }
    return false;
}

void ManualReloadGripReleased(const HandInput& weapon, const HandInput& off)
{
    (void)weapon;
    (void)off;
    PublishOnExit publish;
    const MagazineState state = State();
    if (state == MagazineState::Held)
    {
        SetState(MagazineState::Out, "let go");
        return;
    }
    if (state != MagazineState::OnPath)
        return;
    const MagazinePath path = CurrentMagazinePath();
    StartBlend(true, false, path);
    if (g_s < 0.5f)
    {
        g_s = 0.0f;
        SetState(MagazineState::InGun, "let go: it stays in");
    }
    else
    {
        SetState(MagazineState::Out, "let go half out: it falls out");
    }
}

int GetMagazineGripHand()
{
    const MagazineState state = State();
    return state == MagazineState::OnPath || state == MagazineState::Held ? 1 - g_lastWeaponHand : -1;
}

void SetMagazineReleaseInput(bool down)
{
    if (down && !g_releaseDown)
        g_releasePressed = true;
    g_releaseDown = down;
}

bool GetMagazineDebug(const HandInput& weapon, const HandInput& off, MagazineDebug* out)
{
    if (!Available() || !weapon.tracked)
        return false;
    const MagazineProfile p = GetMagazineProfile();
    const SeatNowResult s = SeatNow(weapon);
    out->seat = s.seat;
    out->aim = s.aim;
    out->boxCentre = s.seat;
    const float offset[3] = {p.grabOffset[0], p.grabOffset[1], -p.grabOffset[2]}; // right, up, back
    for (int i = 0; i < 3; ++i)
    {
        out->axes[i] = s.axes[i];
        out->boxHalf[i] = p.grabHalfSize[i];
        out->boxCentre = VecAdd(out->boxCentre, VecScale(s.axes[i], offset[i]));
    }
    const MagazinePath path = CurrentMagazinePath();
    const Rigid seat = SeatFrame(weapon);
    out->entry = ToVec(Compose(PathDisplacement(path, 1.0f), seat).t);
    const MagazineState state = State();
    if (state == MagazineState::Held && off.tracked)
        out->insertion = ToVec(HeldMagazine(off).t);
    else if (state == MagazineState::OnPath)
        out->insertion = ToVec(Compose(PathDisplacement(path, g_s), seat).t);
    else
        out->insertion = s.seat;
    out->range = p.range;
    out->lockRange = p.lockRange;
    out->handDistance = Distance(out->insertion, s.seat); // going in is judged at the seat again (2026-11-18)
    out->inBox = off.tracked && InGrabBox(weapon, off);
    return true;
}

bool ManualReloadCancelsAnimation(void* firing)
{
    Firing f{};
    return Available() && ReadFiring(&f) && reinterpret_cast<void*>(f.object) == firing;
}

bool ManualReloadBlocksFire()
{
    const MagazineState state = State();
    if (state == MagazineState::InGun)
        return false;
    if (state == MagazineState::OnPath) // held in the gun, still seated: it fires
        return g_s > 0.02f;
    return true;
}

} // namespace mohw::openvr_direct
