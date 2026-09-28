#include "xinput_hook.h"

#include "../openvr_direct/vr_input.h"
#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <Xinput.h> // types only -- nothing from xinput is linked, the real function is reached through the hook

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_xinput.log";

// Below this, a VR stick counts as centred and a real gamepad's value for that stick is used instead. Kept small:
// the game applies its own XInput deadzone on top.
constexpr float kVrStickActiveThreshold = 0.05f;

using XInputGetStateFn = DWORD(WINAPI*)(DWORD userIndex, XINPUT_STATE* state);
using XInputGetCapabilitiesFn = DWORD(WINAPI*)(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* caps);
XInputGetStateFn g_original = nullptr;
XInputGetCapabilitiesFn g_originalCaps = nullptr;
void* g_hookAddress = nullptr;
void* g_capsHookAddress = nullptr;

SHORT ToThumb(float value)
{
    if (value > 1.0f)
        value = 1.0f;
    if (value < -1.0f)
        value = -1.0f;
    return static_cast<SHORT>(lroundf(value * 32767.0f));
}

DWORD WINAPI Hooked(DWORD userIndex, XINPUT_STATE* state)
{
    DWORD result = g_original(userIndex, state);
    if (userIndex != 0 || !state)
        return result;

    // Controller 0 always exists from the game's point of view, idle until SteamVR Input is running: the game may
    // only look for controllers early on, before the VR connection is up.
    if (result != ERROR_SUCCESS)
    {
        memset(state, 0, sizeof(*state));
        result = ERROR_SUCCESS;
    }

    float moveX = 0.0f, moveY = 0.0f, turnX = 0.0f;
    if (!openvr_direct::GetVrSticks(&moveX, &moveY, &turnX))
        return result;

    XINPUT_GAMEPAD& pad = state->Gamepad;
    // Outer deadzone: VR sticks often don't reach 1.0 in every direction (live: Touch right stick peaked at 0.92
    // right vs 0.99 left, so turning right was slower). Deflection past VrStickFullDeflection counts as full, the
    // move stick scaled by its length so its direction is kept.
    float fullAt = GetVrStickFullDeflection();
    if (fullAt < 0.5f || fullAt > 1.0f)
        fullAt = 1.0f;
    float moveLen = sqrtf(moveX * moveX + moveY * moveY);
    if (moveLen > kVrStickActiveThreshold)
    {
        float scale = moveLen / fullAt > 1.0f ? 1.0f / moveLen : 1.0f / fullAt;
        pad.sThumbLX = ToThumb(moveX * scale);
        pad.sThumbLY = ToThumb(moveY * scale);
    }
    float turn = turnX / fullAt * GetVrTurnSpeed();
    if (fabsf(turnX) > kVrStickActiveThreshold)
    {
        pad.sThumbRX = ToThumb(turn);
        pad.sThumbRY = 0;
    }

    // Pad buttons and triggers from the SteamVR pad actions (openvr_direct/vr_input.cpp), on top of a real pad's.
    WORD vrButtons = 0;
    BYTE vrLeftTrigger = 0, vrRightTrigger = 0;
    if (openvr_direct::GetVrPadState(&vrButtons, &vrLeftTrigger, &vrRightTrigger))
    {
        pad.wButtons |= vrButtons;
        if (vrLeftTrigger > pad.bLeftTrigger)
            pad.bLeftTrigger = vrLeftTrigger;
        if (vrRightTrigger > pad.bRightTrigger)
            pad.bRightTrigger = vrRightTrigger;
    }

    // XInput consumers may skip processing when the packet number hasn't changed, so bump it whenever the pad we
    // report differs from the last one.
    static XINPUT_GAMEPAD lastReported{};
    static DWORD extraPackets = 0;
    if (memcmp(&pad, &lastReported, sizeof(pad)) != 0)
    {
        lastReported = pad;
        ++extraPackets;
    }
    state->dwPacketNumber += extraPackets;

    static unsigned long long nextLogMs = 0;
    unsigned long long now = GetTickCount64();
    if (now >= nextLogMs)
    {
        nextLogMs = now + 1000;
        MOHW_LOG(kLogFile, "controller 0: LX=%d LY=%d RX=%d RY=%d buttons=0x%04X packet=%lu", pad.sThumbLX,
                  pad.sThumbLY, pad.sThumbRX, pad.sThumbRY, pad.wButtons, state->dwPacketNumber);
    }
    return result;
}

DWORD WINAPI HookedCaps(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    DWORD result = g_originalCaps(userIndex, flags, caps);
    if (userIndex != 0 || !caps || result == ERROR_SUCCESS)
        return result;

    // No real pad in slot 0: describe a standard wired gamepad with every control present.
    memset(caps, 0, sizeof(*caps));
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = 0xFF;
    caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = static_cast<SHORT>(0xFFC0);
    caps->Gamepad.sThumbLY = static_cast<SHORT>(0xFFC0);
    caps->Gamepad.sThumbRX = static_cast<SHORT>(0xFFC0);
    caps->Gamepad.sThumbRY = static_cast<SHORT>(0xFFC0);
    return ERROR_SUCCESS;
}

bool HookExport(HMODULE module, const char* name, void* detour, void** original, void** outAddress)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (!target)
    {
        MOHW_LOG(kLogFile, "GetProcAddress(%s) FAILED", name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK)
        s = MH_EnableHook(target);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "hooking %s @ %p FAILED: %s", name, target, MH_StatusToString(s));
        return false;
    }
    *outAddress = target;
    MOHW_LOG(kLogFile, "%s hooked @ %p", name, target);
    return true;
}

} // namespace

bool InstallXInputHook()
{
    // The game uses xinput1_3.dll. Loading it here (if the game hasn't yet) is harmless -- the game loads the same
    // DLL itself -- and lets the hook be in place before the game's first controller poll.
    HMODULE xinput = LoadLibraryA("xinput1_3.dll");
    if (!xinput)
    {
        MOHW_LOG(kLogFile, "LoadLibrary(xinput1_3.dll) FAILED (GetLastError=%lu) -- no virtual gamepad", GetLastError());
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    if (!HookExport(xinput, "XInputGetState", reinterpret_cast<void*>(&Hooked), reinterpret_cast<void**>(&g_original),
                    &g_hookAddress))
        return false;
    // Optional: the game may never ask for capabilities, and the state hook alone already reports a connected pad.
    HookExport(xinput, "XInputGetCapabilities", reinterpret_cast<void*>(&HookedCaps),
               reinterpret_cast<void**>(&g_originalCaps), &g_capsHookAddress);
    return true;
}

void RemoveXInputHook()
{
    for (void** address : {&g_hookAddress, &g_capsHookAddress})
    {
        if (*address)
        {
            MH_DisableHook(*address);
            MH_RemoveHook(*address);
            *address = nullptr;
        }
    }
}

} // namespace mohw
