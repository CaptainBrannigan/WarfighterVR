#include "companion_bridge.h"

#include "../shared/ipc_protocol.h"
#include "../sdk/logging.h"

#include <windows.h>
#include <atomic>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_headpose_staleness.log";

// Written on the submit thread, read on the game thread (shot hooks, trigger polling) -- locked so a reader can
// never see half of one publish's quaternion and half of the next.
std::mutex g_controllerMutex;
mohwvr::ipc::ControllerPoseBlock g_controllerPose[2]{}; // 0 = left, 1 = right

bool GetControllerPose(int hand, mohwvr::ipc::ControllerPoseBlock* out)
{
    std::lock_guard<std::mutex> lock(g_controllerMutex);
    if (!g_controllerPose[hand].ready)
        return false;
    *out = g_controllerPose[hand];
    return true;
}

void SetControllerPose(int hand, const mohwvr::ipc::ControllerPoseBlock& block)
{
    std::lock_guard<std::mutex> lock(g_controllerMutex);
    g_controllerPose[hand] = block;
}

} // namespace

mohwvr::ipc::HeadPoseBlock g_headPoseOverride{};
std::atomic<bool> g_hasHeadPoseOverride{false};

void SetHeadPoseOverride(const mohwvr::ipc::HeadPoseBlock& block)
{
    g_headPoseOverride = block;
    g_hasHeadPoseOverride.store(true, std::memory_order_release);
}

// STALENESS DIAGNOSTIC (2026-09-25): HeadPoseBlock's own frameCounter field was added "so consumers that care
// about staleness can compare against their own last-seen value" (ipc_protocol.h) but confirmed via a full grep
// that nothing ever actually reads it -- this producer (openvr_direct.cpp's SubmitThreadProc) runs on its own
// thread, paced entirely by the blocking WaitGetPoses call, completely unsynchronized with the game thread that
// calls GetHeadPose (30Hz AimingControllerUpdate, or every CommitViewTransform call). Logging how many producer
// frames elapsed and how much real time passed between consecutive GetHeadPose calls, from ANY consumer, to see
// whether that staleness is a steady, predictable amount or jitters call to call -- the latter would explain
// rotation-only jitter that's invisible when still (a stale sample of an unchanging value looks identical to a
// fresh one; a stale sample of a changing value doesn't).
//
// BURST MODE (2026-09-25): the original 200ms rate limit only ever samples one call out of many, which isn't
// conclusive evidence of jitter (a smooth-looking sampled sequence can still hide real per-call variance).
// NUMPAD5 (CheckHeadPoseStalenessBurstHotkey below) arms g_verboseLogRemaining, which makes every single call
// log unconditionally, uncapped, until it counts back down to zero -- a genuine every-call capture instead of a
// sparse sample.
std::atomic<int> g_verboseLogRemaining{0};

// HIGH-RES TIMING (2026-09-25): the first burst capture showed msSinceLastCall landing on EXACTLY 0, 15, or 16
// every single time -- never anything in between. That's not real timing data, it's GetTickCount64's own system
// timer resolution (~15.6ms by default on Windows) quantizing the result: two calls 2ms apart and two calls 14ms
// apart both read as "0", anything crossing one tick boundary reads as "15-16" regardless of the true gap.
// QueryPerformanceCounter gives genuine microsecond-level resolution instead -- called fresh each time rather
// than cached, since QueryPerformanceFrequency is documented as cheap and time-invariant for the process's life,
// avoiding any static-init race entirely.
long long QpcMicros()
{
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1000000LL) / freq.QuadPart;
}

void LogStalenessIfDue(UINT64 frameCounter)
{
    static std::atomic<UINT64> lastSeenFrameCounter{0};
    static std::atomic<long long> lastSeenUs{0};
    static std::atomic<unsigned long long> nextLogAllowedMs{0};

    UINT64 prevFrame = lastSeenFrameCounter.exchange(frameCounter, std::memory_order_relaxed);
    long long nowUs = QpcMicros();
    long long prevUs = lastSeenUs.exchange(nowUs, std::memory_order_relaxed);

    if (prevFrame == 0 || prevUs == 0)
        return; // first-ever call, nothing to compare against yet

    int remaining = g_verboseLogRemaining.load(std::memory_order_relaxed);
    if (remaining > 0)
    {
        g_verboseLogRemaining.fetch_sub(1, std::memory_order_relaxed);
        MOHW_LOG(kLogFile, "GetHeadPose [BURST %d]: framesSinceLastCall=%llu usSinceLastCall=%lld", remaining,
                  static_cast<unsigned long long>(frameCounter - prevFrame), nowUs - prevUs);
        return;
    }

    unsigned long long nowMs = GetTickCount64();
    unsigned long long allowed = nextLogAllowedMs.load(std::memory_order_relaxed);
    if (nowMs >= allowed && nextLogAllowedMs.compare_exchange_strong(allowed, nowMs + 200, std::memory_order_relaxed))
        MOHW_LOG(kLogFile, "GetHeadPose: framesSinceLastCall=%llu usSinceLastCall=%lld",
                  static_cast<unsigned long long>(frameCounter - prevFrame), nowUs - prevUs);
}

void CheckHeadPoseStalenessBurstHotkey()
{
    static bool wasDown = false;
    constexpr int kBurstCount = 300; // ~4-5 real seconds at the confirmed ~62-66Hz call rate
    bool down = (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
    if (down && !wasDown)
    {
        g_verboseLogRemaining.store(kBurstCount, std::memory_order_relaxed);
        MOHW_LOG(kLogFile, "===== BURST ARMED: logging next %d calls unconditionally =====", kBurstCount);
    }
    wasDown = down;
}

bool GetHeadPose(mohwvr::ipc::HeadPoseBlock* out)
{
    if (!g_hasHeadPoseOverride.load(std::memory_order_acquire))
        return false;
    *out = g_headPoseOverride; // small POD copy, no lock -- see ipc_protocol.h's HeadPoseBlock comment
    LogStalenessIfDue(out->frameCounter);
    return true;
}

mohwvr::ipc::HmdViewBlock g_hmdViewOverride{};
std::atomic<bool> g_hasHmdViewOverride{false};

void SetHmdViewOverride(const mohwvr::ipc::HmdViewBlock& block)
{
    g_hmdViewOverride = block;
    g_hasHmdViewOverride.store(true, std::memory_order_release);
}

bool GetHmdView(mohwvr::ipc::HmdViewBlock* out)
{
    if (!g_hasHmdViewOverride.load(std::memory_order_acquire))
        return false;
    *out = g_hmdViewOverride; // small POD copy, no lock -- same reasoning as GetHeadPose
    return true;
}

bool GetRightControllerPose(mohwvr::ipc::ControllerPoseBlock* out)
{
    return GetControllerPose(1, out);
}

bool GetLeftControllerPose(mohwvr::ipc::ControllerPoseBlock* out)
{
    return GetControllerPose(0, out);
}

void SetRightControllerPoseOverride(const mohwvr::ipc::ControllerPoseBlock& block)
{
    SetControllerPose(1, block);
}

void SetLeftControllerPoseOverride(const mohwvr::ipc::ControllerPoseBlock& block)
{
    SetControllerPose(0, block);
}

} // namespace mohw
