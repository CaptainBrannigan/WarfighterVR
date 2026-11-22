#pragma once
// The boat seats' math (hooks/boat_seat.cpp), apart from the game so it can be tested offline.
//
// Conventions, the project's proven ones: a camera's rows are right / up / back, row vectors (sdk/rigid.h), and tracking
// space is x right, y up, z back, so a tracking axis carries straight onto the row of the same index
// (hooks/laser_raycast.cpp RENDER MAPPING). A heading y has back = (sin y, 0, cos y): it grows turning left. A pitch
// grows looking up. The game's entity data has x left, y up, z forward: the numbers below have x and z turned round.

#include "../sdk/rigid.h"
#include "../sdk/vr_math.h"

#include <cmath>

namespace mohw::boat {

constexpr float kPi = 3.14159265f;
constexpr float kDegree = kPi / 180.0f;

// THE MK19, from MOHW/Vehicles/rhib/RHIB_SP and its skeleton (tools/fb2dump, 2026-11-22): the turret's two bones,
// front_swivel and front_weapon, sit on one point, so it turns and tilts about a single pivot, and everything here is
// rigid to it. The gunner's camera (component af40b544): 0.90 behind, 0.32 above, tilted 4 degrees down from the barrel.
// The handles: the middle of the weapon's two hand IK points (834bd588, 345e1d5f).
constexpr float kCameraFromPivot[3] = {0.007f, 0.320f, 0.900f};
constexpr float kCameraUp[3] = {0.0f, 0.99756f, -0.06976f};
constexpr float kCameraBack[3] = {0.0f, 0.06976f, 0.99756f};
constexpr float kHandle[3] = {0.044f, 0.0885f, 0.258f};

inline float Length(const float v[3])
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

inline float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

inline Rigid LevelBase(float yaw)
{
    Rigid b{};
    b.R[0][0] = cosf(yaw);
    b.R[0][2] = -sinf(yaw);
    b.R[1][1] = 1.0f;
    b.R[2][0] = sinf(yaw);
    b.R[2][2] = cosf(yaw);
    return b;
}

inline float HeadingOfBack(const float back[3])
{
    return atan2f(back[0], back[2]);
}

inline float HeadHeading(const Quat& head)
{
    Vec3 forward{};
    forward.z = -1.0f;
    const Vec3 f = QuatRotateVector(head, forward);
    return atan2f(-f.x, -f.z);
}

// Where a seat's straight ahead was set: the head's heading and place then.
struct Reference
{
    float yaw = 0.0f;
    float position[3] = {};
};

// A tracking-space vector in the reference's level frame.
inline void IntoReference(const float v[3], float referenceYaw, float out[3])
{
    const float c = cosf(referenceYaw), s = sinf(referenceYaw);
    out[0] = v[0] * c - v[2] * s;
    out[1] = v[1];
    out[2] = v[0] * s + v[2] * c;
}

inline void IntoWorld(const float v[3], const Rigid& base, float out[3])
{
    for (int k = 0; k < 3; ++k)
        out[k] = v[0] * base.R[0][k] + v[1] * base.R[1][k] + v[2] * base.R[2][k];
}

inline void IntoBase(const float v[3], const Rigid& base, float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = v[0] * base.R[i][0] + v[1] * base.R[i][1] + v[2] * base.R[i][2];
}

// The head's camera: the seat's eye point, the level base turned and moved by the head since the reference.
inline Rigid HeadCamera(const Rigid& base, const float eyePoint[3], const Quat& head, const float headPosition[3],
                        const Reference& reference, float metresScale, float maxMetres)
{
    Rigid out{};
    for (int axis = 0; axis < 3; ++axis)
    {
        Vec3 unit{};
        unit.x = axis == 0 ? 1.0f : 0.0f;
        unit.y = axis == 1 ? 1.0f : 0.0f;
        unit.z = axis == 2 ? 1.0f : 0.0f;
        const Vec3 t = QuatRotateVector(head, unit);
        const float tracking[3] = {t.x, t.y, t.z};
        float local[3];
        IntoReference(tracking, reference.yaw, local);
        IntoWorld(local, base, out.R[axis]);
    }
    float moved[3], local[3], world[3];
    for (int k = 0; k < 3; ++k)
        moved[k] = (headPosition[k] - reference.position[k]) * metresScale;
    const float length = Length(moved);
    if (length > maxMetres)
        for (float& m : moved)
            m *= maxMetres / length;
    IntoReference(moved, reference.yaw, local);
    IntoWorld(local, base, world);
    for (int k = 0; k < 3; ++k)
        out.t[k] = eyePoint[k] + world[k];
    return out;
}

// The gun in the world from the gunner's camera: rows its right / up / back, t the pivot. Being from the camera, the
// pivot moves as smoothly as the camera does, and stays put while the gun turns.
inline Rigid GunFromCamera(const Rigid& camera)
{
    Rigid local = RigidIdentity();
    for (int k = 0; k < 3; ++k)
    {
        local.R[1][k] = kCameraUp[k];
        local.R[2][k] = kCameraBack[k];
        local.t[k] = kCameraFromPivot[k];
    }
    return Compose(Inverse(local), camera);
}

inline void GunAngles(const Rigid& gun, float baseYaw, float* yaw, float* pitch)
{
    *yaw = WrapAngleSigned(HeadingOfBack(gun.R[2]) - baseYaw);
    *pitch = -asinf(Clamp(gun.R[2][1], -1.0f, 1.0f));
}

// The handles from the pivot, in the base frame, with the gun at these angles.
inline void HandleAt(float yaw, float pitch, float out[3])
{
    const float y = kHandle[1] * cosf(pitch) - kHandle[2] * sinf(pitch);
    const float z = kHandle[2] * cosf(pitch) + kHandle[1] * sinf(pitch);
    out[0] = kHandle[0] * cosf(yaw) + z * sinf(yaw);
    out[1] = y;
    out[2] = -kHandle[0] * sinf(yaw) + z * cosf(yaw);
}

// The angles that put the handles where d is, or on the line to it (the pivot only turns). False too near the pivot to
// have a direction.
inline bool AimFromHandle(const float d[3], float* yaw, float* pitch)
{
    constexpr float kNearPivot = 0.08f;
    const float length = Length(d);
    if (!(length >= kNearPivot))
        return false;
    const float scale = Length(kHandle) / length;
    const float r = sqrtf(kHandle[1] * kHandle[1] + kHandle[2] * kHandle[2]);
    const float p = Clamp(atan2f(kHandle[1], kHandle[2]) - asinf(Clamp(d[1] * scale / r, -1.0f, 1.0f)), -80.0f * kDegree,
                          80.0f * kDegree);
    const float z = kHandle[2] * cosf(p) + kHandle[1] * sinf(p);
    *yaw = WrapAngleSigned(atan2f(d[0], d[2]) - atan2f(kHandle[0], z));
    *pitch = p;
    return true;
}

// THE HANDS ON THE HANDLES (the user, 2026-11-22: "moving hands down and left moves the barrel up and right ... input
// until the position is matching"): while gripped, the handles are wanted exactly as far from where they were at the
// grab as the hands are from where those were, and the gun's angles are the ones that put them there about its pivot.
// A grab, or a hand joining or leaving, never moves the gun by itself.
struct Lever
{
    int hands = 0; // bit 0 the left, bit 1 the right
    float grabHands[3] = {};
    float grabHandle[3] = {};
    float wanted[3] = {};
    float yaw = 0.0f, pitch = 0.0f;

    void Release() { hands = 0; }

    // handsAt = the middle of the gripping hands, in the base frame.
    bool Update(int holding, const float handsAt[3], float gunYaw, float gunPitch, float* wantYaw, float* wantPitch)
    {
        if (holding == 0)
        {
            hands = 0;
            return false;
        }
        if (holding != hands)
        {
            if (hands == 0)
            {
                HandleAt(gunYaw, gunPitch, wanted);
                yaw = gunYaw;
                pitch = gunPitch;
            }
            for (int k = 0; k < 3; ++k)
            {
                grabHandle[k] = wanted[k];
                grabHands[k] = handsAt[k];
            }
            hands = holding;
        }
        for (int k = 0; k < 3; ++k)
            wanted[k] = grabHandle[k] + handsAt[k] - grabHands[k];
        float y = 0.0f, p = 0.0f;
        if (AimFromHandle(wanted, &y, &p))
        {
            yaw = y;
            pitch = p;
        }
        *wantYaw = yaw;
        *wantPitch = pitch;
        return true;
    }
};

// One axis of the turret, driven as a stick until the gun is where it's wanted. The game's input map negates neither
// axis, but an animation graph turns the turret, so which way it answers is watched: until it has plainly followed, a
// plainly opposite answer flips the sign.
struct TurretAxis
{
    float sign = 1.0f;
    float score = 0.0f;
    float sent = 0.0f;
    float lastAngle = 0.0f;
    bool haveAngle = false;
    bool confirmed = false;

    void Forget() // a gap: the next angle isn't an answer to what was sent
    {
        sent = 0.0f;
        haveAngle = false;
    }

    // angle = the turret's own, growing the way a positive error points. Returns the stick, -1..1.
    float Update(float error, float angle, float strength, float deadzone)
    {
        constexpr float kSettled = 0.6f * kDegree;
        constexpr float kFullStickPerRadian = 4.0f;
        constexpr float kProof = 0.15f;    // radians turned the right way under a clear stick
        constexpr float kDisproof = 0.30f; // and the wrong way: more, since it coasts on for 0.2 s after a reversal
        if (haveAngle && !confirmed && fabsf(sent) > 0.4f)
        {
            const float turned = WrapAngleSigned(angle - lastAngle);
            score += (sent * sign > 0.0f ? 1.0f : -1.0f) * turned;
            if (score >= kProof)
                confirmed = true;
            else if (score <= -kDisproof)
            {
                sign = -sign;
                score = 0.0f;
            }
        }
        haveAngle = true;
        lastAngle = angle;
        sent = 0.0f;
        if (fabsf(error) > kSettled)
        {
            float v = Clamp(fabsf(error) * kFullStickPerRadian * strength, 0.0f, 1.0f);
            if (deadzone > 0.0f && deadzone < 0.9f)
                v = deadzone + (1.0f - deadzone) * v;
            sent = (error > 0.0f ? v : -v) * sign;
        }
        return sent;
    }
};

} // namespace mohw::boat
