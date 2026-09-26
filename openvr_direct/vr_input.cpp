#include "vr_input.h"

#include "openvr_types.h"
#include "../hooks/companion_bridge.h"
#include "../sdk/logging.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace mohw::openvr_direct {
namespace {

constexpr const char* kLogFile = "mohwvr_vrinput.log";

constexpr const char* kManifestFileName = "mohwvr_actions.json";
constexpr const char* kTouchBindingsFileName = "mohwvr_bindings_oculus_touch.json";

constexpr const char* kActionManifestJson = R"({
  "default_bindings": [
    { "controller_type": "oculus_touch", "binding_url": "mohwvr_bindings_oculus_touch.json" }
  ],
  "actions": [
    { "name": "/actions/main/in/RightHandAim", "type": "pose" },
    { "name": "/actions/main/in/LeftHandAim", "type": "pose" },
    { "name": "/actions/main/in/Fire", "type": "boolean" },
    { "name": "/actions/main/in/Move", "type": "vector2" },
    { "name": "/actions/main/in/Turn", "type": "vector2" }
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
      "/actions/main/in/Fire": "Fire",
      "/actions/main/in/Move": "Move",
      "/actions/main/in/Turn": "Turn"
    }
  ]
}
)";

constexpr const char* kTouchBindingsJson = R"({
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
        {
          "path": "/user/hand/right/input/trigger",
          "mode": "trigger",
          "inputs": { "click": { "output": "/actions/main/in/Fire" } }
        },
        {
          "path": "/user/hand/left/input/joystick",
          "mode": "joystick",
          "inputs": { "position": { "output": "/actions/main/in/Move" } }
        },
        {
          "path": "/user/hand/right/input/joystick",
          "mode": "joystick",
          "inputs": { "position": { "output": "/actions/main/in/Turn" } }
        }
      ]
    }
  }
}
)";

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
uint64_t g_fireAction = 0;
uint64_t g_moveAction = 0;
uint64_t g_turnAction = 0;

std::mutex g_stateMutex;
VrActionState g_state;
bool g_haveState = false;

// Lock-free copies of the stick axes for GetVrSticks, which runs inside the game's own axis reads (many per frame).
std::atomic<bool> g_haveSticks{false};
std::atomic<float> g_moveX{0.0f}, g_moveY{0.0f}, g_turnX{0.0f};

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

bool GetNamedHandle(PFN_GetHandle fn, const char* name, uint64_t* out)
{
    int err = fn(g_input, name, out);
    MOHW_LOG(kLogFile, "handle %s = %llu, err=%d", name, static_cast<unsigned long long>(*out), err);
    return err == 0;
}

// Publishes one hand's pose, or marks it untracked. Returns whether it was tracked (for the periodic log).
bool PublishHandPose(uint64_t action, bool rightHand, UINT64 frameCounter, Vec3* outForward)
{
    PoseActionData data{};
    int err = g_getPose(g_input, action, g_trackingUniverse, &data, sizeof(data), 0);
    mohwvr::ipc::ControllerPoseBlock block{};
    block.frameCounter = frameCounter;
    bool tracked = err == 0 && data.active && data.pose.poseIsValid;
    if (tracked)
    {
        Quat q = MatrixToQuat(data.pose.deviceToAbsoluteTracking);
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
    if (rightHand)
        SetRightControllerPoseOverride(block);
    else
        SetLeftControllerPoseOverride(block);
    return tracked;
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
    if (!WriteTextFile(manifestPath, kActionManifestJson) || !WriteTextFile(bindingsPath, kTouchBindingsJson))
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

    int err = setManifest(input, manifestPath.c_str());
    MOHW_LOG(kLogFile, "SetActionManifestPath(%s) err=%d, tracking universe=%d", manifestPath.c_str(), err,
              trackingUniverse);
    if (err != 0)
        return false;

    bool ok = GetNamedHandle(getSetHandle, "/actions/main", &g_mainSet);
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/RightHandAim", &g_rightAimAction) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/LeftHandAim", &g_leftAimAction) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/Fire", &g_fireAction) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/Move", &g_moveAction) && ok;
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/Turn", &g_turnAction) && ok;
    if (!ok)
    {
        g_updateActionState = nullptr; // UpdateVrInput no-ops
        return false;
    }
    MOHW_LOG(kLogFile, "SteamVR Input ready");
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
    Vec3 rightForward{}, leftForward{};
    bool rightTracked = PublishHandPose(g_rightAimAction, true, frameCounter, &rightForward);
    bool leftTracked = PublishHandPose(g_leftAimAction, false, frameCounter, &leftForward);

    DigitalActionData fire{};
    AnalogActionData move{}, turn{};
    int fireErr = g_getDigital(g_input, g_fireAction, &fire, sizeof(fire), 0);
    int moveErr = g_getAnalog(g_input, g_moveAction, &move, sizeof(move), 0);
    int turnErr = g_getAnalog(g_input, g_turnAction, &turn, sizeof(turn), 0);

    VrActionState state{};
    state.fire = fireErr == 0 && fire.active && fire.state;
    if (moveErr == 0 && move.active)
    {
        state.moveX = move.x;
        state.moveY = move.y;
    }
    if (turnErr == 0 && turn.active)
        state.turnX = turn.x;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state = state;
        g_haveState = true;
    }
    g_moveX.store(state.moveX, std::memory_order_relaxed);
    g_moveY.store(state.moveY, std::memory_order_relaxed);
    g_turnX.store(state.turnX, std::memory_order_relaxed);
    g_haveSticks.store(true, std::memory_order_release);

    if (fireErr == 0 && fire.changed)
        MOHW_LOG(kLogFile, "Fire %s", fire.state ? "PRESSED" : "released");

    // Once a second: which actions SteamVR actually has bound (active=0 means no binding reached it), plus the
    // right hand's forward vector for checking the aim direction's axes live.
    static unsigned long long nextLogMs = 0;
    unsigned long long now = GetTickCount64();
    if (now >= nextLogMs)
    {
        nextLogMs = now + 1000;
        MOHW_LOG(kLogFile,
                  "aim tracked R=%d L=%d | active fire=%d move=%d turn=%d | move=(%.2f,%.2f) turnX=%.2f | right "
                  "forward=(%.3f,%.3f,%.3f) | errs fire=%d move=%d turn=%d",
                  rightTracked, leftTracked, fire.active, move.active, turn.active, state.moveX, state.moveY,
                  state.turnX, rightForward.x, rightForward.y, rightForward.z, fireErr, moveErr, turnErr);
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

bool GetVrSticks(float* moveX, float* moveY, float* turnX)
{
    if (!g_haveSticks.load(std::memory_order_acquire))
        return false;
    *moveX = g_moveX.load(std::memory_order_relaxed);
    *moveY = g_moveY.load(std::memory_order_relaxed);
    *turnX = g_turnX.load(std::memory_order_relaxed);
    return true;
}

} // namespace mohw::openvr_direct
