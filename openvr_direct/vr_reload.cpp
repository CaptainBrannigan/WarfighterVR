#include "vr_reload.h"

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

// Written on the submit thread, read by the render threads.
std::atomic<int> g_state{static_cast<int>(MagazineState::InGun)};
std::mutex g_viewMutex;
MagazineView g_view; // guarded by g_viewMutex

std::mutex g_geometryMutex;
MagazineGeometry g_geometry{}; // guarded by g_geometryMutex
std::atomic<unsigned long long> g_pointTick{0};

// Submit thread.
bool g_heldIsNew = false;      // what the hand holds is a fresh magazine, not the one just pulled out
bool g_leftTheWell = false;    // the held magazine has been away from the gun (a held one only seats after that)
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

void SetState(MagazineState state, const char* why)
{
    g_state.store(static_cast<int>(state), std::memory_order_relaxed);
    MOHW_LOG(kLogFile, "magazine: %s (%s)",
             state == MagazineState::InGun ? "in the gun" : state == MagazineState::Held ? "in the hand" : "at the pouch",
             why);
}

bool Available()
{
    if (!GetManualReloadEnabled() || !GetWeaponDriveEnabled() || !IsPlayerAimActive())
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

float Distance(const Vec3& a, const Vec3& b)
{
    Vec3 d = VecSub(a, b);
    return sqrtf(VecDot(d, d));
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

// How far along the seat -> aim line the hand is (below the seat is negative).
float AlongAxis(const SeatNowResult& s, const Vec3& hand)
{
    Vec3 axis = VecSub(s.aim, s.seat);
    const float length = sqrtf(VecDot(axis, axis));
    if (length < 1e-3f)
        return 0.0f;
    return VecDot(VecSub(hand, s.seat), VecScale(axis, 1.0f / length));
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

} // namespace

MagazineView GetMagazineView()
{
    std::lock_guard<std::mutex> lock(g_viewMutex);
    MagazineView view = g_view;
    view.state = static_cast<MagazineState>(g_state.load(std::memory_order_relaxed));
    return view;
}

void PublishMagazineGeometry(const MagazineGeometry& geometry)
{
    {
        std::lock_guard<std::mutex> lock(g_geometryMutex);
        g_geometry = geometry;
    }
    g_pointTick.store(GetTickCount64(), std::memory_order_relaxed);
}

void UpdateManualReload(const HandInput hands[2], int weaponHand, bool haveHead, const float headPos[3],
                        const Vec3& pouchCentre, const Vec3& bodyRight, const Vec3& bodyForward)
{
    unsigned long long now = GetTickCount64();
    if (g_reloadKeyUpAt != 0 && now >= g_reloadKeyUpAt)
    {
        SetGameActionKey("ConceptReload", false);
        g_reloadKeyUpAt = 0;
    }
    if (haveHead)
    {
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

    MagazineState state = static_cast<MagazineState>(g_state.load(std::memory_order_relaxed));
    unsigned generation = GetEquippedWeaponGeneration();
    bool available = Available();
    if ((generation != g_weaponGeneration || !available) && state != MagazineState::InGun)
        SetState(MagazineState::InGun, generation != g_weaponGeneration ? "weapon changed" : "manual reload off for now");
    g_weaponGeneration = generation;
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
    state = static_cast<MagazineState>(g_state.load(std::memory_order_relaxed));

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

    if (state == MagazineState::Held)
    {
        const HandInput& weapon = hands[weaponHand];
        const HandInput& off = hands[1 - weaponHand];
        if (!weapon.tracked || !off.tracked)
            return;
        const MagazineProfile p = GetMagazineProfile();
        const SeatNowResult s = SeatNow(weapon);
        const Vec3 hand = Position(off);
        const float d = Distance(hand, s.seat);
        if (d > p.lockRange)
            g_leftTheWell = true;
        // Locked to the line: it's in once the hand brings it up to the seat.
        if (g_leftTheWell && d <= p.lockRange && AlongAxis(s, hand) >= -p.seatRange)
            Seat();
    }
}

bool ManualReloadGripPressed(const HandInput& weapon, const HandInput& off, const Vec3& pouchCentre, bool haveHead)
{
    if (!Available() || !weapon.tracked || !off.tracked)
        return false;
    MagazineState state = static_cast<MagazineState>(g_state.load(std::memory_order_relaxed));
    if (state == MagazineState::InGun && InGrabBox(weapon, off))
    {
        g_heldIsNew = false;
        g_leftTheWell = false;
        SetState(MagazineState::Held, "pulled out");
        return true;
    }
    if (state == MagazineState::Out)
    {
        // A grip anywhere else stays with the holsters and the two-handed grab.
        if (haveHead && Distance(Position(off), pouchCentre) <= GetMagazinePouchRadius())
        {
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
    if (static_cast<MagazineState>(g_state.load(std::memory_order_relaxed)) != MagazineState::Held)
        return;
    // Let go near the seat with the pulled magazine still there: it just stays in.
    if (!g_leftTheWell && Distance(Position(off), SeatNow(weapon).seat) <= GetMagazineProfile().lockRange)
    {
        SetState(MagazineState::InGun, "let go without pulling it out");
        return;
    }
    SetState(MagazineState::Out, "let go");
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
    out->range = p.range;
    out->lockRange = p.lockRange;
    out->handDistance = off.tracked ? Distance(Position(off), s.seat) : 1e6f;
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
    return static_cast<MagazineState>(g_state.load(std::memory_order_relaxed)) != MagazineState::InGun;
}

} // namespace mohw::openvr_direct
