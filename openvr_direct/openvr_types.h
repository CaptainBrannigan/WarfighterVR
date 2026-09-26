#pragma once
// Plain-data stand-ins for the few OpenVR types this project passes through raw vtable calls, shared by
// openvr_direct.cpp and vr_input.cpp. openvr.h itself is never included -- see openvr_direct.h's top comment (DRM).

#include "../sdk/vr_math.h"

#include <cmath>

namespace mohw::openvr_direct {

// Byte-for-byte stand-in for vr::TrackedDevicePose_t (third_party/openvr/headers/openvr.h). deviceToAbsoluteTracking
// is a row-major 3x4 matrix (float m[row][col]): columns 0-2 are the right/up/forward basis vectors, column 3 is the
// position, all in the compositor's tracking space.
struct RawTrackedDevicePose
{
    float deviceToAbsoluteTracking[3][4];
    float velocity[3];
    float angularVelocity[3];
    int trackingResult; // ETrackingResult -- unused, kept only so the struct's size/layout matches exactly
    bool poseIsValid;
    bool deviceIsConnected;
};
static_assert(sizeof(RawTrackedDevicePose) == 80, "must match vr::TrackedDevicePose_t");

// Standard robust rotation-matrix -> quaternion conversion (Shepperd's method), applied to a pose matrix's
// upper-left 3x3. OpenVR and OpenXR share the same coordinate convention (right-handed, Y-up, -Z-forward), so the
// result plugs directly into consumers originally built against the companion's OpenXR-sourced quaternions.
inline Quat MatrixToQuat(const float m[3][4])
{
    float r00 = m[0][0], r01 = m[0][1], r02 = m[0][2];
    float r10 = m[1][0], r11 = m[1][1], r12 = m[1][2];
    float r20 = m[2][0], r21 = m[2][1], r22 = m[2][2];
    float trace = r00 + r11 + r22;
    Quat q{};
    if (trace > 0.0f)
    {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (r21 - r12) / s;
        q.y = (r02 - r20) / s;
        q.z = (r10 - r01) / s;
    }
    else if (r00 > r11 && r00 > r22)
    {
        float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        q.w = (r21 - r12) / s;
        q.x = 0.25f * s;
        q.y = (r01 + r10) / s;
        q.z = (r02 + r20) / s;
    }
    else if (r11 > r22)
    {
        float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        q.w = (r02 - r20) / s;
        q.x = (r01 + r10) / s;
        q.y = 0.25f * s;
        q.z = (r12 + r21) / s;
    }
    else
    {
        float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        q.w = (r10 - r01) / s;
        q.x = (r02 + r20) / s;
        q.y = (r12 + r21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

} // namespace mohw::openvr_direct
