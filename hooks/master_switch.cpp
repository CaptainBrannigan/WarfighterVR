#include "master_switch.h"

#include "../openvr_direct/openvr_direct.h"
#include "../sdk/logging.h"

#include <windows.h>
#include <tlhelp32.h>
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
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        found = _wcsicmp(entry.szExeFile, L"vrserver.exe") == 0;
    CloseHandle(snapshot);
    return found;
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
    bool loggedNoServer = false, loggedNoHeadset = false;
    for (;;)
    {
        if (!IsSteamVrRunning())
        {
            if (!loggedNoServer)
                MOHW_LOG(kLogFile, "SteamVR isn't running -- the mod is OFF (the game runs unmodified); checking every %lu s",
                          kRecheckMs / 1000);
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
            return;
        }
        if (!loggedNoHeadset)
            MOHW_LOG(kLogFile, "SteamVR is running but no headset is connected -- the mod is OFF; checking every %lu s",
                      kRecheckMs / 1000);
        loggedNoHeadset = true;
        Sleep(kRecheckMs);
    }
    MOHW_LOG(kLogFile, "headset connected -- switching the mod ON (stays on until the game is restarted)");
    installHooks();
    g_active.store(true, std::memory_order_release);
    MOHW_LOG(kLogFile, "mod ON: hooks installed");
}

} // namespace

void StartMasterSwitch(void (*installHooks)(), unsigned startDelayMs)
{
    std::thread(SwitchThreadProc, installHooks, startDelayMs).detach();
}

bool IsModActive()
{
    return g_active.load(std::memory_order_acquire);
}

} // namespace mohw
