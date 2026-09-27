#include "vr_input.h"

#include "openvr_types.h"
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
    { "name": "/actions/main/in/TwoHandGrip", "type": "boolean" },
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
      "/actions/main/in/TwoHandGrip": "Two-handed grip - off hand",
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
          "path": "/user/hand/left/input/grip",
          "mode": "button",
          "inputs": { "click": { "output": "/actions/main/in/TwoHandGrip" } }
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
uint64_t g_twoHandAction = 0;
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

bool ReadHandPose(uint64_t action, PoseActionData* data)
{
    *data = PoseActionData{};
    int err = g_getPose(g_input, action, g_trackingUniverse, data, sizeof(*data), 0);
    return err == 0 && data->active && data->pose.poseIsValid;
}

Vec3 PoseColumn(const PoseActionData& data, int col)
{
    Vec3 v{};
    v.x = data.pose.deviceToAbsoluteTracking[0][col];
    v.y = data.pose.deviceToAbsoluteTracking[1][col];
    v.z = data.pose.deviceToAbsoluteTracking[2][col];
    return v;
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    Vec3 out{};
    out.x = a.y * b.z - a.z * b.y;
    out.y = a.z * b.x - a.x * b.z;
    out.z = a.x * b.y - a.y * b.x;
    return out;
}

bool Normalize(Vec3* v)
{
    float len = sqrtf(VecDot(*v, *v));
    if (len < 1e-4f)
        return false;
    *v = VecScale(*v, 1.0f / len);
    return true;
}

// TWO-HANDED AIM (2026-09-26): pressing the off-hand grip (TwoHandGrip) with the left hand near the rifle -- within
// TwoHandGrabRadius of the line running forward from the right hand, between 5 cm and TwoHandReach along it -- aims
// along the right-to-left-hand vector for as long as the grip is held. Done here, on the right hand's published
// orientation, so the shot direction and the gun on the controller both follow without knowing about it. The right
// hand's up axis is kept (made perpendicular to the new forward), so tilting the gun still rolls it.
// Pose matrices: columns 0-2 = right/up/back (+Z is back, -Z forward), column 3 = position, tracking space.
bool g_twoHanded = false; // submit thread only

void UpdateTwoHanded(bool gripHeld, bool rightTracked, bool leftTracked, const PoseActionData& right,
                     const PoseActionData& left)
{
    bool was = g_twoHanded;
    if (!gripHeld || !rightTracked || !leftTracked)
    {
        g_twoHanded = false;
    }
    else if (!g_twoHanded)
    {
        Vec3 forward = VecScale(PoseColumn(right, 2), -1.0f);
        Vec3 toLeft = VecSub(PoseColumn(left, 3), PoseColumn(right, 3));
        float along = VecDot(toLeft, forward);
        Vec3 perp = VecSub(toLeft, VecScale(forward, along));
        float perpDist = sqrtf(VecDot(perp, perp));
        float radius = GetTwoHandGrabRadius(), reach = GetTwoHandReach();
        g_twoHanded = along >= 0.05f && along <= reach && perpDist <= radius;
        static int missLines = 0;
        if (!g_twoHanded && missLines < 20)
        {
            ++missLines;
            MOHW_LOG(kLogFile,
                      "two-handed grip pressed but left hand not on the rifle: %.2f m along the barrel (0.05..%.2f), "
                      "%.2f m off it (max %.2f)",
                      along, reach, perpDist, radius);
        }
    }
    if (g_twoHanded != was)
        MOHW_LOG(kLogFile, "two-handed aim %s", g_twoHanded ? "ON" : "off");
}

// The right hand's orientation aimed at the left hand, or false if the hands are too close to give a direction.
bool TwoHandedOrientation(const PoseActionData& right, const PoseActionData& left, Quat* out)
{
    Vec3 forward = VecSub(PoseColumn(left, 3), PoseColumn(right, 3));
    if (sqrtf(VecDot(forward, forward)) < 0.08f || !Normalize(&forward))
        return false;
    Vec3 back = VecScale(forward, -1.0f);
    Vec3 up = PoseColumn(right, 1);
    up = VecSub(up, VecScale(forward, VecDot(up, forward)));
    if (!Normalize(&up))
    {
        up = Vec3{};
        up.y = 1.0f;
        up = VecSub(up, VecScale(forward, VecDot(up, forward)));
        if (!Normalize(&up))
            return false;
    }
    Vec3 rightAxis = Cross(up, back);
    if (!Normalize(&rightAxis))
        return false;
    up = Cross(back, rightAxis);
    float m[3][4] = {};
    const Vec3* cols[3] = {&rightAxis, &up, &back};
    for (int c = 0; c < 3; ++c)
    {
        m[0][c] = cols[c]->x;
        m[1][c] = cols[c]->y;
        m[2][c] = cols[c]->z;
    }
    *out = MatrixToQuat(m);
    return true;
}

// Publishes one hand's pose, or marks it untracked. orientationOverride replaces the pose's own orientation.
void PublishHandPose(const PoseActionData& data, bool tracked, bool rightHand, UINT64 frameCounter,
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
    if (rightHand)
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
    ok = GetNamedHandle(getActionHandle, "/actions/main/in/TwoHandGrip", &g_twoHandAction) && ok;
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
    PoseActionData rightPose{}, leftPose{};
    bool rightTracked = ReadHandPose(g_rightAimAction, &rightPose);
    bool leftTracked = ReadHandPose(g_leftAimAction, &leftPose);

    DigitalActionData twoHand{};
    int twoHandErr = g_getDigital(g_input, g_twoHandAction, &twoHand, sizeof(twoHand), 0);
    UpdateTwoHanded(twoHandErr == 0 && twoHand.active && twoHand.state, rightTracked, leftTracked, rightPose, leftPose);
    Quat twoHandedQ{};
    bool useTwoHanded = g_twoHanded && TwoHandedOrientation(rightPose, leftPose, &twoHandedQ);

    Vec3 rightForward{}, leftForward{};
    PublishHandPose(rightPose, rightTracked, true, frameCounter, useTwoHanded ? &twoHandedQ : nullptr, &rightForward);
    PublishHandPose(leftPose, leftTracked, false, frameCounter, nullptr, &leftForward);

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
                  "aim tracked R=%d L=%d | active fire=%d move=%d turn=%d twoHand=%d | move=(%.2f,%.2f) turnX=%.2f | "
                  "right forward=(%.3f,%.3f,%.3f)%s | errs fire=%d move=%d turn=%d twoHand=%d",
                  rightTracked, leftTracked, fire.active, move.active, turn.active, twoHand.active, state.moveX,
                  state.moveY, state.turnX, rightForward.x, rightForward.y, rightForward.z,
                  useTwoHanded ? " (two-handed)" : "", fireErr, moveErr, turnErr, twoHandErr);
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
