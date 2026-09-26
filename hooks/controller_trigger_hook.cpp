#include "controller_trigger_hook.h"

#include "../openvr_direct/vr_input.h"
#include "../sdk/logging.h"

#include <windows.h>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_controllertrigger.log";

bool g_mouseDown = false;

void SendLeftMouseEvent(DWORD flag)
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

    // Fire is a SteamVR boolean action, so SteamVR applies its own press/release thresholds (adjustable in its
    // binding UI) -- no hysteresis needed here.
    if (state.fire && !g_mouseDown)
    {
        g_mouseDown = true;
        SendLeftMouseEvent(MOUSEEVENTF_LEFTDOWN);
        MOHW_LOG(kLogFile, "Fire pressed -> LEFTDOWN");
    }
    else if (!state.fire && g_mouseDown)
    {
        g_mouseDown = false;
        SendLeftMouseEvent(MOUSEEVENTF_LEFTUP);
        MOHW_LOG(kLogFile, "Fire released -> LEFTUP");
    }
}

} // namespace mohw
