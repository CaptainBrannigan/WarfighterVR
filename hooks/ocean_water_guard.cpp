#include "ocean_water_guard.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cstdint>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_oceanwater.log";
constexpr int kMaxPatches = 1000;          // FUN_00CF7BC0's own clamp: the transform buffer holds 1000
constexpr uintptr_t kTransformBytes = 64000; // FUN_00CF87F0 allocates it with this size (module +0x8C8)

// The job entries: __cdecl(job data, argument).
using JobFn = void(__cdecl*)(void* data, void* argument);
JobFn g_cull = nullptr;
JobFn g_preDraw = nullptr;
JobFn g_draw = nullptr;

// FUN_00CEFBA0: __thiscall(module, context, draw list builder, LOD, first transform, count), RET 0x14.
using LodDrawFn = void(__fastcall*)(void* module, void* edx, void* context, void* builder, uint32_t lod,
                                    const unsigned char* transforms, int count);
LodDrawFn g_lodDraw = nullptr;

struct Target
{
    uintptr_t offset;
    void* detour;
    void** original;
    const char* name;
    void* address;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<long> g_waits{0};   // a job had to wait for another view's water jobs
std::atomic<long> g_skipped{0}; // LOD draws whose range was bad
std::atomic<long> g_jobs{0};

void LogIfDue()
{
    static std::atomic<unsigned long long> nextMs{0};
    const unsigned long long now = GetTickCount64();
    unsigned long long due = nextMs.load(std::memory_order_relaxed);
    if (now < due || !nextMs.compare_exchange_strong(due, now + 5000, std::memory_order_relaxed))
        return;
    const long jobs = g_jobs.exchange(0), waits = g_waits.exchange(0), skipped = g_skipped.exchange(0);
    if (waits != 0 || skipped != 0)
        MOHW_LOG(kLogFile,
                 "last 5 s: %ld water job(s) under the two-view lock, %ld waited for the other view's (each one a "
                 "collision the lock prevented), %ld LOD draw(s) with a bad range skipped",
                 jobs, waits, skipped);
}

void AcquireExclusive()
{
    if (!TryAcquireSRWLockExclusive(&g_lock))
    {
        g_waits.fetch_add(1, std::memory_order_relaxed);
        AcquireSRWLockExclusive(&g_lock);
    }
}

void AcquireShared()
{
    if (!TryAcquireSRWLockShared(&g_lock))
    {
        g_waits.fetch_add(1, std::memory_order_relaxed);
        AcquireSRWLockShared(&g_lock);
    }
}

// The setting is read once per job, so a job releases the lock it took even if the setting changes meanwhile.
void __cdecl HookedCull(void* data, void* argument)
{
    const bool lock = GetNativeStereoEnabled();
    if (lock)
    {
        AcquireExclusive();
        g_jobs.fetch_add(1, std::memory_order_relaxed);
    }
    g_cull(data, argument);
    if (lock)
        ReleaseSRWLockExclusive(&g_lock);
}

void __cdecl HookedPreDraw(void* data, void* argument)
{
    const bool lock = GetNativeStereoEnabled();
    if (lock)
    {
        AcquireExclusive();
        g_jobs.fetch_add(1, std::memory_order_relaxed);
    }
    g_preDraw(data, argument);
    if (lock)
        ReleaseSRWLockExclusive(&g_lock);
}

void __cdecl HookedDraw(void* data, void* argument)
{
    const bool lock = GetNativeStereoEnabled();
    if (lock)
    {
        AcquireShared();
        g_jobs.fetch_add(1, std::memory_order_relaxed);
    }
    g_draw(data, argument);
    if (lock)
        ReleaseSRWLockShared(&g_lock);
    LogIfDue();
}

// The LOD's transforms must lie inside the module's transform buffer (module +0x828, 64000 bytes). SEH-guarded.
bool RangeInBuffer(const void* module, const unsigned char* transforms, int count)
{
    if (count < 0 || count > kMaxPatches)
        return false;
    uintptr_t base = 0;
    __try
    {
        base = *reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(module) + OCEANWATER_TRANSFORMS);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    const uintptr_t first = reinterpret_cast<uintptr_t>(transforms);
    return base != 0 && first >= base && first + static_cast<uintptr_t>(count) * 64u <= base + kTransformBytes;
}

void __fastcall HookedLodDraw(void* module, void* edx, void* context, void* builder, uint32_t lod,
                              const unsigned char* transforms, int count)
{
    if (count != 0 && !RangeInBuffer(module, transforms, count))
    {
        const long n = g_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
        static std::atomic<int> lines{0};
        if (lines.fetch_add(1, std::memory_order_relaxed) < 20)
            MOHW_LOG(kLogFile, "LOD %u's range was bad (count %d from %p): drew nothing for it instead of copying (#%ld)",
                     lod, count, static_cast<const void*>(transforms), n);
        count = 0; // FUN_00CEFBA0 does nothing for a count of 0
    }
    g_lodDraw(module, edx, context, builder, lod, transforms, count);
}

Target g_targets[] = {
    {OFFSET_OCEAN_CULL_JOB, reinterpret_cast<void*>(&HookedCull), reinterpret_cast<void**>(&g_cull), "oceanWaterCull job",
     nullptr},
    {OFFSET_OCEAN_PREDRAW_JOB, reinterpret_cast<void*>(&HookedPreDraw), reinterpret_cast<void**>(&g_preDraw),
     "oceanWaterPreDraw job", nullptr},
    {OFFSET_OCEAN_DRAW_JOB, reinterpret_cast<void*>(&HookedDraw), reinterpret_cast<void**>(&g_draw),
     "oceanWaterDraw job", nullptr},
    {OFFSET_OCEAN_LOD_DRAW, reinterpret_cast<void*>(&HookedLodDraw), reinterpret_cast<void**>(&g_lodDraw),
     "LOD draw FUN_00CEFBA0", nullptr},
};

} // namespace

bool InstallOceanWaterGuard()
{
    if (g_moduleBase == 0 || !g_buildVerified || g_drivingExe)
        return false;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    bool all = true;
    for (Target& t : g_targets)
    {
        void* address = Offset<void*>(t.offset);
        s = MH_CreateHook(address, t.detour, t.original);
        if (s == MH_OK)
            s = MH_EnableHook(address);
        if (s != MH_OK)
        {
            MOHW_LOG(kLogFile, "%s hook @ %p FAILED: %s", t.name, address, MH_StatusToString(s));
            all = false;
            continue;
        }
        t.address = address;
    }
    MOHW_LOG(kLogFile, "ocean water guard %s: the water jobs serialized while NativeStereo is on, LOD ranges checked",
             all ? "installed" : "PARTLY installed");
    return all;
}

void RemoveOceanWaterGuard()
{
    for (Target& t : g_targets)
    {
        if (!t.address)
            continue;
        MH_DisableHook(t.address);
        MH_RemoveHook(t.address);
        t.address = nullptr;
    }
}

} // namespace mohw
