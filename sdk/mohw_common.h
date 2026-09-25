#pragma once
// Shared primitives used across the reversed MOHW SDK headers.
//
// The original reversed dump this SDK is built from left PAD/POINTERCHK as
// undefined macros and never showed Vec2 despite using it in RenderViewDesc.
// Definitions below fill those gaps; everything else preserves the original
// field layout/offsets exactly since those are empirical (read from the
// live binary) and must not be "corrected".

#include <cmath>
#include <cstdint>

// Reserves raw bytes for fields that haven't been reversed yet. The byte
// count must exactly match the original class's memory layout or every
// member declared after it lands at the wrong offset.
#define MOHW_PAD_CAT_(a, b) a##b
#define MOHW_PAD_CAT(a, b) MOHW_PAD_CAT_(a, b)
#define PAD(size) unsigned char MOHW_PAD_CAT(_pad_, __LINE__)[size]

// Null check used throughout the original dump wherever a reversed pointer
// is dereferenced. Kept as a simple non-null check; if crashes trace back to
// a non-null-but-freed pointer, harden this with an IsBadReadPtr-style probe.
#define POINTERCHK(p) ((p) != nullptr)

namespace mohw {

class Vec2
{
public:
    float x;
    float y;
};

class Vec3
{
public:
    union
    {
        struct
        {
            float x, y, z, w;
        };
        float data[4];
    };

    float Len() const { return std::sqrt(x * x + y * y + z * z); }

    void Normalize()
    {
        float l = Len();
        x /= l;
        y /= l;
        z /= l;
    }

    Vec3 operator*(float s) const
    {
        Vec3 v;
        v.x = x * s;
        v.y = y * s;
        v.z = z * s;
        v.w = w;
        return v;
    }

    Vec3 operator+(const Vec3& o) const
    {
        Vec3 v;
        v.x = x + o.x;
        v.y = y + o.y;
        v.z = z + o.z;
        v.w = 0.0f;
        return v;
    }

    Vec3 operator-(const Vec3& o) const
    {
        Vec3 v;
        v.x = x - o.x;
        v.y = y - o.y;
        v.z = z - o.z;
        v.w = 0.0f;
        return v;
    }

    Vec3& operator+=(const Vec3& o)
    {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }

    Vec3& operator-=(const Vec3& o)
    {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }

    float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }

    float DistanceTo(const Vec3& o) const
    {
        float dx = o.x - x, dy = o.y - y, dz = o.z - z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
};

// Engine's row-major transform: three orthonormal basis rows (left/up/forward)
// plus a translation row, each a Vec3 padded out to 16 bytes (hence Vec3
// carries an unused .w). This is the type m_viewMatrix/m_projectionMatrix/etc.
// on RenderView all use.
class LinearTransform
{
public:
    union
    {
        struct
        {
            Vec3 left, up, forward, trans;
        };
        Vec3 m_rows[4];
        float data[4][4];
    };

    LinearTransform& operator=(const LinearTransform& from)
    {
        m_rows[0] = from.m_rows[0];
        m_rows[1] = from.m_rows[1];
        m_rows[2] = from.m_rows[2];
        m_rows[3] = from.m_rows[3];
        return *this;
    }

    bool GetOrigin(Vec3* out) const
    {
        if (!out)
            return false;
        *out = trans;
        return true;
    }
};

class QuatTransform
{
public:
    Vec3 transAndScale; // 0x00
    Vec3 rotation;      // 0x10
};

class RenderScreenInfo
{
public:
    uint32_t nWidth;
    uint32_t nHeight;
    uint32_t nWindowWidth;
    uint32_t nWindowHeight;
    float fRefreshRate;
};

} // namespace mohw
