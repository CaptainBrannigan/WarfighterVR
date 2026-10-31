#include "master_switch.h"

#include "../openvr_direct/openvr_direct.h"
#include "../sdk/logging.h"
#include "startup_diag.h"

#include <windows.h>
#include <atomic>
#include <thread>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_master.log";
constexpr DWORD kRecheckMs = 5000;

std::atomic<bool> g_active{false};

// Whether SteamVR's server is running. The connection is only tried then: VR_Init otherwise launches SteamVR itself
// (live 2026-10-15: playing flat with no headset, the game started SteamVR, which then reported no headset).
bool IsSteamVrRunning()
{
    return IsProcessRunning(L"vrserver.exe");
}

// HEADSET TEST (2026-10-15): the VR connection itself. VR_IsHmdPresent was the first try and said yes with no headset
// (the wireless driver counts as present) -- every hook went in and changed flat play (arms hidden, pitch locked,
// weapon offset). The connection only succeeds with a headset there (otherwise VR_Init fails, 215
// Driver_WirelessHmdNotConnected), and it has to be made anyway.
void SwitchThreadProc(void (*installHooks)(), unsigned startDelayMs)
{
    if (startDelayMs > 0)
    {
        MOHW_LOG(kLogFile, "waiting %u ms before the first check", startDelayMs);
        Sleep(startDelayMs);
    }
    const char* report = StartupLogFile();
    bool loggedNoServer = false, loggedNoHeadset = false;
    for (;;)
    {
        if (!IsSteamVrRunning())
        {
            if (!loggedNoServer)
            {
                MOHW_LOG(kLogFile, "SteamVR isn't running -- the mod is OFF (the game runs unmodified); checking every %lu s",
                          kRecheckMs / 1000);
                MOHW_LOG(report, "mod OFF: SteamVR isn't running (no vrserver.exe). The mod needs it and never starts it; "
                                 "checking every %lu s", kRecheckMs / 1000);
                if (IsProcessRunning(L"VirtualDesktop.Streamer.exe"))
                    MOHW_LOG(report, "Virtual Desktop is running, but its VDXR OpenXR runtime can't serve this mod, which "
                                     "uses OpenVR: start SteamVR from Virtual Desktop");
            }
            loggedNoServer = true;
            loggedNoHeadset = false;
            Sleep(kRecheckMs);
            continue;
        }
        loggedNoServer = false;
        openvr_direct::ConnectResult r = openvr_direct::TryConnectOpenVrDirect();
        if (r == openvr_direct::ConnectResult::Connected)
            break;
        if (r == openvr_direct::ConnectResult::Failed)
        {
            MOHW_LOG(kLogFile, "the VR connection failed (see mohwvr_openvr_direct.log) -- the mod stays OFF this session");
            MOHW_LOG(report, "mod OFF for this session: the VR connection failed, see the lines above and "
                             "mohwvr_openvr_direct.log");
            return;
        }
        if (!loggedNoHeadset)
        {
            MOHW_LOG(kLogFile, "SteamVR is running but the VR connection isn't up -- the mod is OFF; checking every %lu s",
                      kRecheckMs / 1000);
            MOHW_LOG(report, "mod OFF for now: SteamVR is running but the VR connection isn't up (reason above); "
                             "checking every %lu s", kRecheckMs / 1000);
        }
        loggedNoHeadset = true;
        Sleep(kRecheckMs);
    }
    MOHW_LOG(kLogFile, "headset connected -- switching the mod ON (stays on until the game is restarted)");
    BeginStartupStep("installing hooks");
    installHooks();
    EndStartupStep();
    g_active.store(true, std::memory_order_release);
    MOHW_LOG(kLogFile, "mod ON: hooks installed");
    MOHW_LOG(report, "mod ON: connected and hooks installed");
}

} // namespace

void StartMasterSwitch(void (*installHooks)(), unsigned startDelayMs)
{
    StartStartupWatchdog();
    std::thread(SwitchThreadProc, installHooks, startDelayMs).detach();
}

bool IsModActive()
{
    return g_active.load(std::memory_order_acquire);
}

} // namespace mohw
