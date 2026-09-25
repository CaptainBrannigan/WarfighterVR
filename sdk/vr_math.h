#pragma once
// Small, self-contained vector/quaternion math for the head-pose -> view-
// matrix patching path (task: patch per-eye view matrix with HMD pose + IPD
// offset, see docs/companion_process_findings.md). Deliberately separate
// from the reversed-engine structs in mohw_common.h/renderview.h -- this is
// our own code operating on copies of engine data, not a reversed layout
// itself, so it doesn't belong mixed into those files.

#include "mohw_common.h"
#include <cmath>

namespace mohw {

struct Quat
{
    float x, y, z, w;
};

inline Vec3 VecAdd(const Vec3& a, const Vec3& b)
{
    Vec3 out{};
    out.x = a.x + b.x;
    out.y = a.y + b.y;
    out.z = a.z + b.z;
    return out;
}

inline Vec3 VecSub(const Vec3& a, const Vec3& b)
{
    Vec3 out{};
    out.x = a.x - b.x;
    out.y = a.y - b.y;
    out.z = a.z - b.z;
    return out;
}

inline Vec3 VecScale(const Vec3& a, float s)
{
    Vec3 out{};
    out.x = a.x * s;
    out.y = a.y * s;
    out.z = a.z * s;
    return out;
}

inline float VecDot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Quat QuatConjugate(const Quat& q)
{
    return Quat{-q.x, -q.y, -q.z, q.w};
}

// Hamilton product, a*b (apply b first, then a, when used as rotations).
inline Quat QuatMultiply(const Quat& a, const Quat& b)
{
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// Standard efficient quaternion-vector rotation (avoids a full q*v*q^-1
// multiply): v' = v + 2*w*cross(qv,v) + 2*cross(qv,cross(qv,v)), rearranged
// into the usual two-cross-product form below. q is assumed unit-length
// (true for orientation quaternions coming straight from OpenXR).
inline Vec3 QuatRotateVector(const Quat& q, const Vec3& v)
{
    Vec3 qv{q.x, q.y, q.z};
    Vec3 cross1{qv.y * v.z - qv.z * v.y, qv.z * v.x - qv.x * v.z, qv.x * v.y - qv.y * v.x};
    Vec3 t = VecScale(cross1, 2.0f);
    Vec3 cross2{qv.y * t.z - qv.z * t.y, qv.z * t.x - qv.x * t.z, qv.x * t.y - qv.y * t.x};
    return VecAdd(VecAdd(v, VecScale(t, q.w)), cross2);
}

// Plain 4x4 matrix multiply, row-major storage (out[4*i+j] = M[row i][col
// j]) -- used to compute a * b for comparing against a candidate
// precombined matrix found in a live buffer dump (task: patch per-eye view
// matrix with HMD pose + IPD offset -- the view matrix alone had no
// visible effect on gameplay geometry despite being confirmed bound and
// correctly patched, most likely because the shader actually consumes a
// separate precombined view*projection matrix elsewhere in the same
// buffer instead of multiplying view and projection itself).
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

// Wraps to (-pi, pi] -- use for a DELTA between two angles so a wraparound
// crossing (e.g. -179 degrees vs +179 degrees) doesn't produce a huge
// spurious jump instead of the small real difference.
inline float WrapAngleSigned(float radians)
{
    radians = fmodf(radians, kTwoPi);
    if (radians > kPi)
        radians -= kTwoPi;
    else if (radians < -kPi)
        radians += kTwoPi;
    return radians;
}

// Wraps to [0, 2*pi) -- matches AimingController+0xC's own documented
// range (project memory: "authoritative absolute yaw heading in radians,
// 0..~2pi").
inline float WrapAngleUnsigned(float radians)
{
    radians = fmodf(radians, kTwoPi);
    if (radians < 0.0f)
        radians += kTwoPi;
    return radians;
}

// Extracts a yaw/pitch pair from an orientation quaternion by rotating the
// SAME local-forward convention already proven live for the render
// rotation hook (commit_view_transform_hook.cpp: local +Z = forward) and
// reading the resulting world-space vector's horizontal/vertical angle --
// deliberately NOT a generic quaternion-to-Euler formula (those have
// axis-order pitfalls this project has been burned by before, see
// commit_view_transform_hook.cpp's "confirmed live... inverted BOTH
// horizontal and vertical" comment); reusing QuatRotateVector keeps this on
// already-validated math instead of a fresh, unverified formula. Axis
// mapping assumption, NOT yet independently live-validated for this
// specific use (only validated for the render hook's basis composition):
// yaw = atan2(forward.x, forward.z), pitch = asin(forward.y). Expect to
// need sdk/settings.h's HeadAimInvertYaw/InvertPitch live if head-look ends
// up backwards or perpendicular to what's expected.
inline void QuatToYawPitch(const Quat& q, float* outYaw, float* outPitch)
{
    Vec3 localForward{};
    localForward.z = 1.0f;
    Vec3 f = QuatRotateVector(q, localForward);

    *outYaw = atan2f(f.x, f.z);
    float clampedY = f.y;
    if (clampedY > 1.0f)
        clampedY = 1.0f;
    if (clampedY < -1.0f)
        clampedY = -1.0f;
    *outPitch = asinf(clampedY);
}

// Extracts ROLL (rotation around the forward axis) from an orientation
// quaternion, independent of yaw/pitch -- the signed angle between the
// quaternion's actual "up" direction and the "roll-free" up direction
// implied by projecting world-up onto the plane perpendicular to forward.
// This is deliberately an ABSOLUTE measurement relative to world-level,
// not a recenter-relative delta like QuatToYawPitch's yaw/pitch -- roll
// has a natural, meaningful zero (level) independent of which way you're
// facing, unlike yaw/pitch which need a recenter reference. Returns 0 if
// forward is too close to vertical (world-up nearly parallel to forward,
// a degenerate case for this projection) -- rare in practice, MOHW's own
// pitch clamp is +/-75/72 degrees, never reaching true vertical.
inline float ExtractRoll(const Quat& q)
{
    Vec3 localForward{};
    localForward.z = 1.0f;
    Vec3 localUp{};
    localUp.y = 1.0f;
    Vec3 forward = QuatRotateVector(q, localForward);
    Vec3 up = QuatRotateVector(q, localUp);

    Vec3 worldUp{};
    worldUp.y = 1.0f;
    float d = VecDot(worldUp, forward);
    Vec3 expectedUp = VecSub(worldUp, VecScale(forward, d));
    float lenSq = VecDot(expectedUp, expectedUp);
    if (lenSq < 0.01f)
        return 0.0f;
    expectedUp = VecScale(expectedUp, 1.0f / sqrtf(lenSq));

    Vec3 cross{expectedUp.y * up.z - expectedUp.z * up.y, expectedUp.z * up.x - expectedUp.x * up.z,
                expectedUp.x * up.y - expectedUp.y * up.x};
    float sinPart = VecDot(cross, forward);
    float cosPart = VecDot(expectedUp, up);
    return atan2f(sinPart, cosPart);
}

// Rotates a Vec3's pitch by degreesDown (positive = steeper downward),
// preserving its horizontal heading and total magnitude. Y is up (same
// convention as QuatToYawPitch's pitch = asin(forward.y) above). Used by
// hooks/shot_redirect_hook.cpp to redirect a fired velocity vector for a
// live test of whether that vector is genuinely the bullet's real fired
// direction (see that file's header comment).
inline Vec3 Vec3RotatePitchDown(const Vec3& v, float degreesDown)
{
    float horiz = sqrtf(v.x * v.x + v.z * v.z);
    float speed = sqrtf(horiz * horiz + v.y * v.y);
    if (speed < 0.0001f)
        return v; // degenerate/zero vector -- nothing meaningful to rotate

    float currentPitch = atan2f(v.y, horiz);
    float newPitch = currentPitch - (degreesDown * (kPi / 180.0f));

    float newHoriz = speed * cosf(newPitch);
    float newY = speed * sinf(newPitch);
    float scale = (horiz > 0.0001f) ? (newHoriz / horiz) : 0.0f;

    Vec3 out{};
    out.x = v.x * scale;
    out.y = newY;
    out.z = v.z * scale;
    return out;
}

// Rotates a Vec3's yaw (heading around the vertical/up Y axis) by
// degreesRight, preserving pitch (the Y/vertical component) and total
// horizontal magnitude exactly. Same atan2(x, z) yaw convention as
// QuatToYawPitch above. Sign of "right" is NOT independently live-verified
// -- if a live test shows it going left instead, negate degreesRight at the
// call site rather than flipping the formula here (keeps this function's
// convention consistent for future callers). Used by
// hooks/fire_candidate_redirect_hook.cpp for a horizontal-redirect test,
// switched from the vertical Vec3RotatePitchDown to get a signal that can't
// be confused with a downward/gravity-like effect.
inline Vec3 Vec3RotateYawRight(const Vec3& v, float degreesRight)
{
    float horiz = sqrtf(v.x * v.x + v.z * v.z);
    if (horiz < 0.0001f)
        return v; // no horizontal component -- nothing meaningful to rotate

    float currentYaw = atan2f(v.x, v.z);
    float newYaw = currentYaw + (degreesRight * (kPi / 180.0f));

    Vec3 out{};
    out.x = horiz * sinf(newYaw);
    out.z = horiz * cosf(newYaw);
    out.y = v.y;
    return out;
}

inline void Mat4Multiply(const float a[16], const float b[16], float out[16])
{
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[4 * row + k] * b[4 * k + col];
            out[4 * row + col] = sum;
        }
    }
}

// SUPERSEDED (2026-09-23): assumed standard LH depth encoding. Live x32dbg verification of the
// engine's own FUN_00704c70 (breakpoint at its ret, 00705160, output matrix dumped whole) proved
// this wrong -- the real M[11] is exactly -1.0 (this function put +1.0 there) and M[10]/M[14] use
// the RH sign convention, not LH. Using this caused the world to fail depth testing entirely
// (only skybox+HUD visible) when it was briefly wired up. Kept only so the wrong convention isn't
// silently forgotten; BuildOffCenterProjectionRH below is the live-verified replacement -- use that.
inline void BuildOffCenterProjectionLH(float tanL, float tanR, float tanT, float tanB, float nearZ, float farZ, float out[16])
{
    float l = tanL * nearZ, r = tanR * nearZ, t = tanT * nearZ, b = tanB * nearZ;
    for (int i = 0; i < 16; ++i)
        out[i] = 0.0f;
    out[0] = 2.0f * nearZ / (r - l);
    out[5] = 2.0f * nearZ / (t - b);
    out[8] = (l + r) / (l - r);
    out[9] = (t + b) / (b - t);
    out[10] = farZ / (farZ - nearZ);
    out[11] = 1.0f;
    out[14] = nearZ * farZ / (nearZ - farZ);
}

// Standard D3DXMatrixPerspectiveOffCenterRH, row-major storage (out[4*row+col], matching
// Mat4Multiply's convention above) -- used by draw_duplication_hook.cpp's simultaneous-stereo path
// to build each eye's own asymmetric projection matrix independently. tanL/tanR/tanT/tanB are
// tan-space half-frustum edges (e.g. hooks/eye_matched_fov.h's GetTrueFrustumEdgesForEye's signed
// convention: tanL negative, tanR positive, tanT positive, tanB negative), NOT yet scaled by
// nearZ -- that scaling happens here.
//
// LIVE-VERIFIED (2026-09-23) against the engine's own FUN_00704c70 via x32dbg: broke at its ret
// (00705160, eax = output matrix pointer per the preceding `mov eax,esi`), dumped all 16 floats
// for a real p2==0 (perspective-branch) call. Observed M[10]=BF800019 (~-1.0000030) and
// M[11]=BF800000 (exactly -1.0) -- for the RH formula below with near~0.05/far~10000,
// far/(near-far) ~ -1.000005 and the constant -1 term land exactly on M[10]/M[11] respectively,
// confirming RH (not LH, where the -1 belongs on M[14] and M[10]/M[14] both come out positive-ish
// for typical near/far). M[14]=BD75C2BF (~-0.057) also matches near*far/(near-far)'s order of
// magnitude/sign for this convention. This is the fix for the world-invisible regression the LH
// version above caused.
inline void BuildOffCenterProjectionRH(float tanL, float tanR, float tanT, float tanB, float nearZ, float farZ, float out[16])
{
    float l = tanL * nearZ, r = tanR * nearZ, t = tanT * nearZ, b = tanB * nearZ;
    for (int i = 0; i < 16; ++i)
        out[i] = 0.0f;
    out[0] = 2.0f * nearZ / (r - l);
    out[5] = 2.0f * nearZ / (t - b);
    out[8] = (l + r) / (r - l);
    out[9] = (t + b) / (t - b);
    out[10] = farZ / (nearZ - farZ);
    out[11] = -1.0f;
    out[14] = nearZ * farZ / (nearZ - farZ);
}

} // namespace mohw
