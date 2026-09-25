#include "alternating_eye.h"

#include "../sdk/logging.h"
#include "../sdk/settings.h"
#include "../sdk/vr_math.h"

#include <windows.h>
#include <atomic>
#include <cstring>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_alternatingeye.log";

std::atomic<bool> g_rightEyeActive{false};

// STALENESS ISOLATION TEST (2026-09-24) state -- see alternating_eye.h's IsStalenessIsolationTestActive comment.
std::atomic<bool> g_stalenessIsolationTestActive{false};

// Same SEH-safe-dereference helpers as fov_scale_hook.cpp -- kept local rather than shared, same per-file
// duplication convention already established for these (the __try/__except-can't-coexist-with-object-unwinding
// constraint means each caller needs its own tiny wrapper with no C++ objects in scope).
bool SehSafeReadFloats(float* dst, const void* src, int count)
{
    __try
    {
        memcpy(dst, src, static_cast<size_t>(count) * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SehSafeWriteFloats(void* dst, const float* src, int count)
{
    __try
    {
        memcpy(dst, src, static_cast<size_t>(count) * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void LogOffsetIfDue(bool rightEye, const Vec3& transOld, const Vec3& transNew)
{
    static std::atomic<unsigned long long> nextLogAllowedMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogAllowedMs.load(std::memory_order_relaxed);
    if (now >= allowed && nextLogAllowedMs.compare_exchange_strong(allowed, now + 1000, std::memory_order_relaxed))
        MOHW_LOG(kLogFile, "eye offset applied: eye=%s trans (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f)",
                  rightEye ? "RIGHT" : "LEFT", transOld.x, transOld.y, transOld.z, transNew.x, transNew.y,
                  transNew.z);
}

} // namespace

bool IsStalenessIsolationTestActive()
{
    return g_stalenessIsolationTestActive.load(std::memory_order_relaxed);
}

bool IsRightEyeActive()
{
    return g_rightEyeActive.load(std::memory_order_relaxed);
}

void AdvanceEyeToNextFrame()
{
    g_rightEyeActive.store(!g_rightEyeActive.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

bool ApplyEyeOffset(void* transformPtr)
{
    float raw[16]{};
    if (!SehSafeReadFloats(raw, transformPtr, 16))
        return false;

    Vec3 left{};
    left.x = raw[0];
    left.y = raw[1];
    left.z = raw[2];
    Vec3 trans{};
    trans.x = raw[12];
    trans.y = raw[13];
    trans.z = raw[14];

    // Sanity check, same philosophy as fov_scale_hook.cpp's ApplyHeadRoll --
    // reject rather than corrupt a non-camera transform.
    float leftLenSq = VecDot(left, left);
    if (leftLenSq < 0.5f || leftLenSq > 2.0f)
        return false;

    // Sign convention: left eye -IPD/2, right eye +IPD/2, both along "left". A straight world-space position
    // delta does NOT need inverting despite the engine inverting this world transform into a view matrix
    // downstream: for view = inverse([R|t]) = [R^T | -R^T*t], shifting t by +worldDelta shifts the view-space
    // translation by the intuitive -R^T*worldDelta, no extra sign flip required. raw[0..2] ("left") here is the
    // same world-space left axis the constant buffer's view-matrix "left" row is derived from (this hook's
    // transform IS the source the engine builds that view matrix from), so it points the same real-world
    // direction either way.
    bool rightEye = IsRightEyeActive();
    float eyeSign = rightEye ? 1.0f : -1.0f;

    // kBaseIpdMeters (average human IPD) times the user-tunable GetIpdScale() (F7/F8, sdk/settings.h).
    constexpr float kBaseIpdMeters = 0.064f;
    float ipdMeters = kBaseIpdMeters * GetIpdScale();

    Vec3 newTrans = VecAdd(trans, VecScale(left, eyeSign * ipdMeters * 0.5f));

    LogOffsetIfDue(rightEye, trans, newTrans);

    raw[12] = newTrans.x;
    raw[13] = newTrans.y;
    raw[14] = newTrans.z;

    return SehSafeWriteFloats(transformPtr, raw, 16);
}

// Alternating-eye rendering is always on (2026-09-20): the Insert toggle was removed -- Insert is
// now the companion's view-mode key, and a stray press used to flip and SAVE this setting. Kept
// as a no-op (beyond the F7/F8 IPD-scale poll below) so present_hook.cpp's call site is unchanged.
void CheckAlternatingEyeHotkeys()
{
    // F7/F8: IPD scale step (migrated from the retired draw-duplication path, which shared this same
    // GetIpdScale()/SetIpdScale() value). Both go through sdk/settings.h's SetIpdScale, so a press also persists
    // to mohwvr_settings.ini.
    constexpr float kIpdScaleStep = 0.1f;
    constexpr float kIpdScaleMin = 0.5f;
    constexpr float kIpdScaleMax = 1.5f;

    static bool ipdDownWasDown = false;
    bool ipdDownDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (ipdDownDown && !ipdDownWasDown)
    {
        float newScale = GetIpdScale() - kIpdScaleStep;
        if (newScale < kIpdScaleMin)
            newScale = kIpdScaleMin;
        SetIpdScale(newScale);
        MOHW_LOG(kLogFile, "F7 pressed -- IPD scale now %.2fx (saved to mohwvr_settings.ini)", newScale);
    }
    ipdDownWasDown = ipdDownDown;

    static bool ipdUpWasDown = false;
    bool ipdUpDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (ipdUpDown && !ipdUpWasDown)
    {
        float newScale = GetIpdScale() + kIpdScaleStep;
        if (newScale > kIpdScaleMax)
            newScale = kIpdScaleMax;
        SetIpdScale(newScale);
        MOHW_LOG(kLogFile, "F8 pressed -- IPD scale now %.2fx (saved to mohwvr_settings.ini)", newScale);
    }
    ipdUpWasDown = ipdUpDown;

    // STALENESS ISOLATION TEST (2026-09-24), CAPS LOCK -- see IsStalenessIsolationTestActive's declaration
    // comment. Caps Lock is a toggle key itself, but GetAsyncKeyState's high bit still reflects physical down/up
    // state fine for edge detection, and it's essentially never a real gameplay bind in an FPS.
    static bool capsWasDown = false;
    bool capsDown = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
    if (capsDown && !capsWasDown)
    {
        bool newValue = !g_stalenessIsolationTestActive.load(std::memory_order_relaxed);
        g_stalenessIsolationTestActive.store(newValue, std::memory_order_relaxed);
        MOHW_LOG(kLogFile,
                  "CAPS LOCK pressed -- staleness isolation test %s (right eye's texture now updates every frame, "
                  "content will look wrong ~half the time -- this is a diagnostic, not a fix)",
                  newValue ? "ON" : "back to normal");
    }
    capsWasDown = capsDown;
}

} // namespace mohw
