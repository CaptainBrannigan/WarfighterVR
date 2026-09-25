#include "controller_trigger_hook.h"

#include "companion_bridge.h"
#include "../sdk/logging.h"

#include <windows.h>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_controllertrigger.log";

// Simple hysteresis so a trigger value hovering right at the edge doesn't
// chatter between down/up every frame -- press past 0.6 to register a
// click, release back below 0.4 to end it.
constexpr float kPressThreshold = 0.6f;
constexpr float kReleaseThreshold = 0.4f;

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
    mohwvr::ipc::ControllerPoseBlock pose{};
    if (!GetRightControllerPose(&pose))
        return;

    if (!g_mouseDown && pose.triggerValue >= kPressThreshold)
    {
        g_mouseDown = true;
        SendLeftMouseEvent(MOUSEEVENTF_LEFTDOWN);
        MOHW_LOG(kLogFile, "trigger=%.3f -> LEFTDOWN", pose.triggerValue);
    }
    else if (g_mouseDown && pose.triggerValue <= kReleaseThreshold)
    {
        g_mouseDown = false;
        SendLeftMouseEvent(MOUSEEVENTF_LEFTUP);
        MOHW_LOG(kLogFile, "trigger=%.3f -> LEFTUP", pose.triggerValue);
    }
}

} // namespace mohw
