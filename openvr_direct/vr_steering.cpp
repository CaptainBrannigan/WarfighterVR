#include "vr_steering.h"

#include "vr_overlay.h"
#include "../hooks/aiming_controller_hook.h"
#include "../hooks/driving_camera_hook.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cmath>

namespace mohw::openvr_direct {
namespace {

constexpr const char* kLogFile = "mohwvr_steering.log";
constexpr float kPi = 3.14159265f;
constexpr float kDegToRad = kPi / 180.0f;
// A hand closer than this to the wheel's axis has no meaningful angle on it: its turn is held until it moves out.
constexpr float kMinRadius = 0.06f;

// Where the wheel was set (submit thread only): the head's position and heading at the level's first head pose or the
// latest recenter.
struct Reference
{
    bool valid = false;
    unsigned generation = 0;
    Vec3 position;
    float yaw = 0.0f; // rotation about +y taking (0,0,-1) to the head's heading
};
Reference g_ref;

struct WheelFrame
{
    Vec3 centre, right, up; // up = towards the top of the wheel, in its (tilted) plane
};

// Per hand (submit thread only). continuous = the hand's angle around the wheel, unwrapped (it can pass 180 degrees);
// anchor = its value when the set of hands on the wheel last changed.
struct HandOnWheel
{
    bool held = false;
    bool haveAngle = false;
    float lastRaw = 0.0f;
    float continuous = 0.0f;
    float anchor = 0.0f;
};
HandOnWheel g_hands[2];
bool g_contributing[2] = {false, false};
float g_wheel = 0.0f;       // radians, + = turned right (clockwise, as the driver sees it)
float g_anchorWheel = 0.0f; // g_wheel when the set of hands last changed

std::atomic<bool> g_active{false};
std::atomic<float> g_stick{0.0f};

Vec3 MakeVec(float x, float y, float z)
{
    Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

WheelFrame ComputeFrame()
{
    float forwardM = 0.0f, downM = 0.0f, rightM = 0.0f, tiltDeg = 0.0f;
    GetDrivingWheelPlacement(&forwardM, &downM, &rightM, &tiltDeg);
    float s = sinf(g_ref.yaw), c = cosf(g_ref.yaw);
    Vec3 forward = MakeVec(-s, 0.0f, -c); // the heading's (0,0,-1)
    Vec3 right = MakeVec(c, 0.0f, -s);
    Vec3 up = MakeVec(0.0f, 1.0f, 0.0f);
    float tilt = tiltDeg * kDegToRad;
    WheelFrame f;
    f.centre = VecAdd(VecAdd(VecAdd(g_ref.position, VecScale(forward, forwardM)), VecScale(up, -downM)),
                      VecScale(right, rightM));
    f.right = right;
    f.up = VecAdd(VecScale(up, cosf(tilt)), VecScale(forward, sinf(tilt))); // top leaning away from the driver
    return f;
}

// The hand's angle on the wheel: 0 at the top, + clockwise as the driver sees it. False near the axis.
bool HandAngle(const HandInput& hand, const WheelFrame& f, float* angle)
{
    Vec3 d = VecSub(MakeVec(hand.pose[0][3], hand.pose[1][3], hand.pose[2][3]), f.centre);
    float x = VecDot(d, f.right), y = VecDot(d, f.up);
    if (x * x + y * y < kMinRadius * kMinRadius)
        return false;
    *angle = atan2f(x, y);
    return true;
}

const char* HandName(int h)
{
    return h == kLeftHand ? "left" : "right";
}

} // namespace

void UpdateSteering(const HandInput hands[2], bool haveHead, const float headPos[3], const Quat& headOrientation)
{
    unsigned generation = GetRecenterGeneration();
    if (haveHead && (!g_ref.valid || generation != g_ref.generation))
    {
        Vec3 back = MakeVec(0.0f, 0.0f, -1.0f);
        Vec3 f = QuatRotateVector(headOrientation, back);
        g_ref.valid = true;
        g_ref.generation = generation;
        g_ref.position = MakeVec(headPos[0], headPos[1], headPos[2]);
        g_ref.yaw = atan2f(-f.x, -f.z);
        WheelFrame frame = ComputeFrame();
        MOHW_LOG(kLogFile, "wheel set: head (%.2f, %.2f, %.2f), heading %.1f deg -> centre (%.2f, %.2f, %.2f)", headPos[0],
                  headPos[1], headPos[2], g_ref.yaw / kDegToRad, frame.centre.x, frame.centre.y, frame.centre.z);
    }

    bool enabled = GetDrivingSteeringWheel() && g_ref.valid;
    WheelFrame frame{};
    if (enabled)
        frame = ComputeFrame(); // every update: the placement settings move the wheel live

    // Each hand's angle, unwrapped while held.
    bool held[2] = {false, false};
    for (int h = 0; h < 2; ++h)
    {
        HandOnWheel& w = g_hands[h];
        held[h] = enabled && hands[h].tracked && hands[h].grip;
        if (held[h] != w.held)
            MOHW_LOG(kLogFile, "%s hand %s the wheel", HandName(h), held[h] ? "grabbed" : "released");
        if (!held[h])
        {
            w.held = false;
            w.haveAngle = false;
            continue;
        }
        float raw = 0.0f;
        if (HandAngle(hands[h], frame, &raw))
        {
            w.continuous = w.haveAngle ? w.continuous + WrapAngleSigned(raw - w.lastRaw) : raw;
            w.lastRaw = raw;
            w.haveAngle = true;
        }
        w.held = true;
    }

    // Re-anchor whenever the set of hands turning the wheel changes, so a grab or a release never makes it jump.
    bool contributing[2] = {held[0] && g_hands[0].haveAngle, held[1] && g_hands[1].haveAngle};
    if (!held[0] && !held[1])
        g_wheel = 0.0f; // let go: back to centre
    if (contributing[0] != g_contributing[0] || contributing[1] != g_contributing[1])
    {
        g_anchorWheel = g_wheel;
        for (int h = 0; h < 2; ++h)
            if (contributing[h])
                g_hands[h].anchor = g_hands[h].continuous;
        g_contributing[0] = contributing[0];
        g_contributing[1] = contributing[1];
    }
    int count = (contributing[0] ? 1 : 0) + (contributing[1] ? 1 : 0);
    if (count > 0)
    {
        float turn = 0.0f;
        for (int h = 0; h < 2; ++h)
            if (contributing[h])
                turn += g_hands[h].continuous - g_hands[h].anchor;
        g_wheel = g_anchorWheel + turn / static_cast<float>(count);
    }

    // Full lock is a hard stop: turning on past it doesn't wind up a turn that has to be undone first.
    float lock = GetDrivingWheelLockDeg() * kDegToRad;
    if (!(lock > 0.1f))
        lock = 90.0f * kDegToRad;
    if (fabsf(g_wheel) > lock)
    {
        g_wheel = g_wheel > 0.0f ? lock : -lock;
        g_anchorWheel = g_wheel;
        for (int h = 0; h < 2; ++h)
            if (contributing[h])
                g_hands[h].anchor = g_hands[h].continuous;
    }

    bool active = held[0] || held[1];
    g_stick.store(g_wheel / lock, std::memory_order_relaxed);
    g_active.store(active, std::memory_order_relaxed);

    // Drawn while driving in stereo -- not over the menu (paused) or a loading screen / movie (the floating screen).
    float radius = 0.0f, thickness = 0.0f, opacity = 0.0f;
    GetDrivingWheelLook(&radius, &thickness, &opacity);
    bool drawn = enabled && DrivingStereoActive() && !DrivingSimulationPaused();
    UpdateWheelOverlay(drawn, frame.centre, frame.right, frame.up, g_wheel, radius, thickness, opacity);

    static unsigned long long nextLogMs = 0;
    unsigned long long now = GetTickCount64();
    if (active && now >= nextLogMs)
    {
        nextLogMs = now + 1000;
        MOHW_LOG(kLogFile, "wheel %.1f deg (stick %.2f), hands on it: left %d right %d", g_wheel / kDegToRad,
                  g_wheel / lock, contributing[kLeftHand] ? 1 : 0, contributing[kRightHand] ? 1 : 0);
    }
}

bool GetSteeringWheelStick(float* x)
{
    if (!g_active.load(std::memory_order_relaxed))
        return false;
    *x = g_stick.load(std::memory_order_relaxed);
    return true;
}

} // namespace mohw::openvr_direct
