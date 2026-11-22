#include "perf_probe.h"

#include "../sdk/logging.h"

#include <windows.h>
#include <atomic>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_perf.log";
constexpr double kDetailSeconds = 120.0; // a line a second this long after a level is built (and after launch)
constexpr double kDetailPeriodMs = 1000.0;
constexpr double kQuietPeriodMs = 10000.0;

const char* const kSlotNames[kPerfSlotCount] = {"batches", "eyes", "commit", "present", "scope", "pair",
                                                "vrinput", "submit", "shown", "late", "dropped"};
// Counted, not timed: printed as the count alone.
const bool kCountOnly[kPerfSlotCount] = {false, false, false, false, false, false, false, false, true, true, true};

std::atomic<long long> g_ticks[kPerfSlotCount];
std::atomic<long> g_calls[kPerfSlotCount];
std::atomic<long long> g_levelBuiltAt{0};

double MsPerTick()
{
    static const double value = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
    }();
    return value;
}

} // namespace

long long PerfNow()
{
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void PerfAdd(PerfSlot slot, long long ticks)
{
    if (slot < 0 || slot >= kPerfSlotCount || ticks < 0)
        return;
    g_ticks[slot].fetch_add(ticks, std::memory_order_relaxed);
    g_calls[slot].fetch_add(1, std::memory_order_relaxed);
}

void PerfNoteLevelBuilt()
{
    g_levelBuiltAt.store(PerfNow(), std::memory_order_relaxed);
    MOHW_LOG_ALWAYS(kLogFile, "---- a level is being built: a line a second for %.0f s ----", kDetailSeconds);
}

void PerfOnPresent()
{
    static long long launchedAt = 0, lastPresent = 0, windowStart = 0;
    static int frames = 0;
    static double frameSumMs = 0.0, worstMs = 0.0;
    const long long now = PerfNow();
    const double msPerTick = MsPerTick();
    if (!launchedAt)
    {
        launchedAt = windowStart = now;
        MOHW_LOG_ALWAYS(kLogFile, "perf probe: per frame, our own code's time next to the frame time (ms); calls in brackets");
    }
    if (lastPresent)
    {
        const double dt = static_cast<double>(now - lastPresent) * msPerTick;
        ++frames;
        frameSumMs += dt;
        if (dt > worstMs)
            worstMs = dt;
    }
    lastPresent = now;

    const long long builtAt = g_levelBuiltAt.load(std::memory_order_relaxed);
    const double sinceLaunch = static_cast<double>(now - launchedAt) * msPerTick / 1000.0;
    const double sinceLevel = builtAt ? static_cast<double>(now - builtAt) * msPerTick / 1000.0 : -1.0;
    const bool detail = sinceLaunch < kDetailSeconds || (sinceLevel >= 0.0 && sinceLevel < kDetailSeconds);
    const double windowMs = static_cast<double>(now - windowStart) * msPerTick;
    if (windowMs < (detail ? kDetailPeriodMs : kQuietPeriodMs) || frames == 0)
        return;

    char slots[512] = "";
    int used = 0;
    for (int s = 0; s < kPerfSlotCount && used < static_cast<int>(sizeof(slots)) - 64; ++s)
    {
        const long long ticks = g_ticks[s].exchange(0, std::memory_order_relaxed);
        const long calls = g_calls[s].exchange(0, std::memory_order_relaxed);
        if (kCountOnly[s])
            used += sprintf_s(slots + used, sizeof(slots) - used, "%s%s %ld", s ? ", " : "", kSlotNames[s], calls);
        else
            used += sprintf_s(slots + used, sizeof(slots) - used, "%s%s %.2f (%ld)", s ? ", " : "", kSlotNames[s],
                              static_cast<double>(ticks) * msPerTick / frames, calls);
    }
    char level[32] = "";
    if (sinceLevel >= 0.0)
        sprintf_s(level, "level +%.0f s, ", sinceLevel);
    MOHW_LOG_ALWAYS(kLogFile, "%s%d frames, %.1f fps, average %.2f ms, worst %.1f ms | ours per frame: %s", level, frames,
                    frames * 1000.0 / windowMs, frameSumMs / frames, worstMs, slots);
    windowStart = now;
    frames = 0;
    frameSumMs = 0.0;
    worstMs = 0.0;
}

} // namespace mohw
