#include "head_position.h"

#include "aiming_controller_hook.h"
#include "companion_bridge.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"
#include "../sdk/vr_math.h"
#include "../shared/ipc_protocol.h"

#include <windows.h>
#include <atomic>
#include <cmath>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_headposition.log";
constexpr float kMaxOffsetMeters = 2.0f; // safety clamp on the applied offset

std::atomic<bool> g_lastPosValid{false};
std::atomic<float> g_lastPosX{0.0f}, g_lastPosY{0.0f}, g_lastPosZ{0.0f};

bool SehReadFloats16(const void* p, float* out)
{
    __try
    {
        const float* f = static_cast<const float*>(p);
        for (int i = 0; i < 16; ++i)
            out[i] = f[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SehWriteTrans(void* p, float x, float y, float z)
{
    __try
    {
        float* f = static_cast<float*>(p);
        f[12] = x;
        f[13] = y;
        f[14] = z;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void PollToggle()
{
    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0; // Pause/Break: unused by the game and every other mod hotkey
    if (down && !wasDown)
    {
        bool v = !GetHeadPositionEnabled();
        SetHeadPositionEnabled(v);
        MOHW_LOG(kLogFile, "PAUSE pressed -- head position now %s (saved to mohwvr_settings.ini)", v ? "ENABLED" : "disabled");
    }
    wasDown = down;
}

} // namespace

void ApplyHeadPosition(void* transformPtr)
{
    // POLLING TABLE DISABLED (2026-09-25), PERFORMANCE TEST -- see present_hook.cpp's identical comment. Higher
    // priority than the others: this poll ran once PER EYE per frame, not just once per Present.
    // PollToggle();
    g_lastPosValid.store(false, std::memory_order_relaxed); // set again below only if this frame actually applied an offset
    if (!transformPtr || !GetHeadPositionEnabled() || !GetHeadAimEnabled())
        return;

    float zeroYaw = 0, zeroPitch = 0, baseYaw = 0, basePitch = 0, origin[3] = {0, 0, 0};
    if (!GetHeadAimMapping(&zeroYaw, &zeroPitch, &baseYaw, &basePitch) || !GetHeadAimPositionOrigin(origin))
        return; // head-aim hasn't recentered yet

    mohwvr::ipc::HeadPoseBlock pose{};
    if (!GetHeadPose(&pose))
        return;

    if (!GetHeadAimInvertYaw())
    {
        // The derivation below (a proper rotation about the vertical axis) only holds for the confirmed-correct invert-yaw
        // setting; the un-inverted mapping is a mirror and is not supported here.
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            MOHW_LOG(kLogFile, "head position skipped: HeadAimInvertYaw is off (mapping would be a mirror, not supported)");
        }
        return;
    }

    float cam[16]{};
    if (!SehReadFloats16(transformPtr, cam))
        return;

    // Displacement since recenter in XR local space (x right, y up, z back), meters.
    float dx = pose.positionX - origin[0];
    float dy = pose.positionY - origin[1];
    float dz = pose.positionZ - origin[2];

    // XR world -> game world is a rotation about the vertical axis by theta = -(zeroYaw + baseYaw)
    // (derived from the head-aim mapping the render-pose stamp already inverts successfully; see
    // project_mohw_render_pose_stamping / this file's header). Rotation about +Y: x' = x cos + z sin, z' = -x sin + z cos.
    float theta = -(zeroYaw + baseYaw);
    float c = cosf(theta), s = sinf(theta);
    float scale = GetHeadPositionScale();
    float ox = (dx * c + dz * s) * scale;
    float oy = dy * scale;
    float oz = (-dx * s + dz * c) * scale;
    if (GetHeadPositionInvertHorizontal()) // live-confirmed 2026-09-21: forward and lateral were both reversed
    {
        ox = -ox;
        oz = -oz;
    }

    float len = sqrtf(ox * ox + oy * oy + oz * oz);
    if (len > kMaxOffsetMeters)
    {
        float k = kMaxOffsetMeters / len;
        ox *= k;
        oy *= k;
        oz *= k;
    }
    if (!std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(oz))
        return;

    if (SehWriteTrans(transformPtr, cam[12] + ox, cam[13] + oy, cam[14] + oz))
    {
        g_lastPosX.store(pose.positionX, std::memory_order_relaxed);
        g_lastPosY.store(pose.positionY, std::memory_order_relaxed);
        g_lastPosZ.store(pose.positionZ, std::memory_order_relaxed);
        g_lastPosValid.store(true, std::memory_order_relaxed);
    }

    static std::atomic<unsigned long long> nextLogMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogMs.load(std::memory_order_relaxed);
    if (now >= allowed && nextLogMs.compare_exchange_strong(allowed, now + 1000, std::memory_order_relaxed))
        MOHW_LOG(kLogFile,
                  "head position: delta XR(%.3f,%.3f,%.3f) m -> game offset(%.3f,%.3f,%.3f) scale %.2f theta %.3f | camera trans (%.3f,%.3f,%.3f)",
                  dx, dy, dz, ox, oy, oz, scale, theta, cam[12], cam[13], cam[14]);
}

bool GetLastAppliedHeadPosition(float out[3])
{
    if (!g_lastPosValid.load(std::memory_order_relaxed))
        return false;
    out[0] = g_lastPosX.load(std::memory_order_relaxed);
    out[1] = g_lastPosY.load(std::memory_order_relaxed);
    out[2] = g_lastPosZ.load(std::memory_order_relaxed);
    return true;
}

} // namespace mohw
