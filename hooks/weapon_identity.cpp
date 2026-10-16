#include "weapon_identity.h"

#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_weaponid.log";

// The chain (see the header).
constexpr uint32_t kControllerOwnerField = 0x1C0;  // AimingController -> ClientSoldierWeapon + kOwnerFieldTarget
constexpr uint32_t kOwnerFieldTarget = 0x7C;
constexpr uint32_t kOwnerController = 0x430;       // ClientSoldierWeapon -> its AimingController
constexpr uint32_t kUnlockWeapon = 0x24;           // SoldierWeaponUnlockAsset.Weapon (SoldierWeaponBlueprint)
constexpr uint32_t kAssetName = 0x08;              // Asset.Name (CString)

std::atomic<void*> g_active{nullptr};
std::atomic<unsigned> g_generation{0};
std::mutex g_mutex;
std::string g_key;     // guarded by g_mutex
std::string g_display; // guarded by g_mutex

// The game is large-address-aware: its heap lives above 2 GB.
bool Plausible(uintptr_t p)
{
    return p >= 0x10000 && p < 0xFFFF0000u && (p & 3) == 0;
}

bool InModule(uintptr_t p)
{
    return p >= g_moduleBase && p < g_moduleBase + MOHW_MP_SIZE;
}

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

// A NUL-terminated printable string at address (up to size - 1 chars), SEH-safe.
bool ReadCString(uintptr_t address, char* out, size_t size)
{
    if (address < 0x10000 || address >= 0xFFFF0000u || size < 2)
        return false;
    __try
    {
        const char* s = reinterpret_cast<const char*>(address);
        size_t i = 0;
        for (; i + 1 < size && s[i] != '\0'; ++i)
        {
            if (s[i] < 0x20 || s[i] > 0x7E)
                return false;
            out[i] = s[i];
        }
        out[i] = '\0';
        return i >= 2 && s[i] == '\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadCode6(uintptr_t address, unsigned char* out)
{
    __try
    {
        memcpy(out, reinterpret_cast<const void*>(address), 6);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// A typed object's type name: vtable slot 0 is GetType, "mov eax, <TypeInfo>; ret" (B8 imm32 C3); TypeInfo -> type
// record -> name (Ghidra\gettype_pattern.txt).
bool TypeName(uintptr_t object, char* out, size_t size)
{
    uint32_t vtable = 0, getType = 0, typeInfo = 0, record = 0, name = 0;
    unsigned char code[6] = {};
    if (!Plausible(object) || !ReadU32(object, &vtable) || !InModule(vtable) || !ReadU32(vtable, &getType) ||
        !InModule(getType) || !ReadCode6(getType, code) || code[0] != 0xB8 || code[5] != 0xC3)
        return false;
    memcpy(&typeInfo, code + 1, 4);
    return InModule(typeInfo) && ReadU32(typeInfo, &record) && InModule(record) && ReadU32(record, &name) &&
           InModule(name) && ReadCString(name, out, size);
}

bool IsTypeNamed(uintptr_t object, const char* wanted)
{
    char type[96];
    return TypeName(object, type, sizeof(type)) && strcmp(type, wanted) == 0;
}

// "MOHW/Weapons/ASSAULT/AK103/spec/AK103__AKM" -> "AK103 AKM": the last path part, underscores as spaces.
void DisplayName(const char* key, char* out, size_t size)
{
    const char* last = strrchr(key, '/');
    last = last ? last + 1 : key;
    size_t n = 0;
    for (const char* c = last; *c && n + 1 < size; ++c)
    {
        char ch = *c == '_' ? ' ' : *c;
        if (ch == ' ' && (n == 0 || out[n - 1] == ' '))
            continue; // "__" -> one space
        out[n++] = ch;
    }
    while (n > 0 && out[n - 1] == ' ')
        --n;
    out[n] = '\0';
}

std::atomic<uintptr_t> g_owner{0}; // the active controller's ClientSoldierWeapon, from Resolve

// The controller's weapon, by the chain in the header. *why = the step that failed.
bool Resolve(uintptr_t controller, char* key, size_t keySize, const char** why)
{
    g_owner.store(0, std::memory_order_relaxed);
    uint32_t ownerField = 0, back = 0;
    if (!ReadU32(controller + kControllerOwnerField, &ownerField) || !Plausible(ownerField))
    {
        *why = "no owner pointer at +0x1C0";
        return false;
    }
    uintptr_t owner = ownerField - kOwnerFieldTarget;
    if (!IsTypeNamed(owner, "ClientSoldierWeapon"))
    {
        *why = "+0x1C0 doesn't lead to a ClientSoldierWeapon";
        return false;
    }
    if (!ReadU32(owner + kOwnerController, &back) || back != controller)
    {
        *why = "the ClientSoldierWeapon doesn't hold this controller";
        return false;
    }
    g_owner.store(owner, std::memory_order_relaxed);
    // The unlock asset: the nearest before the controller field.
    for (uint32_t off = kOwnerController - 4; off >= 4; off -= 4)
    {
        uint32_t unlock = 0, blueprint = 0;
        if (!ReadU32(owner + off, &unlock) || !IsTypeNamed(unlock, "SoldierWeaponUnlockAsset"))
            continue;
        if (ReadU32(unlock + kUnlockWeapon, &blueprint) && Plausible(blueprint))
        {
            uint32_t name = 0;
            if (ReadU32(blueprint + kAssetName, &name) && ReadCString(name, key, keySize))
                return true;
        }
        uint32_t name = 0; // no blueprint: the unlock's own name
        if (ReadU32(unlock + kAssetName, &name) && ReadCString(name, key, keySize))
            return true;
    }
    *why = "no SoldierWeaponUnlockAsset in the ClientSoldierWeapon";
    return false;
}

} // namespace

void NoteActiveAimingController(void* aimingController)
{
    void* previous = g_active.exchange(aimingController, std::memory_order_relaxed);
    if (previous == aimingController || !aimingController)
        return;
    char key[160] = "", display[96] = "";
    const char* why = "";
    bool ok = Resolve(reinterpret_cast<uintptr_t>(aimingController), key, sizeof(key), &why);
    if (ok)
        DisplayName(key, display, sizeof(display));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_key = ok ? key : "";
        g_display = ok ? display : "";
    }
    g_generation.fetch_add(1, std::memory_order_release);
    // The per-weapon settings follow (sdk/settings.h SetWeaponProfile); unidentified = the default profile.
    SetWeaponProfile(ok ? key : nullptr, ok ? display : nullptr);
    if (ok)
        MOHW_LOG(kLogFile, "equipped: %s (%s), controller %p", display, key, aimingController);
    else
        MOHW_LOG(kLogFile, "equipped: unidentified (controller %p: %s)", aimingController, why);
}

bool GetEquippedWeapon(char* key, size_t keySize, char* display, size_t displaySize)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_key.empty())
        return false;
    if (key && keySize)
        snprintf(key, keySize, "%s", g_key.c_str());
    if (display && displaySize)
        snprintf(display, displaySize, "%s", g_display.c_str());
    return true;
}

unsigned GetEquippedWeaponGeneration()
{
    return g_generation.load(std::memory_order_acquire);
}

bool GetReflectedTypeName(uintptr_t object, char* out, size_t size)
{
    return TypeName(object, out, size);
}

uintptr_t GetEquippedSoldierWeapon()
{
    uintptr_t owner = g_owner.load(std::memory_order_relaxed);
    uint32_t held = 0;
    uintptr_t active = reinterpret_cast<uintptr_t>(g_active.load(std::memory_order_relaxed));
    if (!owner || !ReadU32(owner + kOwnerController, &held) || held != active)
        return 0;
    return owner;
}

} // namespace mohw
