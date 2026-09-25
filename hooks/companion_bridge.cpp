#include "companion_bridge.h"

#include "../shared/ipc_protocol.h"

#include <windows.h>
#include <atomic>

namespace mohw {
namespace {

// Right controller pose -- lazy-open-and-keep-mapped: read every Present, not just occasionally, so it's worth
// keeping the mapping open rather than paying OpenFileMappingW's cost every call.
HANDLE g_rightControllerPoseMapping = nullptr;
const mohwvr::ipc::ControllerPoseBlock* g_rightControllerPoseBlock = nullptr;

bool EnsureRightControllerPoseMappingOpen()
{
    if (g_rightControllerPoseBlock)
        return true;
    if (!g_rightControllerPoseMapping)
    {
        g_rightControllerPoseMapping =
            OpenFileMappingW(FILE_MAP_READ, FALSE, mohwvr::ipc::kRightControllerPoseMapName);
        if (!g_rightControllerPoseMapping)
            return false; // no publisher running yet
    }
    g_rightControllerPoseBlock = static_cast<const mohwvr::ipc::ControllerPoseBlock*>(
        MapViewOfFile(g_rightControllerPoseMapping, FILE_MAP_READ, 0, 0, sizeof(mohwvr::ipc::ControllerPoseBlock)));
    if (!g_rightControllerPoseBlock)
    {
        CloseHandle(g_rightControllerPoseMapping);
        g_rightControllerPoseMapping = nullptr;
        return false;
    }
    return true;
}

} // namespace

mohwvr::ipc::HeadPoseBlock g_headPoseOverride{};
std::atomic<bool> g_hasHeadPoseOverride{false};

void SetHeadPoseOverride(const mohwvr::ipc::HeadPoseBlock& block)
{
    g_headPoseOverride = block;
    g_hasHeadPoseOverride.store(true, std::memory_order_release);
}

bool GetHeadPose(mohwvr::ipc::HeadPoseBlock* out)
{
    if (!g_hasHeadPoseOverride.load(std::memory_order_acquire))
        return false;
    *out = g_headPoseOverride; // small POD copy, no lock -- see ipc_protocol.h's HeadPoseBlock comment
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
    if (!EnsureRightControllerPoseMappingOpen() || !g_rightControllerPoseBlock->ready)
        return false;
    *out = *g_rightControllerPoseBlock; // small POD copy, no lock -- same reasoning as GetHeadPose
    return true;
}

void ShutdownCompanionBridge()
{
    if (g_rightControllerPoseBlock)
    {
        UnmapViewOfFile(const_cast<mohwvr::ipc::ControllerPoseBlock*>(g_rightControllerPoseBlock));
        g_rightControllerPoseBlock = nullptr;
    }
    if (g_rightControllerPoseMapping)
    {
        CloseHandle(g_rightControllerPoseMapping);
        g_rightControllerPoseMapping = nullptr;
    }
}

} // namespace mohw
