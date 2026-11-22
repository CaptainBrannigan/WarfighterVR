#include "weapon_state_probe.h"

#include "player_control.h"
#include "weapon_identity.h"
#include "../sdk/logging.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_weaponstate.log";
constexpr unsigned long long kSlotsCheckMs = 1000;

// ---- Readable memory: VirtualQuery, its regions cached (readable or not), so a walk doesn't fault on every float that
// looks like a pointer. SEH stays as the last guard (a region can be freed after it was cached). Game thread only.
struct Region
{
    uintptr_t base = 0, end = 0;
    bool readable = false;
};
constexpr int kRegionCache = 256;
Region g_regions[kRegionCache];
int g_regionCount = 0, g_regionNext = 0;

bool Readable(uintptr_t p, size_t n)
{
    if (p < 0x10000 || p + n < p)
        return false;
    for (int i = 0; i < g_regionCount; ++i)
        if (p >= g_regions[i].base && p < g_regions[i].end)
            return g_regions[i].readable && p + n <= g_regions[i].end;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    const DWORD protect = mbi.Protect & 0xFF;
    Region r;
    r.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    r.end = r.base + mbi.RegionSize;
    r.readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                 (protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                  protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY);
    g_regions[g_regionNext] = r;
    g_regionNext = (g_regionNext + 1) % kRegionCache;
    if (g_regionCount < kRegionCache)
        ++g_regionCount;
    return r.readable && p + n <= r.end;
}

bool SafeCopy(void* dst, uintptr_t src, size_t n)
{
    __try
    {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadBlock(void* dst, uintptr_t src, size_t n)
{
    return Readable(src, n) && SafeCopy(dst, src, n);
}

bool ReadU32(uintptr_t src, uint32_t* out)
{
    return ReadBlock(out, src, 4);
}

bool Plausible(uintptr_t p)
{
    return p >= 0x10000 && p < 0xFFFF0000u && (p & 3) == 0;
}

// ---- 2. The weapon slots ----
constexpr uint32_t kComponentSoldier = 0xC8;
constexpr uint32_t kComponentSlotsBegin = 0xCC;
constexpr uint32_t kComponentSlotsEnd = 0xD0;
constexpr int kMaxSlots = 16;

uintptr_t g_lastWeapon = 0;
unsigned long long g_weaponChangedMs = 0, g_nextSlotsCheckMs = 0;
bool g_slotsLoggedForWeapon = false;
char g_lastSlotsSignature[512] = "";

// The soldier's weapons component: reached from the soldier (its first 0x1000 bytes), its +0xC8 that soldier, its slot
// array holding the weapon in hand. 0 if none.
uintptr_t FindWeaponsComponent(uintptr_t soldier, uintptr_t weapon, uint32_t* soldierOffset)
{
    static uint32_t words[0x1000 / 4];
    if (!ReadBlock(words, soldier, sizeof(words)))
        return 0;
    for (int i = 0; i < 0x400; ++i)
    {
        const uintptr_t p = words[i];
        uint32_t back = 0, begin = 0, end = 0;
        if (!Plausible(p) || p == soldier || !ReadU32(p + kComponentSoldier, &back) || back != soldier ||
            !ReadU32(p + kComponentSlotsBegin, &begin) || !ReadU32(p + kComponentSlotsEnd, &end) || !Plausible(begin) ||
            end <= begin || end - begin > kMaxSlots * 4)
            continue;
        uint32_t slots[kMaxSlots] = {};
        if (!ReadBlock(slots, begin, end - begin))
            continue;
        for (uint32_t k = 0; k < (end - begin) / 4; ++k)
            if (slots[k] == weapon)
            {
                *soldierOffset = static_cast<uint32_t>(i * 4);
                return p;
            }
    }
    return 0;
}

uintptr_t g_component = 0, g_componentSoldier = 0; // the last found, checked again before reuse
uint32_t g_componentOffset = 0;

void CheckSlots(uintptr_t weapon, unsigned long long now, bool force)
{
    (void)now;
    PlayerControl control{};
    if (!GetPlayerControl(&control) || !control.soldier)
        return;
    uint32_t offset = g_componentOffset, back = 0;
    uintptr_t component = 0;
    if (g_component && g_componentSoldier == control.soldier && ReadU32(g_component + kComponentSoldier, &back) &&
        back == control.soldier)
        component = g_component;
    else
        component = FindWeaponsComponent(control.soldier, weapon, &offset);
    g_component = component;
    g_componentSoldier = control.soldier;
    g_componentOffset = offset;
    char signature[512] = "";
    char lines[kMaxSlots][200] = {};
    int lineCount = 0;
    uint32_t begin = 0, end = 0;
    if (component && ReadU32(component + kComponentSlotsBegin, &begin) && ReadU32(component + kComponentSlotsEnd, &end))
    {
        uint32_t slots[kMaxSlots] = {};
        const uint32_t count = (end - begin) / 4;
        ReadBlock(slots, begin, count * 4);
        int used = 0;
        for (uint32_t k = 0; k < count && k < kMaxSlots; ++k)
        {
            char key[160] = "", display[96] = "";
            const bool named = slots[k] && GetSoldierWeaponName(slots[k], key, sizeof(key), display, sizeof(display));
            used += sprintf_s(signature + used, sizeof(signature) - used, "%u:%s;", k, named ? display : slots[k] ? "?" : "-");
            if (slots[k])
                sprintf_s(lines[lineCount++], "    slot %u: %08X %s%s%s", k, slots[k], named ? display : "(not a named weapon)",
                          named ? "  " : "", slots[k] == weapon ? "<- in hand" : "");
        }
    }
    else
        sprintf_s(signature, "no component");
    if (!force && strcmp(signature, g_lastSlotsSignature) == 0)
        return;
    strcpy_s(g_lastSlotsSignature, signature);
    char type[96] = "";
    if (component && !GetReflectedTypeName(component, type, sizeof(type)))
        strcpy_s(type, "untyped");
    char display[96] = "";
    GetEquippedWeapon(nullptr, 0, display, sizeof(display));
    if (!component)
    {
        MOHW_LOG_ALWAYS(kLogFile, "WEAPON SLOTS: not found (soldier %08X, %s in hand, ClientSoldierWeapon %08X)",
                        static_cast<unsigned>(control.soldier), display, static_cast<unsigned>(weapon));
        return;
    }
    MOHW_LOG_ALWAYS(kLogFile, "WEAPON SLOTS (%s in hand): soldier %08X +0x%X -> %s %08X, slots %08X..%08X (%u)", display,
                    static_cast<unsigned>(control.soldier), offset, type, static_cast<unsigned>(component), begin, end,
                    (end - begin) / 4);
    for (int i = 0; i < lineCount; ++i)
        MOHW_LOG_ALWAYS(kLogFile, "%s", lines[i]);
}

} // namespace

void WeaponStateProbeOnPresent()
{
    const unsigned long long now = GetTickCount64();
    const uintptr_t weapon = GetEquippedSoldierWeapon();
    if (!weapon)
        return;
    if (weapon != g_lastWeapon)
    {
        g_lastWeapon = weapon;
        g_weaponChangedMs = now;
        g_slotsLoggedForWeapon = false;
    }
    // A second after each change (the slots settle), then every second while they change (a pickup).
    if (!g_slotsLoggedForWeapon && now - g_weaponChangedMs >= kSlotsCheckMs)
    {
        g_slotsLoggedForWeapon = true;
        g_regionCount = g_regionNext = 0; // a fresh look at the address space
        CheckSlots(weapon, now, true);
        g_nextSlotsCheckMs = now + kSlotsCheckMs;
    }
    else if (g_slotsLoggedForWeapon && now >= g_nextSlotsCheckMs)
    {
        g_nextSlotsCheckMs = now + kSlotsCheckMs;
        CheckSlots(weapon, now, false);
    }
}

} // namespace mohw
