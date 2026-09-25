#pragma once
// Eye-matched view geometry (2026-09-20) -- ONE implementation shared by the 32-bit game
// side (hooks/eye_matched_fov.cpp) and the 64-bit companion (companion/main.cpp), so the
// FOV the game renders with and the crop/declared FOV the companion submits can never
// disagree. Replaces the old FovScale multiplier + cover-crop + 1.12 margin method.
//
// The game can only render a SYMMETRIC frustum, but a headset eye's real frustum is
// asymmetric (Quest 3 via SteamVR: left eye -54/+40 deg horizontally, +44/-55 deg
// vertically). So the game renders a centered frustum that ENCLOSES both eyes' real
// frustums, and the companion samples each eye's real sub-rectangle out of it and
// declares the real (asymmetric) FOV to the compositor -- no stretch, no margin fudge.
//
//   game vertical FOV = 2*atan(T_V),  T_V = max over both eyes of |tan(up)|,|tan(down)|
//   game horizontal half-tan T_H = T_V * (game render aspect W/H)
//   eye crop in the source (v=0 at top): u0 = (xl+T_H)/(2T_H), u1 = (xr+T_H)/(2T_H),
//                                        v0 = (T_V-yu)/(2T_V), v1 = (T_V-yd)/(2T_V)
//   where xl/xr/yu/yd are the eye's real frustum edges in tan space, clamped to the source.
//   The declared FOV is atan() of those (clamped) edges, so what is declared is exactly
//   what is shown.
//
// Angles are OpenXR XrFovf half-angles in radians (angleLeft/angleDown negative), indexed
// by eye (0 = left, 1 = right) -- the same layout as ipc_protocol.h's HmdViewBlock.

#include <cmath>

namespace mohwvr::eyefrustum {

struct EyeCrop
{
    float uvScaleOffset[4]; // .xy = scale, .zw = offset (remaps the eye's full 0..1 UV into the source)
    float angleLeft;        // declared FOV for this eye's projection layer view (radians)
    float angleRight;
    float angleUp;
    float angleDown;
    bool clamped;           // true if the source did not fully cover the eye's real frustum
};

inline float EnclosingHalfTanVertical(const float up[2], const float down[2])
{
    float t = 0.0f;
    for (int i = 0; i < 2; ++i)
    {
        float u = fabsf(tanf(up[i]));
        float d = fabsf(tanf(down[i]));
        if (u > t)
            t = u;
        if (d > t)
            t = d;
    }
    return t;
}

// Vertical FOV (radians) the game should render with so its frustum encloses both eyes.
inline float EnclosingVerticalFovRad(const float up[2], const float down[2])
{
    return 2.0f * atanf(EnclosingHalfTanVertical(up, down));
}

inline EyeCrop ComputeEyeCrop(int eye, const float left[2], const float right[2], const float up[2], const float down[2],
                              float sourceAspect)
{
    EyeCrop out{};
    float tV = EnclosingHalfTanVertical(up, down);
    float tH = tV * sourceAspect;
    if (tV <= 0.0f || tH <= 0.0f)
    {
        out.uvScaleOffset[0] = out.uvScaleOffset[1] = 1.0f;
        out.angleLeft = left[eye];
        out.angleRight = right[eye];
        out.angleUp = up[eye];
        out.angleDown = down[eye];
        return out;
    }

    float xl = tanf(left[eye]);
    float xr = tanf(right[eye]);
    float yu = tanf(up[eye]);
    float yd = tanf(down[eye]);
    if (xl < -tH) { xl = -tH; out.clamped = true; }
    if (xr > tH) { xr = tH; out.clamped = true; }
    if (yu > tV) { yu = tV; out.clamped = true; }
    if (yd < -tV) { yd = -tV; out.clamped = true; }

    float u0 = (xl + tH) / (2.0f * tH);
    float u1 = (xr + tH) / (2.0f * tH);
    float v0 = (tV - yu) / (2.0f * tV);
    float v1 = (tV - yd) / (2.0f * tV);
    out.uvScaleOffset[0] = u1 - u0;
    out.uvScaleOffset[1] = v1 - v0;
    out.uvScaleOffset[2] = u0;
    out.uvScaleOffset[3] = v0;
    out.angleLeft = atanf(xl);
    out.angleRight = atanf(xr);
    out.angleUp = atanf(yu);
    out.angleDown = atanf(yd);
    return out;
}

} // namespace mohwvr::eyefrustum
