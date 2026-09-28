#include "controller_trigger_hook.h"

#include "../openvr_direct/vr_input.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"

#include <windows.h>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_controllertrigger.log";

bool g_mouseDown = false;
bool g_adsDown = false;

void SendMouseEvent(DWORD flag)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    SendInput(1, &input, sizeof(INPUT));
}

} // namespace

void UpdateControllerTriggerMouseInput()
{
    openvr_direct::VrActionState state{};
    if (!openvr_direct::GetVrActionState(&state))
        return;
    // Default: the triggers go to the virtual pad's RT / LT instead (openvr_direct/vr_input.cpp). Buttons still held
    // when the setting flips are released below, since the state is then treated as released.
    if (!GetFireViaMouse())
    {
        state.fire = false;
        state.ads = false;
    }

    // Fire is a SteamVR boolean action, so SteamVR applies its own press/release thresholds (adjustable in its
    // binding UI) -- no hysteresis needed here.
    if (state.fire && !g_mouseDown)
    {
        g_mouseDown = true;
        SendMouseEvent(MOUSEEVENTF_LEFTDOWN);
        MOHW_LOG(kLogFile, "Fire pressed -> LEFTDOWN");
    }
    else if (!state.fire && g_mouseDown)
    {
        g_mouseDown = false;
        SendMouseEvent(MOUSEEVENTF_LEFTUP);
        MOHW_LOG(kLogFile, "Fire released -> LEFTUP");
    }

    // Off-hand trigger = aim down sights: the game's zoom is on the right mouse button (ConceptZoom, mouse button 1
    // in its profile). Held while the trigger is held, so it follows the game's own hold/toggle zoom setting.
    if (state.ads && !g_adsDown)
    {
        g_adsDown = true;
        SendMouseEvent(MOUSEEVENTF_RIGHTDOWN);
        MOHW_LOG(kLogFile, "ADS pressed -> RIGHTDOWN");
    }
    else if (!state.ads && g_adsDown)
    {
        g_adsDown = false;
        SendMouseEvent(MOUSEEVENTF_RIGHTUP);
        MOHW_LOG(kLogFile, "ADS released -> RIGHTUP");
    }
}

} // namespace mohw
