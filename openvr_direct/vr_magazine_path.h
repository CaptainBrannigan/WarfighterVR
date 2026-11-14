#pragma once
// THE MAGAZINE'S PATH (2026-11-14): how a magazine leaves and enters its gun, shared by the reload's state machine
// (vr_reload.cpp, submit thread, tracking space) and the drawing (hooks/camera_matrix_test_hook.cpp, the batch's view).
//
// Both work in the gun's frame at the magazine's seat ("gun-local"): origin = the seated Wep_Mag point, axes = the gun's
// right / up / back (its rest view's). The magazine's own frame ("mag-local") is that same frame while it's seated, so a
// magazine at path position s is mag-local * PathDisplacement(s) in gun-local. s = 0 seated, 1 clear of the magwell.
//   Straight (AR): it slides along the line from the seat to the weapon profile's aim point; s = 1 is travel metres
//   down that line.
//   Hinge (AK): it rocks about the gun's right axis through the hinge point (the front lug), its bottom swinging
//   forward; s = 1 is the platform's angle.
// Row-vector convention (sdk/rigid.h): world = local * R + t, Compose(a, b) = a then b.

#include "../sdk/rigid.h"
#include "../sdk/settings.h"

#include <cmath>

namespace mohw::openvr_direct {

struct MagazinePath
{
    bool hinge = false;
    float axis[3] = {0, 1, 0}; // straight: unit, seat -> aim (the way in)
    float travel = 0.06f;      // straight, metres
    float pivot[3] = {};       // hinge point
    float angleRad = 0.4f;     // hinge
};

// The equipped gun's path, from its weapon profile (the aim point) and its magazine platform.
inline MagazinePath CurrentMagazinePath()
{
    MagazinePath p;
    const MagazinePlatform plat = GetMagazinePlatform();
    const MagazineProfile prof = GetMagazineProfile();
    p.hinge = plat.hinge;
    float a[3] = {prof.aimPoint[0], prof.aimPoint[1], -prof.aimPoint[2]}; // right, up, back
    const float length = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (length > 1e-3f)
        for (int k = 0; k < 3; ++k)
            p.axis[k] = a[k] / length;
    p.travel = plat.travel;
    p.pivot[0] = 0.0f;
    p.pivot[1] = plat.hingePoint[1];
    p.pivot[2] = -plat.hingePoint[0]; // forward = -back
    p.angleRad = plat.hingeDeg * 3.14159265f / 180.0f;
    return p;
}

// The magazine at s, gun-local: mag-local -> gun-local.
inline Rigid PathDisplacement(const MagazinePath& p, float s)
{
    s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
    Rigid d = RigidIdentity();
    if (!p.hinge)
    {
        for (int k = 0; k < 3; ++k)
            d.t[k] = -p.axis[k] * p.travel * s;
        return d;
    }
    // About +x: up turns toward back, so a point below the pivot swings forward (the AK rocked out).
    const float a = p.angleRad * s, c = std::cos(a), n = std::sin(a);
    d.R[1][1] = c;
    d.R[1][2] = n;
    d.R[2][1] = -n;
    d.R[2][2] = c;
    for (int j = 0; j < 3; ++j) // about the pivot: p' = (p - pivot) R + pivot
        d.t[j] = p.pivot[j] - (p.pivot[0] * d.R[0][j] + p.pivot[1] * d.R[1][j] + p.pivot[2] * d.R[2][j]);
    return d;
}

// The s at which `grab` (a point on the magazine, mag-local) comes nearest `target` (gun-local). Unclamped.
inline float ProjectOnPath(const MagazinePath& p, const float grab[3], const float target[3])
{
    if (!p.hinge)
    {
        if (p.travel < 1e-4f)
            return 0.0f;
        float d = 0.0f;
        for (int k = 0; k < 3; ++k)
            d += (grab[k] - target[k]) * p.axis[k];
        return d / p.travel;
    }
    // The angle about x from the grab point to the target, seen from the pivot (the y-z plane).
    const float ay = grab[1] - p.pivot[1], az = grab[2] - p.pivot[2];
    const float by = target[1] - p.pivot[1], bz = target[2] - p.pivot[2];
    const float angle = std::atan2(ay * bz - az * by, ay * by + az * bz);
    return p.angleRad > 1e-4f ? angle / p.angleRad : 0.0f;
}

// How far `grab` travels from s = 0 to 1, metres (at least 1 cm).
inline float PathLength(const MagazinePath& p, const float grab[3])
{
    float length = p.travel;
    if (p.hinge)
    {
        const float ay = grab[1] - p.pivot[1], az = grab[2] - p.pivot[2];
        float radius = std::sqrt(ay * ay + az * az);
        radius = radius < 0.03f ? 0.03f : radius;
        length = radius * p.angleRad;
    }
    return length < 0.01f ? 0.01f : length;
}

// Rotation = roll (about back), then pitch (about right), then yaw (about up), degrees: as the grips and the hands'
// calibrations (hooks/camera_matrix_test_hook.cpp RotationFromDeg).
inline Rigid RotationFromDegrees(const float deg[3])
{
    const float kRad = 3.14159265f / 180.0f;
    const float cp = std::cos(deg[0] * kRad), sp = std::sin(deg[0] * kRad);
    const float cy = std::cos(deg[1] * kRad), sy = std::sin(deg[1] * kRad);
    const float cr = std::cos(deg[2] * kRad), sr = std::sin(deg[2] * kRad);
    const Rigid roll{{{cr, sr, 0}, {-sr, cr, 0}, {0, 0, 1}}, {0, 0, 0}};
    const Rigid pitch{{{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}}, {0, 0, 0}};
    const Rigid yaw{{{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}}, {0, 0, 0}};
    return Compose(Compose(roll, pitch), yaw);
}

// The magazine in the hand that holds it: mag-local -> that controller's frame (its right / up / back). The platform's
// offsets are for the left hand; a magazine in the right hand (the gun in the left) is mirrored.
inline Rigid MagazineHold(const MagazinePlatform& plat, bool inRightHand)
{
    float deg[3] = {plat.holdDeg[0], plat.holdDeg[1], plat.holdDeg[2]};
    float pos[3] = {plat.holdPos[0], plat.holdPos[1], -plat.holdPos[2]}; // right, up, back
    if (inRightHand)
    {
        pos[0] = -pos[0];
        deg[1] = -deg[1];
        deg[2] = -deg[2];
    }
    Rigid r = RotationFromDegrees(deg);
    for (int k = 0; k < 3; ++k)
        r.t[k] = pos[k];
    return r;
}

// Between two frames, u = 0..1: the rotation the short way round, the origin lerped.
inline Rigid BlendRigid(const Rigid& a, const Rigid& b, float u)
{
    u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
    Rigid r = b;
    BlendRotation(a.R, b.R, u, r.R);
    for (int k = 0; k < 3; ++k)
        r.t[k] = a.t[k] + (b.t[k] - a.t[k]) * u;
    return r;
}

inline float SmoothStep01(float x)
{
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    return x * x * (3.0f - 2.0f * x);
}

} // namespace mohw::openvr_direct
