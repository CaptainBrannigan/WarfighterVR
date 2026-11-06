#include "player_control.h"

#include "weapon_identity.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"

#include <windows.h>
#include <atomic>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_control.log";
constexpr uint32_t kPlayerManager = 0x34;  // ClientGameContext
constexpr uint32_t kLocalPlayer = 0xB8;    // ClientPlayerManager
constexpr uint32_t kSoldierWeak = 0x42C;   // ClientPlayer
constexpr uint32_t kAttached = 0x440;
constexpr uint32_t kAttachedEntry = 0x444;
constexpr uint32_t kControlled = 0x448;
constexpr uint32_t kControlledEntry = 0x44C;

std::atomic<bool> g_ownSoldier{true};

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

void TypeOf(uintptr_t object, char* out, size_t size)
{
    if (!object)
        snprintf(out, size, "none");
    else if (!GetReflectedTypeName(object, out, size))
        snprintf(out, size, "?");
}

} // namespace

bool GetPlayerControl(PlayerControl* out)
{
    uint32_t context = 0, manager = 0, player = 0;
    if (!ReadU32(g_baseDelta + OFFSET_CLIENTGAMECONTEXT, &context) || !context ||
        !ReadU32(context + kPlayerManager, &manager) || !manager || !ReadU32(manager + kLocalPlayer, &player) ||
        !player)
        return false;
    PlayerControl c{};
    c.player = player;
    uint32_t token = 0, object = 0, v = 0;
    if (ReadU32(player + kSoldierWeak, &token) && token && ReadU32(token, &object) && object != 0 && object != 4)
        c.soldier = object - 4;
    if (ReadU32(player + kAttached, &v))
        c.attached = v;
    if (ReadU32(player + kControlled, &v))
        c.controlled = v;
    ReadU32(player + kAttachedEntry, &c.attachedEntry);
    ReadU32(player + kControlledEntry, &c.controlledEntry);
    *out = c;
    return true;
}

void PlayerControlOnPresent()
{
    if (g_drivingExe)
        return;
    static unsigned long long nextMs = 0;
    const unsigned long long now = GetTickCount64();
    if (now < nextMs)
        return;
    nextMs = now + 250;

    PlayerControl c{};
    const bool have = GetPlayerControl(&c);
    g_ownSoldier.store(!have || !c.controlled || c.controlled == c.soldier, std::memory_order_relaxed);

    static PlayerControl last{};
    static bool lastHave = false;
    if (have == lastHave && c.player == last.player && c.soldier == last.soldier && c.attached == last.attached &&
        c.controlled == last.controlled && c.attachedEntry == last.attachedEntry &&
        c.controlledEntry == last.controlledEntry)
        return;
    last = c;
    lastHave = have;
    if (!have)
    {
        MOHW_LOG(kLogFile, "no local player");
        return;
    }
    char soldierType[64], attachedType[64], controlledType[64];
    TypeOf(c.soldier, soldierType, sizeof(soldierType));
    TypeOf(c.attached, attachedType, sizeof(attachedType));
    TypeOf(c.controlled, controlledType, sizeof(controlledType));
    MOHW_LOG(kLogFile,
             "player %08X: soldier %08X (%s); attached to %08X (%s, entry %u); controls %08X (%s, entry %u)%s",
             static_cast<unsigned>(c.player), static_cast<unsigned>(c.soldier), soldierType,
             static_cast<unsigned>(c.attached), attachedType, c.attachedEntry, static_cast<unsigned>(c.controlled),
             controlledType, c.controlledEntry,
             c.controlled && c.controlled != c.soldier ? " -- not the soldier" : "");
}

bool IsControllingOwnSoldier()
{
    return g_ownSoldier.load(std::memory_order_relaxed);
}

} // namespace mohw
