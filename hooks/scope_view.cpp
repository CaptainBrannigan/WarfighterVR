#include "scope_view.h"

#include "aiming_controller_hook.h"
#include "alternating_eye.h"
#include "companion_bridge.h"
#include "render_pose_stamp.h"
#include "../openvr_direct/vr_input.h"
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

// The game's hip FOV: its view FOV while ADS isn't held, eased (it animates in and out of ADS).
float g_hipFovY = 0.0f; // game thread

std::mutex g_maskMutex;
struct Mask
{
    int eye = -1;
    float cx = 0.5f, cy = 0.5f, rx = 0.1f, ry = 0.1f;
    bool reticle = false;
    unsigned long long ms = 0;
};
Mask g_mask; // guarded by g_maskMutex
std::atomic<int> g_hiddenEye{-1};
std::atomic<unsigned long long> g_hiddenEyeMs{0};
std::atomic<unsigned long long> g_gameReticleMs{0}; // NoteScopeGameReticle

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

} // namespace

bool ComputeScopedEye(const float centre[16], const float eyeTan[2][4], float gameFovY, ScopeEyeView* out)
{
    const ScopeSettings s = GetScopeSettings();
    // The game's zoom: its FOV now against its hip FOV (learned while ADS isn't held).
    openvr_direct::VrActionState vr{};
    const bool haveVr = openvr_direct::GetVrActionState(&vr);
    const bool ads = haveVr && vr.ads;
    if (gameFovY > 0.05f && gameFovY < 3.0f)
    {
        if (g_hipFovY <= 0.0f)
            g_hipFovY = gameFovY;
        else if (!ads)
            g_hipFovY += (gameFovY - g_hipFovY) * 0.2f;
    }
    float gameZoom = 1.0f;
    if (g_hipFovY > 0.05f && gameFovY > 0.01f && gameFovY < g_hipFovY)
        gameZoom = std::tan(g_hipFovY * 0.5f) / std::tan(gameFovY * 0.5f);
    const float magnification = s.magnification > 0.0f ? s.magnification : (ads && gameZoom >= s.minZoom ? gameZoom : 0.0f);

    static int lastEye = -2;
    static unsigned long long nextLogMs = 0;
    const unsigned long long now = GetTickCount64();
    const auto logDue = [&]() {
        if (now < nextLogMs)
            return false;
        nextLogMs = now + 2000;
        return true;
    };

    if (!s.enabled || magnification < 1.0f || !GetWeaponDriveEnabled() || !IsPlayerAimActive() ||
        GetPlayerAimDriveWeight() < 1.0f)
    {
        if (lastEye != -1)
            MOHW_LOG(kLogFile, "scoped eye off (%s; game FOV %.1f deg, hip %.1f deg, zoom %.2fx, ADS %d)",
                     !s.enabled ? "setting off" : magnification < 1.0f ? "no scope magnification" : "gun not on the controller",
                     gameFovY * 57.29578f, g_hipFovY * 57.29578f, gameZoom, ads ? 1 : 0);
        lastEye = -1;
        ClearScope();
        return false;
    }

    Quat headQ{};
    Vec3 headP{};
    mohwvr::ipc::ControllerPoseBlock hand{};
    if (!ShownHead(&headQ, &headP) || !GetRightControllerPose(&hand))
    {
        ClearScope();
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

    // Which eye is behind it: the one nearest the line, within the eye box, behind the hand.
    int eye = -1;
    float bestPerp = 1e9f, bestAlong = 0.0f;
    for (int e = 0; e < 2; ++e)
    {
        const Vec3 eyePos = VecAdd(headP, QuatRotateVector(headQ, V(GetEyeOffsetAlongRow0(e == 1) / scale, 0.0f, 0.0f)));
        const Vec3 v = VecSub(eyePos, origin);
        const float along = VecDot(v, forward);
        const float perp = Length(VecSub(v, VecScale(forward, along)));
        if (along <= 0.05f && along >= -s.relief && perp <= s.eyeBox && perp < bestPerp)
        {
            eye = e;
            bestPerp = perp;
            bestAlong = along;
        }
    }
    if (eye != lastEye)
    {
        if (eye >= 0)
            MOHW_LOG(kLogFile, "SCOPED EYE: %s eye behind the scope, %.1f cm off the sight line, %.1f cm behind the hand; %.2fx (%s)",
                     eye == 0 ? "left" : "right", bestPerp * 100.0f, -bestAlong * 100.0f, magnification,
                     s.magnification > 0.0f ? "the weapon's" : "the game's ADS zoom");
        else
            MOHW_LOG(kLogFile, "scoped eye: no eye behind the scope (%.2fx ready)", magnification);
        lastEye = eye;
    }
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
    out->eye = eye;
    if (!FrustumFromTangents((tL - x0) / magnification, (tR - x0) / magnification, (tU - y0) / magnification,
                             (tD - y0) / magnification, out))
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

bool GetScopeMask(int* eye, float* cx, float* cy, float* radiusX, float* radiusY, bool* reticle)
{
    std::lock_guard<std::mutex> lock(g_maskMutex);
    if (g_mask.eye < 0 || GetTickCount64() - g_mask.ms > kRecentMs)
        return false;
    *eye = g_mask.eye;
    *cx = g_mask.cx;
    *cy = g_mask.cy;
    *radiusX = g_mask.rx;
    *radiusY = g_mask.ry;
    // The game's own reticle in the view (its zoom optic): no drawn one over it.
    *reticle = g_mask.reticle && GetTickCount64() - g_gameReticleMs.load(std::memory_order_relaxed) > kRecentMs;
    return true;
}

int GetScopeHiddenEye()
{
    if (GetTickCount64() - g_hiddenEyeMs.load(std::memory_order_relaxed) > kRecentMs)
        return -1;
    return g_hiddenEye.load(std::memory_order_relaxed);
}

void NoteScopeGameReticle()
{
    const unsigned long long now = GetTickCount64();
    if (now - g_gameReticleMs.exchange(now, std::memory_order_relaxed) > 2000)
        MOHW_LOG(kLogFile, "the game's zoom optic (its reticle) is drawn in the scoped eye: the crosshair stands aside");
}

} // namespace mohw
