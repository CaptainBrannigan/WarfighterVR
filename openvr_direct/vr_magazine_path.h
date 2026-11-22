#pragma once
// THE MAGAZINE'S PATH (2026-11-14): how a magazine leaves its gun, shared by the reload's state machine (vr_reload.cpp,
// submit thread, tracking space) and the drawing (hooks/camera_matrix_test_hook.cpp, the batch's view).
//
// Both work in the gun's frame at the magazine's seat ("gun-local"): origin = the seated Wep_Mag point, axes = the gun's
// right / up / back (its rest view's). The magazine's own frame ("mag-local") is that same frame while it's seated, so a
// magazine at path position s is mag-local * PathDisplacement(s) in gun-local. s = 0 seated, 1 clear of the magwell:
// it slides along the line from the seat to the weapon profile's aim point, s = 1 being the platform's travel metres
// down that line. (2026-11-18: straight only -- the AK's hinged rock-out is gone, and going in no longer uses the path:
// vr_reload.cpp seats a held magazine as the first version did.)
// Row-vector convention (sdk/rigid.h): world = local * R + t, Compose(a, b) = a then b.

#include "../sdk/rigid.h"
#include "../sdk/settings.h"

#include <cmath>

namespace mohw::openvr_direct {

struct MagazinePath
{
    float axis[3] = {0, 1, 0}; // unit, seat -> aim (the way in)
    float travel = 0.06f;      // metres
};

// The equipped gun's path, from its weapon profile (the aim point) and its magazine platform.
inline MagazinePath CurrentMagazinePath()
{
    MagazinePath p;
    const MagazinePlatform plat = GetMagazinePlatform();
    const MagazineProfile prof = GetMagazineProfile();
    float a[3] = {prof.aimPoint[0], prof.aimPoint[1], -prof.aimPoint[2]}; // right, up, back
    const float length = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (length > 1e-3f)
        for (int k = 0; k < 3; ++k)
            p.axis[k] = a[k] / length;
    p.travel = plat.travel;
    return p;
}

// The magazine at s, gun-local: mag-local -> gun-local.
inline Rigid PathDisplacement(const MagazinePath& p, float s)
{
    s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
    Rigid d = RigidIdentity();
    for (int k = 0; k < 3; ++k)
        d.t[k] = -p.axis[k] * p.travel * s;
    return d;
}

// The s at which `grab` (a point on the magazine, mag-local) comes nearest `target` (gun-local). Unclamped.
inline float ProjectOnPath(const MagazinePath& p, const float grab[3], const float target[3])
{
    if (p.travel < 1e-4f)
        return 0.0f;
    float d = 0.0f;
    for (int k = 0; k < 3; ++k)
        d += (grab[k] - target[k]) * p.axis[k];
    return d / p.travel;
}

// How far the magazine travels from s = 0 to 1, metres (at least 1 cm).
inline float PathLength(const MagazinePath& p)
{
    return p.travel < 0.01f ? 0.01f : p.travel;
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

// The magazine waiting in the pouch, mag-local -> the frame its position and the body's axes give (right, up, back):
// turned by the pouch rotation (MagazinePouchPitch / Yaw / Roll), its insertion point at the pouch's centre.
inline Rigid PouchMagazine(const Rigid& pouchFrame)
{
    float deg[3];
    GetMagazinePouchRotation(deg);
    return Compose(RotationFromDegrees(deg), pouchFrame);
}

// The magazine in the hand that holds it: mag-local -> that controller's frame (its right / up / back), at the
// platform's hold (Magazine in hand right / up / forward, pitch / yaw / roll). The values are for the left hand; a
// magazine in the right hand (the gun in the left) is mirrored.
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

// A magazine taken from the pouch, mag-local -> the controller's frame, by MagazinePouchGrab (2026-11-21, the user:
// "only that the magazine snaps to the hand, but is [vertical] relative to where the hand grips it ... give an offset";
// first done on the hand's Z, then: "I meant the vertical axis"). grabbed: where it sat relative to the hand at the
// grab. Snapped: the platform's hold. Snapped with the vertical from the grip: the hold without its up offset, slid
// along the magazine's own vertical (mag-local up, the line it goes into the magwell along) until the hand is as far up
// the magazine as it gripped it, then the hold's up added in the hand's up as an offset from there.
inline Rigid PouchGrabInHand(const Rigid& grabbed, const MagazinePlatform& plat, bool inRightHand)
{
    const int mode = GetMagazinePouchGrab();
    if (mode == kPouchGrabAsGrabbed)
        return grabbed;
    if (mode == kPouchGrabSnap)
        return MagazineHold(plat, inRightHand);
    MagazinePlatform level = plat;
    level.holdPos[1] = 0.0f;
    Rigid r = MagazineHold(level, inRightHand);
    // The controller in mag-local: its up component is how far up the magazine the hand is. Moving the magazine d along
    // its own up (r.R[1], in the hand's frame) lowers that by d.
    const float gripped = Inverse(grabbed).t[1];
    const float held = Inverse(r).t[1];
    const float d = held - gripped;
    for (int k = 0; k < 3; ++k)
        r.t[k] += d * r.R[1][k];
    r.t[1] += plat.holdPos[1];
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
