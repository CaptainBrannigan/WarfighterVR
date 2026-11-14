#include "input_restriction.h"

#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_control.log";
constexpr unsigned kInputYaw = 4;
constexpr unsigned kInputPitch = 5;
constexpr unsigned kInputFire = 9;
constexpr unsigned kInputCount = 74; // FUN_00b22c80 gives back 0..0x49

std::atomic<bool> g_known{false};
std::atomic<bool> g_look{false};
std::atomic<bool> g_fire{false};
std::atomic<unsigned long long> g_lockedChangedMs{0}; // when (look || fire) last changed

bool ReadU32(uintptr_t address, uint32_t* out)
{
    __try
    {
        *out = *reinterpret_cast<const uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadU8(uintptr_t address, uint8_t* out)
{
    __try
    {
        *out = *reinterpret_cast<const uint8_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

struct Mask
{
    uintptr_t player = 0;
    int humans = 0;
    uint32_t low = 0;  // inputs 0..11
    uint64_t high = 0; // inputs 12..73
};

// The first human player in the server's list (single player: the only one).
bool ReadHumanMask(Mask* out)
{
    uint32_t context = 0, manager = 0, begin = 0, end = 0;
    if (!ReadU32(g_baseDelta + OFFSET_SERVERGAMECONTEXT, &context) || !context ||
        !ReadU32(context + SERVERGAMECONTEXT_PLAYERMANAGER, &manager) || !manager ||
        !ReadU32(manager + SERVERPLAYERMANAGER_PLAYERS_BEGIN, &begin) ||
        !ReadU32(manager + SERVERPLAYERMANAGER_PLAYERS_END, &end) || end < begin || (end - begin) % 4 != 0 ||
        end - begin > 256 * 4)
        return false;
    Mask m{};
    for (uint32_t p = begin; p < end; p += 4)
    {
        uint32_t player = 0;
        uint8_t isAi = 1;
        if (!ReadU32(p, &player) || !player || !ReadU8(player + SERVERPLAYER_IS_AI, &isAi) || isAi != 0)
            continue;
        ++m.humans;
        if (m.player)
            continue;
        uint32_t low = 0, highLo = 0, highHi = 0;
        if (!ReadU32(player + PLAYER_INPUT_ALLOWED_LOW, &low) || !ReadU32(player + PLAYER_INPUT_ALLOWED_HIGH, &highLo) ||
            !ReadU32(player + PLAYER_INPUT_ALLOWED_HIGH + 4, &highHi))
            continue;
        m.player = player;
        m.low = low;
        m.high = (static_cast<uint64_t>(highHi) << 32) | highLo;
    }
    if (!m.player)
        return false;
    *out = m;
    return true;
}

bool Allowed(const Mask& m, unsigned input)
{
    if (input < 12)
        return (m.low >> input & 1u) != 0;
    return (m.high >> (input - 12) & 1u) != 0;
}

// The inputs InputRestrictionEntityData names, by the index FUN_00b245b0 hands each one to.
const char* InputName(unsigned input)
{
    switch (input)
    {
    case 0: return "Throttle";
    case 1: return "Strafe";
    case 2: return "Brake";
    case 3: return "SwitchInventory";
    case 4: return "Yaw";
    case 5: return "Pitch";
    case 6: return "Roll";
    case 9: return "Fire";
    case 12: return "Countermeasure";
    case 13: return "Zoom";
    case 14: return "Jump";
    case 15: return "ChangePose";
    case 16: return "Prone";
    case 18: return "Reload";
    case 19: return "Weapon1";
    case 20: return "Weapon2";
    case 21: return "Weapon3";
    case 22: return "Weapon4";
    case 23: return "PrimaryWeapon";
    case 24: return "GrenadeLauncher";
    case 25: return "StaticGadget";
    case 26: return "Gadget1";
    case 27: return "Gadget2";
    case 28: return "Melee";
    case 29: return "ThrowGrenade";
    case 30: return "Sprint";
    case 32: return "FireMode";
    case 36: return "ToggleCamera";
    case 42: return "PeekAndLean";
    case 43: return "BreathControl";
    case 44: return "ChangeVehicle";
    case 57: return "ClassAbility";
    case 60: return "AmmoMode";
    case 65: return "ToggleLTLM";
    case 66: return "Pickup";
    default: return nullptr;
    }
}

void LogMask(const Mask& m, bool look, bool fire)
{
    char taken[768];
    int used = 0;
    taken[0] = '\0';
    int count = 0;
    for (unsigned i = 0; i < kInputCount && used < static_cast<int>(sizeof(taken)) - 24; ++i)
    {
        if (Allowed(m, i))
            continue;
        ++count;
        const char* name = InputName(i);
        if (name)
            used += snprintf(taken + used, sizeof(taken) - used, " %s", name);
        else
            used += snprintf(taken + used, sizeof(taken) - used, " #%u", i);
    }
    MOHW_LOG(kLogFile,
             "INPUT LOCKS on player %08X (%d human): look %s, fire %s; %d input(s) taken:%s (mask %08X %08X%08X)%s",
             static_cast<unsigned>(m.player), m.humans, look ? "TAKEN" : "free", fire ? "TAKEN" : "free", count,
             count ? taken : " none", m.low, static_cast<unsigned>(m.high >> 32), static_cast<unsigned>(m.high),
             GetScriptedScenesFromLocks() ? "" : " -- setting off, not used");
}

} // namespace

void InputRestrictionOnPresent()
{
    if (g_drivingExe)
        return;
    Mask m{};
    if (!ReadHumanMask(&m))
    {
        if (g_known.exchange(false, std::memory_order_relaxed))
        {
            g_look.store(false, std::memory_order_relaxed);
            g_fire.store(false, std::memory_order_relaxed);
            g_lockedChangedMs.store(GetTickCount64(), std::memory_order_relaxed);
            MOHW_LOG(kLogFile, "INPUT LOCKS: no human server player (left the level?)");
        }
        return;
    }
    // Nothing allowed at all reads as a mask that isn't set up yet, not as everything taken (the game always leaves
    // something; the friendly-fire end takes three inputs).
    if (m.low == 0 && m.high == 0)
    {
        static unsigned long long lastLogMs = 0;
        const unsigned long long now = GetTickCount64();
        if (now - lastLogMs > 10000)
        {
            lastLogMs = now;
            MOHW_LOG(kLogFile, "INPUT LOCKS: player %08X's mask reads 0 -- not used", static_cast<unsigned>(m.player));
        }
        g_known.store(false, std::memory_order_relaxed);
        return;
    }
    const bool look = !Allowed(m, kInputYaw) || !Allowed(m, kInputPitch);
    const bool fire = !Allowed(m, kInputFire);
    const bool wasLocked = g_known.load(std::memory_order_relaxed) &&
                           (g_look.load(std::memory_order_relaxed) || g_fire.load(std::memory_order_relaxed));
    g_look.store(look, std::memory_order_relaxed);
    g_fire.store(fire, std::memory_order_relaxed);
    g_known.store(true, std::memory_order_relaxed);
    if ((look || fire) != wasLocked)
        g_lockedChangedMs.store(GetTickCount64(), std::memory_order_relaxed);

    static uintptr_t lastPlayer = 0;
    static uint32_t lastLow = 0;
    static uint64_t lastHigh = 0;
    if (m.player != lastPlayer || m.low != lastLow || m.high != lastHigh)
    {
        lastPlayer = m.player;
        lastLow = m.low;
        lastHigh = m.high;
        LogMask(m, look, fire);
    }
}

bool IsLookRestricted()
{
    return GetScriptedScenesFromLocks() && g_known.load(std::memory_order_relaxed) &&
           g_look.load(std::memory_order_relaxed);
}

bool IsFireRestricted()
{
    return GetScriptedScenesFromLocks() && g_known.load(std::memory_order_relaxed) &&
           g_fire.load(std::memory_order_relaxed);
}

float GetInputLockDriveWeight()
{
    constexpr float kBlendMs = 200.0f;
    constexpr unsigned long long kBackAfterMs = 250;
    if (!GetScriptedScenesFromLocks())
        return 1.0f;
    const unsigned long long since = g_lockedChangedMs.load(std::memory_order_relaxed);
    const unsigned long long now = GetTickCount64();
    const unsigned long long elapsed = since == 0 || now < since ? ~0ull : now - since;
    if (IsLookRestricted() || IsFireRestricted())
    {
        const float t = elapsed == ~0ull ? 1.0f : static_cast<float>(elapsed) / kBlendMs;
        return t >= 1.0f ? 0.0f : 1.0f - t;
    }
    if (elapsed == ~0ull)
        return 1.0f;
    if (elapsed <= kBackAfterMs)
        return 0.0f;
    const float t = static_cast<float>(elapsed - kBackAfterMs) / kBlendMs;
    return t >= 1.0f ? 1.0f : t;
}

} // namespace mohw
