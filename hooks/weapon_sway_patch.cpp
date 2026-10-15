#include "weapon_sway_patch.h"

#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"
#include "camera_matrix_test_hook.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_weaponsway.log";
constexpr unsigned long long kApplyEveryMs = 1000; // re-check known instances, apply the current setting
// Scans after a level loads (the weapons' zoom data arrives with it), once more a little later for anything streamed
// in after. Each scan reads all of the game's writable memory (live: ~1.2 GB, ~5 s on this low-priority thread), so
// not periodically.
constexpr unsigned long long kScanDelaysAfterLoadMs[] = {3000, 20000};
constexpr size_t kMaxInstances = 1024;

// The sway multipliers scaled, and their offsets in ZoomLevelData.
constexpr uint32_t kSwayFields[] = {ZOOMLEVELDATA_SWAYPITCH, ZOOMLEVELDATA_SWAYYAW, ZOOMLEVELDATA_SUPPORTEDSWAYPITCH,
                                    ZOOMLEVELDATA_SUPPORTEDSWAYYAW};
constexpr int kSwayFieldCount = static_cast<int>(sizeof(kSwayFields) / sizeof(kSwayFields[0]));

struct ZoomInstance
{
    uintptr_t address;
    float fov;                       // fingerprint: a write only goes ahead while the address still holds this zoom level
    float original[kSwayFieldCount]; // as the game had them
};

// SEH-safe: whether address still holds a ZoomLevelData (its vtable), checked before every write.
bool StillZoomLevelData(uintptr_t address, uint32_t vtable)
{
    __try
    {
        return *reinterpret_cast<const uint32_t*>(address) == vtable;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadFloatSeh(uintptr_t address, float* out)
{
    __try
    {
        *out = *reinterpret_cast<const float*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteFloatSeh(uintptr_t address, float value)
{
    __try
    {
        *reinterpret_cast<float*>(address) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Plausible(const float* base)
{
    float fov = base[ZOOMLEVELDATA_FIELDOFVIEW / 4];
    if (!std::isfinite(fov) || fov < 1.0f || fov > 180.0f) // a real zoom level has a field of view (0 = not one)
        return false;
    for (uint32_t off : kSwayFields)
    {
        float v = base[off / 4];
        if (!std::isfinite(v) || v < 0.0f || v > 50.0f)
            return false;
    }
    return true;
}

// SEH-safe: dword-aligned addresses in [begin, end) holding vtable with plausible fields.
void ScanRegion(uintptr_t begin, uintptr_t end, uint32_t vtable, uintptr_t* out, size_t maxOut, size_t* count)
{
    __try
    {
        for (uintptr_t p = begin; p + ZOOMLEVELDATA_SIZE <= end && *count < maxOut; p += 4)
        {
            if (*reinterpret_cast<const uint32_t*>(p) == vtable && Plausible(reinterpret_cast<const float*>(p)))
                out[(*count)++] = p;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Every committed, private, read-write region of the process.
std::vector<uintptr_t> ScanMemory(uint32_t vtable, size_t* megabytes)
{
    std::vector<uintptr_t> found(kMaxInstances);
    size_t count = 0;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    size_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < maxAddr && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t next = base + mbi.RegionSize;
        bool readWrite = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0 &&
                         (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && readWrite)
        {
            ScanRegion(base, next, vtable, found.data(), found.size(), &count);
            scanned += mbi.RegionSize;
        }
        if (next <= addr)
            break;
        addr = next;
    }
    found.resize(count);
    *megabytes = scanned >> 20;
    return found;
}

std::vector<ZoomInstance> g_instances; // sway thread only

// Whether address still holds the zoom level recorded (vtable and field of view), checked before every write.
bool StillSameZoomLevel(const ZoomInstance& z, uint32_t vtable)
{
    float fov = 0.0f;
    return StillZoomLevelData(z.address, vtable) && ReadFloatSeh(z.address + ZOOMLEVELDATA_FIELDOFVIEW, &fov) &&
           fov == z.fov;
}

// Replaces the tracked list with this scan's finds. An address already tracked with the same field of view keeps its
// recorded originals (its current values may be ones this patch wrote); anything else is read fresh.
void Rescan(uint32_t vtable)
{
    unsigned long long start = GetTickCount64();
    size_t megabytes = 0;
    std::vector<uintptr_t> found = ScanMemory(vtable, &megabytes);
    std::vector<ZoomInstance> next;
    int fresh = 0;
    for (uintptr_t a : found)
    {
        ZoomInstance z{};
        z.address = a;
        if (!ReadFloatSeh(a + ZOOMLEVELDATA_FIELDOFVIEW, &z.fov))
            continue;
        bool kept = false;
        for (const ZoomInstance& old : g_instances)
            if (old.address == a && old.fov == z.fov)
            {
                z = old;
                kept = true;
            }
        if (!kept)
        {
            bool ok = true;
            for (int i = 0; i < kSwayFieldCount; ++i)
                ok = ok && ReadFloatSeh(a + kSwayFields[i], &z.original[i]);
            if (!ok)
                continue;
            ++fresh;
            MOHW_LOG(kLogFile, "ZoomLevelData @ %p: fov %.1f, sway pitch %.3f yaw %.3f, supported pitch %.3f yaw %.3f",
                      reinterpret_cast<void*>(a), z.fov, z.original[0], z.original[1], z.original[2], z.original[3]);
        }
        next.push_back(z);
    }
    g_instances.swap(next);
    MOHW_LOG(kLogFile, "scan: %u MB in %llu ms, %u ZoomLevelData (%d new)", static_cast<unsigned>(megabytes),
              GetTickCount64() - start, static_cast<unsigned>(g_instances.size()), fresh);
}

// Writes scale into every tracked zoom level that is still the same one.
void ApplyScale(uint32_t vtable, float scale)
{
    int written = 0;
    for (const ZoomInstance& z : g_instances)
    {
        if (!StillSameZoomLevel(z, vtable))
            continue;
        for (int f = 0; f < kSwayFieldCount; ++f)
            WriteFloatSeh(z.address + kSwayFields[f], z.original[f] * scale);
        ++written;
    }
    MOHW_LOG(kLogFile, "applied sway scale %.2f to %d of %u zoom levels", scale, written,
              static_cast<unsigned>(g_instances.size()));
}

// Writes only happen in gameplay, right after a scan or when the setting changes -- never repeatedly: during a level
// change the game frees and reuses this memory, and a stale address re-asserted every second could land in a new
// object (live 2026-09-30: a crash while loading the next level, in game code reading a freed data pointer; not proven
// to be this, but this is the one thing that could write into freed memory).
void SwayThreadProc()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    uint32_t vtable = static_cast<uint32_t>(g_baseDelta + OFFSET_ZOOMLEVELDATA_VTABLE);
    MOHW_LOG(kLogFile, "ADS sway patch thread started (ZoomLevelData vtable %08X)", vtable);
    bool wasInLevel = false;
    unsigned long long levelSinceMs = 0;
    size_t scansDone = 0;
    float appliedScale = 1.0f; // what the tracked zoom levels hold now
    for (;;)
    {
        Sleep(static_cast<DWORD>(kApplyEveryMs));
        unsigned long long now = GetTickCount64();
        float scale = GetAdsSwayScale();
        if (!(scale >= 0.0f && scale <= 1.0f))
            scale = 1.0f;
        bool inLevel = IsPlayerSkeletonLoaded();
        if (inLevel && !wasInLevel)
        {
            levelSinceMs = now;
            scansDone = 0;
        }
        wasInLevel = inLevel;
        if (!inLevel)
            continue;
        constexpr size_t kScanCount = sizeof(kScanDelaysAfterLoadMs) / sizeof(kScanDelaysAfterLoadMs[0]);
        bool scanned = false;
        if (scale < 1.0f && scansDone < kScanCount && now - levelSinceMs >= kScanDelaysAfterLoadMs[scansDone])
        {
            Rescan(vtable);
            ++scansDone;
            scanned = true;
        }
        if (scanned || scale != appliedScale)
        {
            ApplyScale(vtable, scale);
            appliedScale = scale;
        }
    }
}

} // namespace

void StartWeaponSwayPatch()
{
    static bool started = false;
    if (started || g_moduleBase == 0 || !g_buildVerified)
        return;
    started = true;
    std::thread(SwayThreadProc).detach();
}

} // namespace mohw
