#include "scope_view.h"

#include "aiming_controller_hook.h"
#include "alternating_eye.h"
#include "companion_bridge.h"
#include "laser_raycast.h"
#include "render_pose_stamp.h"
#include "weapon_identity.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"
#include "../sdk/vr_math.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_scope.log";
constexpr float kMaxFovYRad = 3.05f;
constexpr unsigned long long kRecentMs = 200; // the mask and the hidden eye only count from a frame this recent

std::mutex g_maskMutex;
struct Mask
{
    int eye = -1;
    float cx = 0.5f, cy = 0.5f, rx = 0.1f, ry = 0.1f;
    bool reticle = false;
    float reticleX = 0.5f, reticleY = 0.5f; // the impact point, fractions of the half
    unsigned long long ms = 0;
};
Mask g_mask; // guarded by g_maskMutex
std::atomic<int> g_hiddenEye{-1};
std::atomic<unsigned long long> g_hiddenEyeMs{0};

Vec3 V(float x, float y, float z)
{
    Vec3 v{};
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

float Length(const Vec3& v)
{
    return std::sqrt(VecDot(v, v));
}

Vec3 Normalized(const Vec3& v)
{
    const float l = Length(v);
    return l > 1e-6f ? VecScale(v, 1.0f / l) : v;
}

Vec3 CrossV(const Vec3& a, const Vec3& b)
{
    return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

// The head pose the frame is shown from (hooks/camera_matrix_test_hook.cpp ShownHeadPose): the live pose, or the
// render-pose stamp when there is one.
bool ShownHead(Quat* q, Vec3* p)
{
    mohwvr::ipc::HeadPoseBlock head{};
    if (!GetHeadPose(&head))
        return false;
    *q = Quat{head.orientationX, head.orientationY, head.orientationZ, head.orientationW};
    *p = V(head.positionX, head.positionY, head.positionZ);
    float sq[4], sp[3];
    bool posValid = false;
    if (GetCommittedFrameRenderPose(sq, sp, &posValid))
    {
        *q = Quat{sq[0], sq[1], sq[2], sq[3]};
        if (posValid)
            *p = V(sp[0], sp[1], sp[2]);
    }
    return true;
}

// A frustum given by its tangents as the projection builder's inputs (native_stereo.cpp EyeFrustum).
bool FrustumFromTangents(float tL, float tR, float tU, float tD, ScopeEyeView* out)
{
    const float T = (tU - tD) * 0.5f;
    const float width = tR - tL;
    if (!(T > 1e-4f && width > 1e-4f))
        return false;
    out->aspect = width / (2.0f * T);
    out->offX = ((tL + tR) * 0.5f) / (out->aspect * T * 2.0f);
    out->offY = ((tU + tD) * 0.5f) / (T * 2.0f);
    out->fovY = 2.0f * std::atan(T);
    if (out->fovY > kMaxFovYRad)
        out->fovY = kMaxFovYRad;
    return true;
}

void ClearScope()
{
    g_hiddenEye.store(-1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_maskMutex);
    g_mask.eye = -1;
}

// The screen-space lens pictures (ScopeLensPicture), guarded by g_maskMutex.
ScopeLensPicture g_lens[2];
unsigned long long g_lensMs = 0;

void ClearLens()
{
    std::lock_guard<std::mutex> lock(g_maskMutex);
    g_lens[0].on = g_lens[1].on = false;
}

// Each eye but the scoped one: the lens disc (ScopeLensRadius at lensPos: the gun profile's lens offset from the optic
// zero) in its view, filled from the other eye's half around the laser's point as that eye sees it (x0, y0), angles
// divided by the magnification, anchored where THIS eye sees the laser's point (ax, ay; holographic, 2026-11-17).
//   this half: tangent x = tL + u (tR - tL), y = tU - v (tU - tD); source tangent = (x0, y0) + (x - ax, y - ay) / m
//   source frame uv: u = ((xs - tLs) / (tRs - tLs) + src) / 2, v = (tUs - ys) / (tUs - tDs)
// CENTRED ON THE LASER (2026-11-17, the user: the picture "was also not accurate to where the laser aim pointed"): the
// laser's point is where the shots land (hooks/laser_raycast.h GetLaserAimPoint), so the picture's centre shows it, from
// the source eye's own position (no parallax at any range). Without the laser running: the sight line's vanishing point,
// the same tangent in both eyes (they share the head's axes). centre = the head-centre camera the views are built from.
void UpdateLensPictures(const ScopeSettings& s, const Quat& headQ, const Vec3& headP, float scale, const Vec3& lensPos,
                        const Vec3& forward, float m, const float eyeTan[2][4], int scopedEye, const float centre[16])
{
    ScopeLensPicture pictures[2];
    const Quat inv = QuatConjugate(headQ);
    const Vec3 dirInEye = QuatRotateVector(inv, forward);
    // Not while an eye looks through the scope: its half is the scope's magnified view then, not a source for the other.
    if (s.lensPicture && scopedEye < 0 && dirInEye.z < -0.2f && m >= 1.0f)
    {
        float sourceX[2], sourceY[2]; // the picture's centre in each eye's view, as a tangent
        sourceX[0] = sourceX[1] = dirInEye.x / -dirInEye.z;
        sourceY[0] = sourceY[1] = dirInEye.y / -dirInEye.z;
        float aim[3];
        if (GetLaserAimPoint(aim))
        {
            // Into the head-centre camera's frame (rows right / up / back), then each eye is its offset along the right.
            const float d[3] = {aim[0] - centre[12], aim[1] - centre[13], aim[2] - centre[14]};
            const float right = d[0] * centre[0] + d[1] * centre[1] + d[2] * centre[2];
            const float up = d[0] * centre[4] + d[1] * centre[5] + d[2] * centre[6];
            const float back = d[0] * centre[8] + d[1] * centre[9] + d[2] * centre[10];
            if (back < -0.05f)
                for (int e = 0; e < 2; ++e)
                {
                    sourceX[e] = (right - GetEyeOffsetAlongRow0(e == 1)) / -back;
                    sourceY[e] = up / -back;
                }
        }
        for (int e = 0; e < 2; ++e)
        {
            const int src = 1 - e;
            const Vec3 eyePos = VecAdd(headP, QuatRotateVector(headQ, V(GetEyeOffsetAlongRow0(e == 1) / scale, 0.0f, 0.0f)));
            const Vec3 p = QuatRotateVector(inv, VecSub(lensPos, eyePos));
            const float dist = Length(p);
            if (p.z > -0.05f || dist < 0.05f)
                continue;
            const float lx = p.x / -p.z, ly = p.y / -p.z, lr = s.lensRadius / dist;
            const float tLd = eyeTan[e][0], tRd = eyeTan[e][1], tUd = eyeTan[e][2], tDd = eyeTan[e][3];
            const float tLs = eyeTan[src][0], tRs = eyeTan[src][1], tUs = eyeTan[src][2], tDs = eyeTan[src][3];
            if (!(tRd - tLd > 0.05f && tUd - tDd > 0.05f && tRs - tLs > 0.05f && tUs - tDs > 0.05f))
                continue;
            ScopeLensPicture& pic = pictures[e];
            // HOLOGRAPHIC (2026-11-17): magnified about the laser's point where THIS eye sees it (ax, ay), not about the
            // lens's centre (lx, ly): source tangent = (x0, y0) + (x - ax, y - ay) / m.
            const float x0 = sourceX[src], y0 = sourceY[src];
            const float ax = sourceX[e], ay = sourceY[e];
            pic.map[0] = 0.5f * (tRd - tLd) / (m * (tRs - tLs));
            pic.map[2] = 0.5f * ((x0 + (tLd - ax) / m - tLs) / (tRs - tLs) + static_cast<float>(src));
            pic.map[1] = (tUd - tDd) / (m * (tUs - tDs));
            pic.map[3] = (tUs - y0 - (tUd - ay) / m) / (tUs - tDs);
            pic.circle[0] = (lx - tLd) / (tRd - tLd);
            pic.circle[1] = (tUd - ly) / (tUd - tDd);
            pic.circle[2] = lr / (tRd - tLd);
            pic.circle[3] = (tRd - tLd) / (tUd - tDd);
            pic.reticle[0] = (ax - tLd) / (tRd - tLd);
            pic.reticle[1] = (tUd - ay) / (tUd - tDd);
            pic.reticle[3] = s.reticle ? 1.0f : 0.0f;
            pic.on = true;
        }
    }
    static bool lastOn = false;
    const bool on = pictures[0].on || pictures[1].on;
    if (on != lastOn)
        MOHW_LOG(kLogFile, "lens picture %s (%.2fx, lens %.1f cm right / %.1f cm up of the optic zero, %.1f cm ahead of the "
                 "hand, radius %.1f cm)", on ? "ON" : "off", m, s.lensRight * 100.0f, s.lensUp * 100.0f,
                 s.lensForward * 100.0f, s.lensRadius * 100.0f);
    lastOn = on;
    std::lock_guard<std::mutex> lock(g_maskMutex);
    g_lens[0] = pictures[0];
    g_lens[1] = pictures[1];
    g_lensMs = GetTickCount64();
}

// RED DOT SIGHT (2026-11-22, scope_view.h): each eye gets a red dot where IT sees the impact point -- the laser's point
// at its true direction from that eye, as the holographic crosshair (the sight line's vanishing point without the laser)
// -- drawn only inside the lens disc as that eye sees it: the lens is the sight's window. No picture, no magnification.
// Kept in g_lens with reticle.w = 2 (hooks/shaders/scope_lens.hlsl draws the dot) and reticle.z = the dot's radius.
std::atomic<unsigned long long> g_redDotMs{0};

void UpdateRedDots(const ScopeSettings& s, const Quat& headQ, const Vec3& headP, float scale, const Vec3& lensPos,
                   const Vec3& forward, const float eyeTan[2][4], const float centre[16])
{
    ScopeLensPicture dots[2];
    const Quat inv = QuatConjugate(headQ);
    const Vec3 dirInEye = QuatRotateVector(inv, forward);
    if (dirInEye.z < -0.2f)
    {
        float dotX[2], dotY[2];
        dotX[0] = dotX[1] = dirInEye.x / -dirInEye.z;
        dotY[0] = dotY[1] = dirInEye.y / -dirInEye.z;
        float aim[3];
        if (GetLaserAimPoint(aim))
        {
            const float d[3] = {aim[0] - centre[12], aim[1] - centre[13], aim[2] - centre[14]};
            const float right = d[0] * centre[0] + d[1] * centre[1] + d[2] * centre[2];
            const float up = d[0] * centre[4] + d[1] * centre[5] + d[2] * centre[6];
            const float back = d[0] * centre[8] + d[1] * centre[9] + d[2] * centre[10];
            if (back < -0.05f)
                for (int e = 0; e < 2; ++e)
                {
                    dotX[e] = (right - GetEyeOffsetAlongRow0(e == 1)) / -back;
                    dotY[e] = up / -back;
                }
        }
        float size = s.redDotSizeDeg;
        if (!(size > 0.0f))
            size = 0.15f;
        const float dotTan = std::tan(size * 0.5f * 3.14159265f / 180.0f);
        for (int e = 0; e < 2; ++e)
        {
            const Vec3 eyePos = VecAdd(headP, QuatRotateVector(headQ, V(GetEyeOffsetAlongRow0(e == 1) / scale, 0.0f, 0.0f)));
            const Vec3 p = QuatRotateVector(inv, VecSub(lensPos, eyePos));
            const float dist = Length(p);
            if (p.z > -0.05f || dist < 0.05f)
                continue;
            const float lx = p.x / -p.z, ly = p.y / -p.z, lr = s.lensRadius / dist;
            const float tL = eyeTan[e][0], tR = eyeTan[e][1], tU = eyeTan[e][2], tD = eyeTan[e][3];
            if (!(tR - tL > 0.05f && tU - tD > 0.05f))
                continue;
            // Only when the dot is in the window (with its own radius of slack): the shader clips it to the lens anyway.
            const float ox = (dotX[e] - lx) / lr, oy = (dotY[e] - ly) / lr, slack = 1.0f + dotTan / lr;
            if (ox * ox + oy * oy > slack * slack)
                continue;
            ScopeLensPicture& dot = dots[e];
            dot.circle[0] = (lx - tL) / (tR - tL);
            dot.circle[1] = (tU - ly) / (tU - tD);
            dot.circle[2] = lr / (tR - tL);
            dot.circle[3] = (tR - tL) / (tU - tD);
            dot.reticle[0] = (dotX[e] - tL) / (tR - tL);
            dot.reticle[1] = (tU - dotY[e]) / (tU - tD);
            dot.reticle[2] = dotTan / (tR - tL);
            dot.reticle[3] = 2.0f;
            dot.on = true;
        }
    }
    g_redDotMs.store(GetTickCount64(), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_maskMutex);
    g_lens[0] = dots[0];
    g_lens[1] = dots[1];
    g_lensMs = GetTickCount64();
}

} // namespace

bool IsRedDotSightUp()
{
    return GetTickCount64() - g_redDotMs.load(std::memory_order_relaxed) <= kRecentMs;
}

bool GetScopeLensPictures(ScopeLensPicture out[2])
{
    std::lock_guard<std::mutex> lock(g_maskMutex);
    if (GetTickCount64() - g_lensMs > kRecentMs || !(g_lens[0].on || g_lens[1].on))
        return false;
    out[0] = g_lens[0];
    out[1] = g_lens[1];
    return true;
}

bool ComputeScopedEye(const float centre[16], const float eyeTan[2][4], ScopeEyeView* out)
{
    const ScopeSettings s = GetScopeSettings();
    // The gun's own scope (its profile; 2026-11-17: the game's ADS zoom no longer stands in for it, so it needs no ADS),
    // for a red dot with a flip magnifier only while the game's magnifier is switched on (its state holds out of ADS).
    float magnification = s.magnification;
    bool magnifierOn = false;
    const bool magnifierKnown = GetEquippedOpticState(&magnifierOn, nullptr);
    const bool magnifierOff = s.needsMagnifier && !(magnifierKnown && magnifierOn);
    if (magnifierOff)
        magnification = 0.0f;
    // A red dot sight (the profile's SightType, 2026-11-22): the dot, unless the game's magnifier turns it into the scope
    // ("Scope shows: only with the game's magnifier on", with a magnification).
    const bool magnified = s.needsMagnifier && magnifierKnown && magnifierOn && s.magnification >= 1.0f;
    const bool redDot = s.redDot && !magnified;

    static int lastEye = -2;
    static unsigned long long nextLogMs = 0;
    const unsigned long long now = GetTickCount64();
    const auto logDue = [&]() {
        if (now < nextLogMs)
            return false;
        nextLogMs = now + 2000;
        return true;
    };

    if ((!redDot && ((!s.enabled && !s.lensPicture) || magnification < 1.0f)) || !GetWeaponDriveEnabled() ||
        !IsPlayerAimActive() || GetPlayerAimDriveWeight() < 1.0f)
    {
        if (lastEye != -1)
            MOHW_LOG(kLogFile, "scope off (%s)",
                     redDot                          ? "red dot: gun not on the controller"
                     : (!s.enabled && !s.lensPicture) ? "settings off"
                     : magnifierOff                ? "the game's magnifier is off"
                     : magnification < 1.0f        ? "this gun has no scope magnification"
                                                   : "gun not on the controller");
        lastEye = -1;
        ClearScope();
        ClearLens();
        return false;
    }

    Quat headQ{};
    Vec3 headP{};
    mohwvr::ipc::ControllerPoseBlock hand{};
    if (!ShownHead(&headQ, &headP) || !GetRightControllerPose(&hand))
    {
        ClearScope();
        ClearLens();
        return false;
    }
    float scale = GetHeadPositionScale();
    if (!(scale > 0.01f))
        scale = 1.0f;
    // The sight line, tracking space: the weapon hand's optic zero, along its forward.
    const Quat handQ{hand.orientationX, hand.orientationY, hand.orientationZ, hand.orientationW};
    float height = 0.0f, windage = 0.0f;
    GetOpticOffset(&height, &windage);
    const Vec3 origin = VecAdd(V(hand.positionX, hand.positionY, hand.positionZ), QuatRotateVector(handQ, V(windage, height, 0.0f)));
    const Vec3 forward = Normalized(QuatRotateVector(handQ, V(0.0f, 0.0f, -1.0f)));

    if (redDot)
    {
        if (lastEye != -3)
            MOHW_LOG(kLogFile, "RED DOT SIGHT (lens %.1f cm right / %.1f cm up of the optic zero, %.1f cm ahead of the "
                     "hand, radius %.1f cm; dot %.2f deg)", s.lensRight * 100.0f, s.lensUp * 100.0f,
                     s.lensForward * 100.0f, s.lensRadius * 100.0f, s.redDotSizeDeg);
        lastEye = -3;
        ClearScope();
        const Vec3 window = VecAdd(origin, QuatRotateVector(handQ, V(s.lensRight, s.lensUp, -s.lensForward)));
        UpdateRedDots(s, headQ, headP, scale, window, forward, eyeTan, centre);
        return false;
    }

    // Which eye is behind it: the one nearest the line, within the eye box, behind the hand. HYSTERESIS (2026-11-18, the
    // user: standing still "caused the camera to flicker in and out until I moved"): the eye already behind the scope
    // stays there until it's kStayFactor past the box, so a hand resting on the box's edge doesn't flip the view between
    // the scope and the eye's own every few frames.
    constexpr float kStayFactor = 1.25f;
    int eye = -1;
    float bestPerp = 1e9f, bestAlong = 0.0f;
    for (int e = 0; e < 2; ++e)
    {
        const Vec3 eyePos = VecAdd(headP, QuatRotateVector(headQ, V(GetEyeOffsetAlongRow0(e == 1) / scale, 0.0f, 0.0f)));
        const Vec3 v = VecSub(eyePos, origin);
        const float along = VecDot(v, forward);
        const float perp = Length(VecSub(v, VecScale(forward, along)));
        const float stay = e == lastEye ? kStayFactor : 1.0f;
        if (s.enabled && along <= 0.05f * stay && along >= -s.relief * stay && perp <= s.eyeBox * stay && perp < bestPerp)
        {
            eye = e;
            bestPerp = perp;
            bestAlong = along;
        }
    }
    if (eye != lastEye)
    {
        if (eye >= 0)
            MOHW_LOG(kLogFile, "SCOPED EYE: %s eye behind the scope, %.1f cm off the sight line, %.1f cm behind the hand; %.2fx",
                     eye == 0 ? "left" : "right", bestPerp * 100.0f, -bestAlong * 100.0f, magnification);
        else
            MOHW_LOG(kLogFile, "scoped eye: no eye behind the scope (%.2fx ready)", magnification);
        lastEye = eye;
    }
    // The lens disc on this gun: from the optic zero, ScopeLensRight / ScopeLensUp in the hand's frame, and ScopeLensForward
    // ahead of the hand (the gun's profile).
    const Vec3 lensPos = VecAdd(origin, QuatRotateVector(handQ, V(s.lensRight, s.lensUp, -s.lensForward)));
    UpdateLensPictures(s, headQ, headP, scale, lensPos, forward, magnification, eyeTan, eye, centre);
    if (eye < 0)
    {
        ClearScope();
        return false;
    }

    // The scope camera: on the sight line level with the eye, looking along it, with the eye's up.
    const Vec3 camPos = VecAdd(origin, VecScale(forward, bestAlong));
    const Vec3 headUp = QuatRotateVector(headQ, V(0.0f, 1.0f, 0.0f));
    Vec3 up = Normalized(VecSub(headUp, VecScale(forward, VecDot(headUp, forward))));
    const Vec3 back = VecScale(forward, -1.0f);
    const Vec3 right = Normalized(CrossV(up, back));
    up = CrossV(back, right);
    // Into the game's world through the head-centre camera: a tracking point p is at Q^-1 (p - head) * scale in that
    // camera's frame (x right, y up, z back), then through its rows.
    const Quat inv = QuatConjugate(headQ);
    const auto toWorldDir = [&](const Vec3& d, float w[3]) {
        const Vec3 l = QuatRotateVector(inv, d);
        for (int k = 0; k < 3; ++k)
            w[k] = l.x * centre[k] + l.y * centre[4 + k] + l.z * centre[8 + k];
    };
    const Vec3 rel = QuatRotateVector(inv, VecSub(camPos, headP));
    float* m = out->camera;
    toWorldDir(right, m + 0);
    toWorldDir(up, m + 4);
    toWorldDir(back, m + 8);
    m[3] = m[7] = m[11] = 0.0f;
    for (int k = 0; k < 3; ++k)
        m[12 + k] = centre[12 + k] + (rel.x * centre[k] + rel.y * centre[4 + k] + rel.z * centre[8 + k]) * scale;
    m[15] = 1.0f;

    // Where the sight line meets the eye's view (its tangent plane: the eye's axes are the head's).
    const Vec3 dirInEye = QuatRotateVector(inv, forward);
    if (dirInEye.z > -0.2f) // pointing well away from where the eye looks
    {
        ClearScope();
        return false;
    }
    const float x0 = dirInEye.x / -dirInEye.z, y0 = dirInEye.y / -dirInEye.z;
    const float tL = eyeTan[eye][0], tR = eyeTan[eye][1], tU = eyeTan[eye][2], tD = eyeTan[eye][3];
    // HOLOGRAPHIC (2026-11-17, the user: "wherever you see the reticle is the impact point"): the laser's point -- where
    // the shots land -- as the scope camera sees it (ax, ay), shown where the EYE sees it (lx, ly): the magnified view is
    // anchored on the impact point's true direction from this eye, which is also where the SteamVR laser dot overlay
    // sits, and the crosshair goes there. So the reticle stays on the impact point whatever the eye's offset from the
    // sight line (it moves a little inside the eyepiece instead), and the sight line's parallax from the bore is taken
    // out at whatever range the laser hits. Without the laser running: the sight line (the eyepiece's centre).
    float ax = 0.0f, ay = 0.0f, lx = x0, ly = y0;
    float aim[3];
    if (GetLaserAimPoint(aim))
    {
        const float d[3] = {aim[0] - m[12], aim[1] - m[13], aim[2] - m[14]};
        const float cx = d[0] * m[0] + d[1] * m[1] + d[2] * m[2];
        const float cy = d[0] * m[4] + d[1] * m[5] + d[2] * m[6];
        const float cz = d[0] * m[8] + d[1] * m[9] + d[2] * m[10];
        // The same point from the eye: the head-centre camera's frame (rows right / up / back), the eye along its right.
        const float e[3] = {aim[0] - centre[12], aim[1] - centre[13], aim[2] - centre[14]};
        const float er = e[0] * centre[0] + e[1] * centre[1] + e[2] * centre[2] - GetEyeOffsetAlongRow0(eye == 1);
        const float eu = e[0] * centre[4] + e[1] * centre[5] + e[2] * centre[6];
        const float eb = e[0] * centre[8] + e[1] * centre[9] + e[2] * centre[10];
        constexpr float kMaxOffset = 0.25f; // ~14 deg: past that the laser is on something else, not the target
        if (cz < -0.05f && eb < -0.05f && std::fabs(cx / cz) < kMaxOffset && std::fabs(cy / cz) < kMaxOffset &&
            std::fabs(er / eb - x0) < kMaxOffset && std::fabs(eu / eb - y0) < kMaxOffset)
        {
            ax = cx / -cz;
            ay = cy / -cz;
            lx = er / -eb;
            ly = eu / -eb;
        }
    }
    out->eye = eye;
    if (!FrustumFromTangents((tL - lx) / magnification + ax, (tR - lx) / magnification + ax,
                             (tU - ly) / magnification + ay, (tD - ly) / magnification + ay, out))
    {
        ClearScope();
        return false;
    }

    // The eyepiece in the eye's half, narrowing as the eye leaves the line (exit pupil).
    const float pupil = 1.0f - 0.6f * (bestPerp / s.eyeBox) * (bestPerp / s.eyeBox);
    const float r = std::tan(s.apparentFovDeg * 0.5f * 3.14159265f / 180.0f) * pupil;
    {
        std::lock_guard<std::mutex> lock(g_maskMutex);
        g_mask.eye = eye;
        g_mask.cx = (x0 - tL) / (tR - tL);
        g_mask.cy = (tU - y0) / (tU - tD);
        g_mask.rx = r / (tR - tL);
        g_mask.ry = r / (tU - tD);
        g_mask.reticle = s.reticle;
        g_mask.reticleX = (lx - tL) / (tR - tL);
        g_mask.reticleY = (tU - ly) / (tU - tD);
        g_mask.ms = now;
    }
    g_hiddenEye.store(eye, std::memory_order_relaxed);
    g_hiddenEyeMs.store(now, std::memory_order_relaxed);
    if (logDue())
        MOHW_LOG(kLogFile,
                 "scope view: %s eye, %.2fx, sight line at tan (%.3f, %.3f) in the eye's view, eyepiece radius tan %.3f; "
                 "frustum fovY %.2f deg aspect %.3f offset (%.3f, %.3f)",
                 eye == 0 ? "left" : "right", magnification, x0, y0, r, out->fovY * 57.29578f, out->aspect, out->offX,
                 out->offY);
    return true;
}

bool GetScopeMask(int* eye, float* cx, float* cy, float* radiusX, float* radiusY, bool* reticle, float* reticleX,
                  float* reticleY)
{
    std::lock_guard<std::mutex> lock(g_maskMutex);
    if (g_mask.eye < 0 || GetTickCount64() - g_mask.ms > kRecentMs)
        return false;
    *eye = g_mask.eye;
    *cx = g_mask.cx;
    *cy = g_mask.cy;
    *radiusX = g_mask.rx;
    *radiusY = g_mask.ry;
    *reticle = g_mask.reticle;
    *reticleX = g_mask.reticleX;
    *reticleY = g_mask.reticleY;
    return true;
}

int GetScopeHiddenEye()
{
    if (GetTickCount64() - g_hiddenEyeMs.load(std::memory_order_relaxed) > kRecentMs)
        return -1;
    return g_hiddenEye.load(std::memory_order_relaxed);
}

} // namespace mohw
