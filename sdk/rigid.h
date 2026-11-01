#pragma once
// Rigid transforms in the row-vector convention the first-person code uses throughout: world = local * R + t, rows =
// axes, and "A * B" means apply A, then B. Shared by hooks/camera_matrix_test_hook.cpp (the rig's cameras) and
// hooks/first_person_rig.cpp (the arms).

#include <cmath>

namespace mohw {

struct Rigid
{
    float R[3][3]; // rows = axes
    float t[3];
};

inline Rigid RigidIdentity()
{
    Rigid out{};
    for (int i = 0; i < 3; ++i)
        out.R[i][i] = 1.0f;
    return out;
}

// a then b.
inline Rigid Compose(const Rigid& a, const Rigid& b)
{
    Rigid out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.R[i][j] = a.R[i][0] * b.R[0][j] + a.R[i][1] * b.R[1][j] + a.R[i][2] * b.R[2][j];
    for (int j = 0; j < 3; ++j)
        out.t[j] = a.t[0] * b.R[0][j] + a.t[1] * b.R[1][j] + a.t[2] * b.R[2][j] + b.t[j];
    return out;
}

inline Rigid Inverse(const Rigid& a)
{
    Rigid out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.R[i][j] = a.R[j][i];
    for (int j = 0; j < 3; ++j)
        out.t[j] = -(a.t[0] * out.R[0][j] + a.t[1] * out.R[1][j] + a.t[2] * out.R[2][j]);
    return out;
}

inline Rigid FromMatrix(const float m[16])
{
    Rigid out{};
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            out.R[i][j] = m[i * 4 + j];
        out.t[i] = m[12 + i];
    }
    return out;
}

inline void ToMatrix(const Rigid& a, float m[16])
{
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            m[i * 4 + j] = a.R[i][j];
        m[12 + i] = a.t[i];
    }
}

inline void PointTimes(const float p[3], const Rigid& a, float out[3])
{
    float r[3];
    for (int j = 0; j < 3; ++j)
        r[j] = p[0] * a.R[0][j] + p[1] * a.R[1][j] + p[2] * a.R[2][j] + a.t[j];
    out[0] = r[0], out[1] = r[1], out[2] = r[2];
}

// Rotation matrix (rows) -> quaternion, x y z w, any consistent pair with RotationFromQuat.
inline void QuatFromRotation(const float R[3][3], float q[4])
{
    const float trace = R[0][0] + R[1][1] + R[2][2];
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (R[1][2] - R[2][1]) / s;
        q[1] = (R[2][0] - R[0][2]) / s;
        q[2] = (R[0][1] - R[1][0]) / s;
    }
    else if (R[0][0] > R[1][1] && R[0][0] > R[2][2])
    {
        const float s = std::sqrt(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2.0f;
        q[3] = (R[1][2] - R[2][1]) / s;
        q[0] = 0.25f * s;
        q[1] = (R[0][1] + R[1][0]) / s;
        q[2] = (R[2][0] + R[0][2]) / s;
    }
    else if (R[1][1] > R[2][2])
    {
        const float s = std::sqrt(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2.0f;
        q[3] = (R[2][0] - R[0][2]) / s;
        q[0] = (R[0][1] + R[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (R[1][2] + R[2][1]) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2.0f;
        q[3] = (R[0][1] - R[1][0]) / s;
        q[0] = (R[2][0] + R[0][2]) / s;
        q[1] = (R[1][2] + R[2][1]) / s;
        q[2] = 0.25f * s;
    }
}

inline void RotationFromQuat(const float q[4], float R[3][3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0][0] = 1.0f - 2.0f * (y * y + z * z);
    R[0][1] = 2.0f * (x * y + z * w);
    R[0][2] = 2.0f * (x * z - y * w);
    R[1][0] = 2.0f * (x * y - z * w);
    R[1][1] = 1.0f - 2.0f * (x * x + z * z);
    R[1][2] = 2.0f * (y * z + x * w);
    R[2][0] = 2.0f * (x * z + y * w);
    R[2][1] = 2.0f * (y * z - x * w);
    R[2][2] = 1.0f - 2.0f * (x * x + y * y);
}

// The shortest rotation taking unit a onto unit b, as a row-vector rotation (a * R = b). False if they're opposite.
inline bool RotationBetween(const float a[3], const float b[3], float R[3][3])
{
    const float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (c < -0.999f)
        return false;
    const float v[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float k = 1.0f / (1.0f + c);
    const float K[3][3] = {{0, -v[2], v[1]}, {v[2], 0, -v[0]}, {-v[1], v[0], 0}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            const float square = K[i][0] * K[0][j] + K[i][1] * K[1][j] + K[i][2] * K[2][j];
            R[j][i] = (i == j ? 1.0f : 0.0f) + K[i][j] + k * square; // transposed: column form to row vectors
        }
    return true;
}

// Between two rotations, u = 0..1 (normalised lerp, the short way round).
inline void BlendRotation(const float a[3][3], const float b[3][3], float u, float out[3][3])
{
    float qa[4], qb[4];
    QuatFromRotation(a, qa);
    QuatFromRotation(b, qb);
    const float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    const float sign = d < 0.0f ? -1.0f : 1.0f;
    float q[4], len = 0.0f;
    for (int k = 0; k < 4; ++k)
    {
        q[k] = qa[k] + (sign * qb[k] - qa[k]) * u;
        len += q[k] * q[k];
    }
    len = std::sqrt(len);
    if (len < 1e-6f)
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out[i][j] = b[i][j];
        return;
    }
    for (int k = 0; k < 4; ++k)
        q[k] /= len;
    RotationFromQuat(q, out);
}

} // namespace mohw
