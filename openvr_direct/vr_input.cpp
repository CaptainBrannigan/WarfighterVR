#include "vr_input.h"

#include "openvr_types.h"
#include "vr_hands.h"
#include "vr_overlay.h"
#include "../hooks/aiming_controller_hook.h"
#include "../hooks/companion_bridge.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace mohw::openvr_direct {
namespace {

constexpr const char* kLogFile = "mohwvr_vrinput.log";

constexpr const char* kManifestFileName = "mohwvr_actions.json";
constexpr const char* kTouchBindingsFileName = "mohwvr_bindings_oculus_touch.json";

// GAME ACTIONS (2026-09-28): one SteamVR boolean action per game keybinding (a GstKeyBinding concept from the game's
// own profile, infantry or infantrySP context), holding that action's key for as long as the button is held -- so
// everything the game can do from the keyboard can be bound to the controllers in SteamVR's binding UI. Keys are
// looked up from the profile at launch (vr_hands.cpp), so rebinding in the game's own menu carries over. Menu select
// and back aren't in the profile (the menus use fixed Enter / Escape), so those two send fixed keys. Movement (WASD)
// and the arrow-key actions are left out on purpose; fire and zoom are mouse-bound and have their own actions.
struct GameAction
{
    const char* name;        // SteamVR action /actions/main/in/Game<name>
    const char* concept;     // GstKeyBinding concept whose keyboard key is sent, or nullptr for fixedKey
    int fixedKey;            // DirectInput scancode sent when concept is nullptr
    const char* label;
    const char* defaultPath; // the oculus_touch input bound by default, or nullptr = unbound by default
    const char* defaultMode; // "button", or "joystick" for a stick click
    const char* defaultInput; // "click" or "long" (long press)
};
constexpr int kDikEscape = 0x01;
constexpr int kDikEnter = 0x1C;
const GameAction kGameActions[] = {
    {"Jump", "ConceptJump", -1, "Jump", nullptr, nullptr, nullptr},
    {"MenuSelect", nullptr, kDikEnter, "Menu select (Enter)", nullptr, nullptr, nullptr},
    {"Reload", "ConceptReload", -1, "Reload", nullptr, nullptr, nullptr},
    {"Interact", "ConceptInteract", -1, "Interact / use", nullptr, nullptr, nullptr},
    {"MenuBack", nullptr, kDikEscape, "Menu back / pause (Escape)", nullptr, nullptr, nullptr},
    {"Sprint", "ConceptSprint", -1, "Sprint", nullptr, nullptr, nullptr},
    {"Crouch", "ConceptCrouch", -1, "Crouch", nullptr, nullptr, nullptr},
    {"MeleeAttack", "ConceptMeleeAttack", -1, "Melee", nullptr, nullptr, nullptr},
    {"ThrowGrenade", "ConceptThrowGrenade", -1, "Throw grenade", nullptr, nullptr, nullptr},
    {"SelectInventoryItem1", "ConceptSelectInventoryItem1", -1, "Select primary weapon", nullptr, nullptr, nullptr},
    {"SelectInventoryItem2", "ConceptSelectInventoryItem2", -1, "Select secondary weapon", nullptr, nullptr, nullptr},
    {"ToggleLTLM", "ConceptToggleLTLM", -1, "Toggle LTLM designator", nullptr, nullptr, nullptr},
    {"SwitchToGrenadeLauncher", "ConceptSwitchToGrenadeLauncher", -1, "Grenade launcher", nullptr, nullptr, nullptr},
    {"Prone", "ConceptProne", -1, "Prone", nullptr, nullptr, nullptr},
    {"SprintSlide", "ConceptSprintSlide", -1, "Sprint slide", nullptr, nullptr, nullptr},
    {"PickUp", "ConceptPickUp", -1, "Pick up", nullptr, nullptr, nullptr},
    {"ChangeVehicle", "ConceptChangeVehicle", -1, "Change vehicle seat", nullptr, nullptr, nullptr},
    {"CycleFireMode", "ConceptCycleFireMode", -1, "Cycle fire mode", nullptr, nullptr, nullptr},
    {"ToggleWeaponLight", "ConceptToggleWeaponLight", -1, "Toggle weapon light", nullptr, nullptr, nullptr},
    {"ClassAbility", "ConceptClassAbility", -1, "Class ability", nullptr, nullptr, nullptr},
    {"ShowHAG", "ConceptShowHAG", -1, "Show HAG", nullptr, nullptr, nullptr},
    {"Spot", "ConceptSpot", -1, "Spot", nullptr, nullptr, nullptr},
    {"BreathControl", "ConceptBreathControl", -1, "Hold breath", nullptr, nullptr, nullptr},
    {"PeekAndLean", "ConceptPeekAndLean", -1, "Peek and lean", nullptr, nullptr, nullptr},
    {"PCPeekLeft", "ConceptPCPeekLeft", -1, "Peek left", nullptr, nullptr, nullptr},
    {"PCPeekRight", "ConceptPCPeekRight", -1, "Peek right", nullptr, nullptr, nullptr},
};
constexpr int kGameActionCount = static_cast<int>(sizeof(kGameActions) / sizeof(kGameActions[0]));

// PAD ACTIONS (2026-09-28): the virtual gamepad's buttons as SteamVR actions, fed into hooks/xinput_hook.cpp's pad.
// The game maps pad buttons to actions itself, per context (infantry, vehicles, the Apache, spectating, and the
// menus' own pad navigation), from the gamepad bindings in its profile (type=2 entries; 5 = A = jump, 6 = X =
// reload/use, 7 = B = crouch, 4 = Y = next weapon, 10/11 = stick clicks, 16/17 = LB/RB, 14/15 = LT/RT). Default Touch
// layout is 1:1 (A=A, B=B, X=X, Y=Y, stick clicks), the D-pad on double presses, Start on a long Y; LB, RB and Back
// are unbound by default. Triggers aren't here: the weapon hand's trigger is RT and the other hand's LT.
struct PadAction
{
    const char* name; // SteamVR action /actions/main/in/Pad<name>
    const char* label;
    unsigned short mask; // XINPUT_GAMEPAD_* bit
    const char* defaultPath;
    const char* defaultMode;
    const char* defaultInput; // "click", "double" (double press), "long" (long press)
};
const PadAction kPadActions[] = {
    {"A", "A", 0x1000, "/user/hand/right/input/a", "button", "click"},
    {"B", "B", 0x2000, "/user/hand/right/input/b", "button", "click"},
    {"X", "X", 0x4000, "/user/hand/left/input/x", "button", "click"},
    {"Y", "Y", 0x8000, "/user/hand/left/input/y", "button", "click"},
    {"LeftStick", "Left stick click", 0x0040, "/user/hand/left/input/joystick", "joystick", "click"},
    {"RightStick", "Right stick click", 0x0080, "/user/hand/right/input/joystick", "joystick", "click"},
    {"DpadUp", "D-pad up", 0x0001, "/user/hand/left/input/y", "button", "double"},
    {"DpadDown", "D-pad down", 0x0002, "/user/hand/right/input/a", "button", "double"},
    {"DpadLeft", "D-pad left", 0x0004, "/user/hand/left/input/x", "button", "double"},
    {"DpadRight", "D-pad right", 0x0008, "/user/hand/right/input/b", "button", "double"},
    {"Start", "Start / menu", 0x0010, "/user/hand/left/input/y", "button", "long"},
    {"Back", "Back / view", 0x0020, nullptr, nullptr, nullptr},
    // Unbound by default: the grip can't take a double press (see BuildTouchBindings).
    {"LB", "Left bumper", 0x0100, nullptr, nullptr, nullptr},
    {"RB", "Right bumper", 0x0200, nullptr, nullptr, nullptr},
};
constexpr int kPadActionCount = static_cast<int>(sizeof(kPadActions) / sizeof(kPadActions[0]));

std::string PadActionName(const PadAction& a)
{
    return std::string("/actions/main/in/Pad") + a.name;
}

std::string GameActionName(const GameAction& a)
{
    return std::string("/actions/main/in/Game") + a.name;
}

std::string BuildActionManifest()
{
    std::string s = R"({
  "default_bindings": [
    { "controller_type": "oculus_touch", "binding_url": "mohwvr_bindings_oculus_touch.json" }
  ],
  "actions": [
    { "name": "/actions/main/in/RightHandAim", "type": "pose" },
    { "name": "/actions/main/in/LeftHandAim", "type": "pose" },
    { "name": "/actions/main/in/FireRight", "type": "boolean" },
    { "name": "/actions/main/in/FireLeft", "type": "boolean" },
    { "name": "/actions/main/in/GripRight", "type": "vector1" },
    { "name": "/actions/main/in/GripLeft", "type": "vector1" },
    { "name": "/actions/main/in/Recenter", "type": "boolean" },
    { "name": "/actions/main/in/Move", "type": "vector2" },
    { "name": "/actions/main/in/Turn", "type": "vector2" })";
    for (const PadAction& a : kPadActions)
        s += ",\n    { \"name\": \"" + PadActionName(a) + "\", \"type\": \"boolean\" }";
    for (const GameAction& a : kGameActions)
        s += ",\n    { \"name\": \"" + GameActionName(a) + "\", \"type\": \"boolean\" }";
    s += R"(
  ],
  "action_sets": [
    { "name": "/actions/main", "usage": "single" }
  ],
  "localization": [
    {
      "language_tag": "en_US",
      "/actions/main": "Gameplay",
      "/actions/main/in/RightHandAim": "Right hand aim",
      "/actions/main/in/LeftHandAim": "Left hand aim",
      "/actions/main/in/FireRight": "Fire - right hand",
      "/actions/main/in/FireLeft": "Fire - left hand",
      "/actions/main/in/GripRight": "Grip - right hand (pull): holsters, two-handed, double-tap RB",
      "/actions/main/in/GripLeft": "Grip - left hand (pull): holsters, two-handed, double-tap LB",
      "/actions/main/in/Recenter": "Recenter view",
      "/actions/main/in/Move": "Move",
      "/actions/main/in/Turn": "Turn")";
    for (const PadAction& a : kPadActions)
        s += ",\n      \"" + PadActionName(a) + "\": \"Gamepad: " + a.label + "\"";
    for (const GameAction& a : kGameActions)
        s += ",\n      \"" + GameActionName(a) + "\": \"Keyboard: " + a.label + "\"";
    s += R"(
    }
  ]
}
)";
    return s;
}

// One default binding: an input of a controller source feeding an action.
struct DefaultBinding
{
    std::string path, mode, input, output;
};

// Holding a button that also has a double press: SteamVR then only reports its click once the double-press window has
// passed, as a pulse (live 2026-09-29: A/B/X/Y always exactly the 100 ms minimum hold, stick clicks held for seconds),
// so the game never saw a hold. A long press does stay true while held, and a click is not reported after it (a long
// Y = Start sent no Y), so such a source also gets its click's action on "long", after this delay: held past it, the
// button is held for real until release.
constexpr const char* kHoldLongPressDelay = "0.3";

// The default Touch binding, grouped into one source per path+mode (a button can carry several inputs, e.g. A = click
// A + double press D-pad down).
std::string BuildTouchBindings()
{
    std::vector<DefaultBinding> bindings = {
        {"/user/hand/right/input/trigger", "trigger", "click", "/actions/main/in/FireRight"},
        {"/user/hand/left/input/trigger", "trigger", "click", "/actions/main/in/FireLeft"},
        // Grips: the analog pull ("trigger" mode "pull", a vector1 action); the press and the double-tap bumpers are
        // made in UpdateGripGesture, as SteamVR can't bind a double press on the Touch grip (see there).
        {"/user/hand/right/input/grip", "trigger", "pull", "/actions/main/in/GripRight"},
        {"/user/hand/left/input/grip", "trigger", "pull", "/actions/main/in/GripLeft"},
        {"/user/hand/left/input/joystick", "joystick", "position", "/actions/main/in/Move"},
        {"/user/hand/right/input/joystick", "joystick", "position", "/actions/main/in/Turn"},
    };
    for (const PadAction& a : kPadActions)
        if (a.defaultPath)
            bindings.push_back({a.defaultPath, a.defaultMode, a.defaultInput, PadActionName(a)});
    for (const GameAction& a : kGameActions)
        if (a.defaultPath)
            bindings.push_back({a.defaultPath, a.defaultMode, a.defaultInput, GameActionName(a)});

    std::string sources;
    std::vector<bool> done(bindings.size(), false);
    for (size_t i = 0; i < bindings.size(); ++i)
    {
        if (done[i])
            continue;
        std::string inputs, clickOutput;
        bool hasDouble = false, hasLong = false;
        for (size_t j = i; j < bindings.size(); ++j)
            if (!done[j] && bindings[j].path == bindings[i].path && bindings[j].mode == bindings[i].mode)
            {
                done[j] = true;
                if (!inputs.empty())
                    inputs += ", ";
                inputs += "\"" + bindings[j].input + "\": { \"output\": \"" + bindings[j].output + "\" }";
                if (bindings[j].input == "click")
                    clickOutput = bindings[j].output;
                hasDouble = hasDouble || bindings[j].input == "double";
                hasLong = hasLong || bindings[j].input == "long";
            }
        std::string parameters;
        if (bindings[i].mode == "button" && hasDouble && !hasLong && !clickOutput.empty())
        {
            inputs += ", \"long\": { \"output\": \"" + clickOutput + "\" }"; // see kHoldLongPressDelay
            parameters = std::string(",\n          \"parameters\": { \"long_press_delay\": \"") + kHoldLongPressDelay + "\" }";
        }
        if (!sources.empty())
            sources += ",\n";
        sources += "        {\n          \"path\": \"" + bindings[i].path + "\",\n          \"mode\": \"" + bindings[i].mode +
                   "\",\n          \"inputs\": { " + inputs + " }" + parameters + "\n        }";
    }

    std::string s = R"({
  "controller_type": "oculus_touch",
  "name": "MOHW VR defaults",
  "description": "Default MOHW VR bindings for Touch controllers",
  "bindings": {
    "/actions/main": {
      "poses": [
        { "output": "/actions/main/in/RightHandAim", "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/main/in/LeftHandAim", "path": "/user/hand/left/pose/tip" }
      ],
      "sources": [
)";
    s += sources;
    s += R"(
      ]
    }
  }
}
)";
    return s;
}

// Plain-data stand-ins for IVRInput's structs (openvr.h, IVRInput section). That section has no #pragma pack, so
// they use natural alignment: the 64-bit handles sit at offset 8. SteamVR also checks the size we pass.
struct ActiveActionSet
{
    uint64_t actionSet;
    uint64_t restrictedToDevice;
    uint64_t secondaryActionSet;
    uint32_t padding;
    int32_t priority;
};
static_assert(sizeof(ActiveActionSet) == 32, "must match vr::VRActiveActionSet_t");

struct DigitalActionData
{
    bool active;
    uint64_t activeOrigin;
    bool state;
    bool changed;
    float updateTime;
};
static_assert(sizeof(DigitalActionData) == 24, "must match vr::InputDigitalActionData_t");

struct AnalogActionData
{
    bool active;
    uint64_t activeOrigin;
    float x, y, z;
    float deltaX, deltaY, deltaZ;
    float updateTime;
};
static_assert(sizeof(AnalogActionData) == 48, "must match vr::InputAnalogActionData_t");

struct PoseActionData
{
    bool active;
    uint64_t activeOrigin;
    RawTrackedDevicePose pose;
};
static_assert(sizeof(PoseActionData) == 96, "must match vr::InputPoseActionData_t");

// IVRInput_011 vtable slots, counted from openvr.h's class IVRInput declaration order.
constexpr int kSetActionManifestPathIndex = 0;
constexpr int kGetActionSetHandleIndex = 1;
constexpr int kGetActionHandleIndex = 2;
constexpr int kUpdateActionStateIndex = 4;
constexpr int kGetDigitalActionDataIndex = 5;
constexpr int kGetAnalogActionDataIndex = 6;
constexpr int kGetPoseActionDataForNextFrameIndex = 8;

using PFN_SetActionManifestPath = int(__thiscall*)(void* self, const char* path);
using PFN_GetHandle = int(__thiscall*)(void* self, const char* name, uint64_t* outHandle);
using PFN_UpdateActionState = int(__thiscall*)(void* self, ActiveActionSet* sets, uint32_t sizeOfSet, uint32_t setCount);
using PFN_GetDigitalActionData = int(__thiscall*)(void* self, uint64_t action, DigitalActionData* data, uint32_t size,
                                                  uint64_t restrictToDevice);
using PFN_GetAnalogActionData = int(__thiscall*)(void* self, uint64_t action, AnalogActionData* data, uint32_t size,
                                                 uint64_t restrictToDevice);
using PFN_GetPoseActionDataForNextFrame = int(__thiscall*)(void* self, uint64_t action, int trackingUniverse,
                                                            PoseActionData* data, uint32_t size, uint64_t restrictToDevice);

// Written once on the connect thread before SubmitThreadProc starts (std::thread's constructor orders that), then
// only read on the submit thread.
void* g_input = nullptr;
int g_trackingUniverse = 1;
PFN_UpdateActionState g_updateActionState = nullptr;
PFN_GetDigitalActionData g_getDigital = nullptr;
PFN_GetAnalogActionData g_getAnalog = nullptr;
PFN_GetPoseActionDataForNextFrame g_getPose = nullptr;
uint64_t g_mainSet = 0;
uint64_t g_rightAimAction = 0;
uint64_t g_leftAimAction = 0;
uint64_t g_fireAction[2] = {0, 0}; // [kLeftHand], [kRightHand]
uint64_t g_gripAction[2] = {0, 0};
uint64_t g_recenterAction = 0;
uint64_t g_gameActionHandles[kGameActionCount] = {};
uint64_t g_padActionHandles[kPadActionCount] = {};
// Lock-free virtual pad state for GetVrPadState (buttons mask, triggers 0-255).
std::atomic<bool> g_havePad{false};
std::atomic<unsigned> g_padButtons{0};
std::atomic<unsigned> g_padLeftTrigger{0}, g_padRightTrigger{0};
bool g_gameActionDown[kGameActionCount] = {}; // submit thread only: the key is currently held
uint64_t g_moveAction = 0;
uint64_t g_turnAction = 0;

std::mutex g_stateMutex;
VrActionState g_state;
bool g_haveState = false;

// Lock-free copies of the stick axes for GetVrSticks, which runs inside the game's own axis reads (many per frame).
std::atomic<bool> g_haveSticks{false};
std::atomic<float> g_moveX{0.0f}, g_moveY{0.0f}, g_turnX{0.0f}, g_turnY{0.0f};
std::atomic<unsigned long long> g_lastFireHeldMs{0}; // GetVrLastFireHeldMs

bool WriteTextFile(const std::string& path, const char* text)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
        return false;
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    fclose(f);
    return ok;
}

// GRIPS (2026-09-29): the grip actions are the grip's analog pull (vector1, "trigger" mode "pull"), and the press and
// the double-tap are made here. SteamVR can't do it: the Touch grip is an analog "trigger"-type input and every
// button-mode input on it (click with double / held / long) is rejected ("Invalid input type button::double for path
// /user/hand/.../input/grip" in vrserver.txt), so a grip double press was never bindable.
// Pressed past kGripOnPull, released under kGripOffPull (hysteresis, so a half-held grip doesn't chatter). A press that
// starts within kGripDoubleTapMs of a short (under kGripTapMaxMs) press's release is a double-tap: that hand's bumper
// (left = LB, right = RB) is held for as long as the second press, when GripDoubleTapBumpers is on. The grip itself is
// reported straight away either way, so a double-tap is also two quick grip presses to the holsters.
constexpr float kGripOnPull = 0.70f;
constexpr float kGripOffPull = 0.55f;
constexpr unsigned long long kGripDoubleTapMs = 300;
constexpr unsigned long long kGripTapMaxMs = 300;

struct GripGesture
{
    bool down = false;
    bool bumper = false;
    unsigned long long pressedAtMs = 0;
    unsigned long long lastTapReleaseMs = 0; // release of the last short press, 0 = none
};
GripGesture g_gripGesture[2]; // submit thread only

// Returns whether the grip is down; *bumper = the double-tap bumper is held.
bool UpdateGripGesture(int hand, float pull, bool* bumper)
{
    GripGesture& g = g_gripGesture[hand];
    unsigned long long now = GetTickCount64();
    bool down = g.down ? pull > kGripOffPull : pull >= kGripOnPull;
    if (down && !g.down)
    {
        g.pressedAtMs = now;
        g.bumper = GetGripDoubleTapBumpers() && g.lastTapReleaseMs != 0 && now - g.lastTapReleaseMs <= kGripDoubleTapMs;
        g.lastTapReleaseMs = 0;
        if (g.bumper)
            MOHW_LOG(kLogFile, "grip double-tap (%s hand) -> %s", hand == kLeftHand ? "left" : "right",
                      hand == kLeftHand ? "LB" : "RB");
    }
    else if (!down && g.down)
    {
        // Only a short press that wasn't itself a double-tap's second press can start the next double-tap.
        g.lastTapReleaseMs = (!g.bumper && now - g.pressedAtMs <= kGripTapMaxMs) ? now : 0;
        g.bumper = false;
    }
    g.down = down;
    *bumper = g.bumper;
    return down;
}

bool GetNamedHandle(PFN_GetHandle fn, const char* name, uint64_t* out)
{
    int err = fn(g_input, name, out);
    MOHW_LOG(kLogFile, "handle %s = %llu, err=%d", name, static_cast<unsigned long long>(*out), err);
    return err == 0;
}

bool ReadHandPose(uint64_t action, PoseActionData* data)
{
    *data = PoseActionData{};
    int err = g_getPose(g_input, action, g_trackingUniverse, data, sizeof(*data), 0);
    return err == 0 && data->active && data->pose.poseIsValid;
}

// Publishes one hand's pose, or marks it untracked. aimSlot = the "right controller" slot (the weapon hand, which the
// shot and gun hooks read); otherwise the "left" (off hand) slot. orientationOverride replaces the pose's own
// orientation.
void PublishHandPose(const PoseActionData& data, bool tracked, bool aimSlot, UINT64 frameCounter,
                     const Quat* orientationOverride, Vec3* outForward)
{
    mohwvr::ipc::ControllerPoseBlock block{};
    block.frameCounter = frameCounter;
    if (tracked)
    {
        Quat q = orientationOverride ? *orientationOverride : MatrixToQuat(data.pose.deviceToAbsoluteTracking);
        block.ready = 1;
        block.orientationX = q.x;
        block.orientationY = q.y;
        block.orientationZ = q.z;
        block.orientationW = q.w;
        block.positionX = data.pose.deviceToAbsoluteTracking[0][3];
        block.positionY = data.pose.deviceToAbsoluteTracking[1][3];
        block.positionZ = data.pose.deviceToAbsoluteTracking[2][3];
        Vec3 localForward{};
        localForward.z = 1.0f;
        *outForward = QuatRotateVector(q, localForward);
    }
    if (aimSlot)
        SetRightControllerPoseOverride(block);
    else
        SetLeftControllerPoseOverride(block);
}

} // namespace

bool InitVrInput(void* input, int trackingUniverse)
{
    if (!input)
        return false;
    g_input = input;
    g_trackingUniverse = trackingUniverse;

    std::string manifestPath = LogFilePath(kManifestFileName);
    std::string bindingsPath = LogFilePath(kTouchBindingsFileName);
    if (!WriteTextFile(manifestPath, BuildActionManifest().c_str()) ||
        !WriteTextFile(bindingsPath, BuildTouchBindings().c_str()))
    {
        MOHW_LOG(kLogFile, "FAILED to write the action manifest/bindings next to the DLL (%s)", manifestPath.c_str());
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(input);
    auto setManifest = reinterpret_cast<PFN_SetActionManifestPath>(vtable[kSetActionManifestPathIndex]);
    auto getSetHandle = reinterpret_cast<PFN_GetHandle>(vtable[kGetActionSetHandleIndex]);
    auto getActionHandle = reinterpret_cast<PFN_GetHandle>(vtable[kGetActionHandleIndex]);
    g_updateActionState = reinterpret_cast<PFN_UpdateActionState>(vtable[kUpdateActionStateIndex]);
    g_getDigital = reinterpret_cast<PFN_GetDigitalActionData>(vtable[kGetDigitalActionDataIndex]);
    g_getAnalog = reinterpret_cast<PFN_GetAnalogActionData>(vtable[kGetAnalogActionDataIndex]);
    g_getPose = reinterpret_cast<PFN_GetPoseActionDataForNextFrame>(vtable[kGetPoseActionDataForNextFrameIndex]);

    // IPCError (7) = our end of the call timed out while vrserver was busy (live 2026-09-28: it logged the manifest as
    // received, but took 1.01 s, while the Steam client was flooding it with binding reloads). Retry, and on a timeout
    // carry on to the handle lookups anyway -- the server did take the manifest, so they normally resolve.
    constexpr int kIpcError = 7;
    constexpr int kManifestAttempts = 5;
    int err = 0;
    for (int attempt = 1; attempt <= kManifestAttempts; ++attempt)
    {
        err = setManifest(input, manifestPath.c_str());
        MOHW_LOG(kLogFile, "SetActionManifestPath(%s) err=%d (attempt %d), tracking universe=%d", manifestPath.c_str(),
                  err, attempt, trackingUniverse);
        if (err != kIpcError)
            break;
        Sleep(750);
    }
    if (err != 0 && err != kIpcError)
        return false;

    bool ok = GetNamedHandle(getSetHandle, "/actions/main", &g_mainSet);
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/RightHandAim", &g_rightAimAction) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/LeftHandAim", &g_leftAimAction) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/FireLeft", &g_fireAction[kLeftHand]) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/FireRight", &g_fireAction[kRightHand]) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/GripLeft", &g_gripAction[kLeftHand]) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/GripRight", &g_gripAction[kRightHand]) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/Recenter", &g_recenterAction) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/Move", &g_moveAction) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/Turn", &g_turnAction) && ok;
    // Game actions: one failing only loses that action, not the whole input system.
    for (int i = 0; i < kGameActionCount; ++i)
        if (!GetNamedHandle(getActionHandle, GameActionName(kGameActions[i]).c_str(), &g_gameActionHandles[i]))
            g_gameActionHandles[i] = 0;
    for (int i = 0; i < kPadActionCount; ++i)
        if (!GetNamedHandle(getActionHandle, PadActionName(kPadActions[i]).c_str(), &g_padActionHandles[i]))
            g_padActionHandles[i] = 0;
    if (!ok)
    {
        g_updateActionState = nullptr; // UpdateVrInput no-ops
        return false;
    }
    MOHW_LOG(kLogFile, "SteamVR Input ready");
    PreloadGameBindings();
    return true;
}

void UpdateVrInput()
{
    if (!g_updateActionState)
        return;

    ActiveActionSet set{};
    set.actionSet = g_mainSet;
    int err = g_updateActionState(g_input, &set, sizeof(set), 1);
    static int lastUpdateErr = 0;
    if (err != lastUpdateErr)
    {
        MOHW_LOG(kLogFile, "UpdateActionState err=%d", err);
        lastUpdateErr = err;
    }
    if (err != 0)
        return;

    static UINT64 frameCounter = 0;
    ++frameCounter;
    PoseActionData poses[2]{};
    HandInput hands[2]{};
    const uint64_t poseActions[2] = {g_leftAimAction, g_rightAimAction};
    bool gripBumper[2] = {false, false}; // a grip double-tap held: LB (left) / RB (right), see UpdateGripGesture
    for (int h = 0; h < 2; ++h)
    {
        hands[h].tracked = ReadHandPose(poseActions[h], &poses[h]);
        memcpy(hands[h].pose, poses[h].pose.deviceToAbsoluteTracking, sizeof(hands[h].pose));
        DigitalActionData fire{};
        AnalogActionData gripPull{};
        bool gripReadOk =
            g_getAnalog(g_input, g_gripAction[h], &gripPull, sizeof(gripPull), 0) == 0 && gripPull.active;
        hands[h].grip = UpdateGripGesture(h, gripReadOk ? gripPull.x : 0.0f, &gripBumper[h]);
        hands[h].trigger =
            g_getDigital(g_input, g_fireAction[h], &fire, sizeof(fire), 0) == 0 && fire.active && fire.state;
    }

    mohwvr::ipc::HeadPoseBlock head{};
    bool haveHead = GetHeadPose(&head);
    float headPos[3] = {head.positionX, head.positionY, head.positionZ};
    Quat headQ{head.orientationX, head.orientationY, head.orientationZ, head.orientationW};
    HandsResult result{};
    UpdateHands(hands, haveHead, headPos, headQ, &result);

    // The weapon hand's pose goes in the "right controller" (aim) slot everything downstream reads -- the shot
    // direction and origin, the gun on the controller -- and the other hand in the left slot.
    int weaponHand = result.weaponHand;
    Vec3 aimForward{}, offForward{};
    PublishHandPose(poses[weaponHand], hands[weaponHand].tracked, true, frameCounter,
                    result.twoHanded ? &result.weaponOrientation : nullptr, &aimForward);
    PublishHandPose(poses[1 - weaponHand], hands[1 - weaponHand].tracked, false, frameCounter, nullptr, &offForward);

    // Sight dots, from the weapon hand's published aim orientation.
    {
        Quat aimQ = result.twoHanded ? result.weaponOrientation : MatrixToQuat(poses[weaponHand].pose.deviceToAbsoluteTracking);
        float origin[3] = {poses[weaponHand].pose.deviceToAbsoluteTracking[0][3],
                           poses[weaponHand].pose.deviceToAbsoluteTracking[1][3],
                           poses[weaponHand].pose.deviceToAbsoluteTracking[2][3]};
        bool gripOk = !GetDotsOnlyWithGrip() || result.weaponGripped;
        // No dots with the weapon attachment off: shots follow the game's aim then, not the controller.
        UpdateSightDot(hands[weaponHand].tracked && haveHead && gripOk && GetWeaponDriveEnabled(), origin, aimQ, headPos);
    }

    AnalogActionData move{}, turn{};
    int moveErr = g_getAnalog(g_input, g_moveAction, &move, sizeof(move), 0);
    int turnErr = g_getAnalog(g_input, g_turnAction, &turn, sizeof(turn), 0);

    VrActionState state{};
    state.fire = result.fire;
    state.ads = result.ads;
    if (moveErr == 0 && move.active)
    {
        state.moveX = move.x;
        state.moveY = move.y;
    }
    if (turnErr == 0 && turn.active)
    {
        state.turnX = turn.x;
        state.turnY = turn.y;
    }
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state = state;
        g_haveState = true;
    }
    g_moveX.store(state.moveX, std::memory_order_relaxed);
    g_moveY.store(state.moveY, std::memory_order_relaxed);
    g_turnX.store(state.turnX, std::memory_order_relaxed);
    g_turnY.store(state.turnY, std::memory_order_relaxed);
    g_haveSticks.store(true, std::memory_order_release);

    if (state.fire)
        g_lastFireHeldMs.store(GetTickCount64(), std::memory_order_relaxed);
    static bool lastFire = false;
    if (state.fire != lastFire)
        MOHW_LOG(kLogFile, "Fire (%s hand) %s", weaponHand == kLeftHand ? "left" : "right",
                  state.fire ? "PRESSED" : "released");
    lastFire = state.fire;

    DigitalActionData recenter{};
    if (g_getDigital(g_input, g_recenterAction, &recenter, sizeof(recenter), 0) == 0 && recenter.active &&
        recenter.changed && recenter.state)
        RequestRecenter("Recenter action (controller)");

    // Pad actions -> the virtual pad's buttons; triggers from the hand roles (weapon hand = RT = fire, off hand = LT =
    // zoom), unless fire/ADS go out as mouse clicks instead (controller_trigger_hook.cpp).
    {
        // Minimum hold: with a double press also bound, SteamVR can only report a single click once the double-press
        // window has passed, and then as a pulse as short as one input update (~11 ms) -- the game samples the pad at
        // ~30 Hz, so most single presses were missed (live: "single press needs ~4 tries, double press works").
        // Every press is held for at least kPadMinHoldMs.
        constexpr unsigned long long kPadMinHoldMs = 100;
        static unsigned long long heldUntil[kPadActionCount] = {};
        unsigned long long nowMs = GetTickCount64();
        unsigned buttons = 0;
        for (int i = 0; i < kPadActionCount; ++i)
        {
            if (!g_padActionHandles[i])
                continue;
            DigitalActionData data{};
            bool down =
                g_getDigital(g_input, g_padActionHandles[i], &data, sizeof(data), 0) == 0 && data.active && data.state;
            if (down && nowMs >= heldUntil[i])
                heldUntil[i] = nowMs + kPadMinHoldMs;
            if (down || nowMs < heldUntil[i])
                buttons |= kPadActions[i].mask;
        }
        // Grip double-taps: LB / RB, with the same minimum hold.
        constexpr unsigned short kBumperMask[2] = {0x0100, 0x0200}; // [kLeftHand] = LB, [kRightHand] = RB
        static unsigned long long bumperHeldUntil[2] = {0, 0};
        for (int h = 0; h < 2; ++h)
        {
            if (gripBumper[h] && nowMs >= bumperHeldUntil[h])
                bumperHeldUntil[h] = nowMs + kPadMinHoldMs;
            if (gripBumper[h] || nowMs < bumperHeldUntil[h])
                buttons |= kBumperMask[h];
        }
        bool triggersToPad = !GetFireViaMouse();
        static unsigned lastButtons = 0;
        if (buttons != lastButtons)
        {
            MOHW_LOG(kLogFile, "gamepad buttons 0x%04X", buttons);
            lastButtons = buttons;
        }
        g_padButtons.store(buttons, std::memory_order_relaxed);
        g_padRightTrigger.store(triggersToPad && result.fire ? 255u : 0u, std::memory_order_relaxed);
        g_padLeftTrigger.store(triggersToPad && result.ads ? 255u : 0u, std::memory_order_relaxed);
        g_havePad.store(true, std::memory_order_release);
    }

    // Game actions: hold the game's key while the action is held (released too if the action goes inactive, e.g. a
    // binding change mid-press, so no key is left stuck down).
    for (int i = 0; i < kGameActionCount; ++i)
    {
        if (!g_gameActionHandles[i])
            continue;
        DigitalActionData data{};
        bool held = g_getDigital(g_input, g_gameActionHandles[i], &data, sizeof(data), 0) == 0 && data.active &&
                    data.state;
        if (held == g_gameActionDown[i])
            continue;
        g_gameActionDown[i] = held;
        const GameAction& a = kGameActions[i];
        int dik = a.concept ? SetGameActionKey(a.concept, held) : SendGameScancode(a.fixedKey, held);
        MOHW_LOG(kLogFile, "game action %s %s -> key %d", kGameActions[i].label, held ? "pressed" : "released", dik);
    }

    // Once a second: which actions SteamVR actually has bound (active=0 means no binding reached it), plus the
    // right hand's forward vector for checking the aim direction's axes live.
    static unsigned long long nextLogMs = 0;
    unsigned long long now = GetTickCount64();
    if (now >= nextLogMs)
    {
        nextLogMs = now + 1000;
        MOHW_LOG(kLogFile,
                  "tracked L=%d R=%d | weapon hand %s%s | grip L=%d R=%d trigger L=%d R=%d | active move=%d turn=%d | "
                  "move=(%.2f,%.2f) turnX=%.2f | errs move=%d turn=%d",
                  hands[kLeftHand].tracked, hands[kRightHand].tracked, weaponHand == kLeftHand ? "left" : "right",
                  result.twoHanded ? " (two-handed)" : "", hands[kLeftHand].grip, hands[kRightHand].grip,
                  hands[kLeftHand].trigger, hands[kRightHand].trigger, move.active, turn.active, state.moveX,
                  state.moveY, state.turnX, moveErr, turnErr);
    }
}

bool GetVrActionState(VrActionState* out)
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_haveState)
        return false;
    *out = g_state;
    return true;
}

bool GetVrPadState(unsigned short* buttons, unsigned char* leftTrigger, unsigned char* rightTrigger)
{
    if (!g_havePad.load(std::memory_order_acquire))
        return false;
    *buttons = static_cast<unsigned short>(g_padButtons.load(std::memory_order_relaxed));
    *leftTrigger = static_cast<unsigned char>(g_padLeftTrigger.load(std::memory_order_relaxed));
    *rightTrigger = static_cast<unsigned char>(g_padRightTrigger.load(std::memory_order_relaxed));
    return true;
}

unsigned long long GetVrLastFireHeldMs()
{
    return g_lastFireHeldMs.load(std::memory_order_relaxed);
}

bool GetVrSticks(float* moveX, float* moveY, float* turnX, float* turnY)
{
    if (!g_haveSticks.load(std::memory_order_acquire))
        return false;
    *moveX = g_moveX.load(std::memory_order_relaxed);
    *moveY = g_moveY.load(std::memory_order_relaxed);
    *turnX = g_turnX.load(std::memory_order_relaxed);
    *turnY = g_turnY.load(std::memory_order_relaxed);
    return true;
}

} // namespace mohw::openvr_direct
