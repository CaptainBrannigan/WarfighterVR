#include "settings.h"

#include "logging.h" // reuses LogFilePath()'s "next to the DLL" resolution

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_settings.log";
// Baseline confirmed live 2026-08-04 (see docs/STATUS.md's Current status)
// -- FOV scale 2.2x + IPD scale 3.5x together produced a real, perceptible
// sense of depth for the first time. These are the values a fresh install
// (no settings file yet) starts from.
constexpr float kDefaultFovScale = 1.6f; // defaults below updated 2026-09-20 to the values the user has tuned live
constexpr float kDefaultIpdScale = 1.0f;
// Head-aim defaults to OFF (unlike FOV/IPD) -- see settings.h's declaration
// comment for why. 1.0 sensitivity is an untested starting guess, not a
// calibrated value.
constexpr bool kDefaultHeadAimEnabled = true;
constexpr float kDefaultHeadAimSensitivity = 1.0f;
// CONFIRMED live (2026-08-22, with commit_view_transform_hook.cpp's
// render rotation temporarily disabled to isolate this hook): both axes
// need inverting for aim/movement direction to correctly follow the HMD.
// Was: default false/false (unconfirmed axis convention, flip live via
// hotkey if backwards) -- now the confirmed-correct default.
constexpr bool kDefaultHeadAimInvertYaw = true;
constexpr bool kDefaultHeadAimInvertPitch = true;
constexpr bool kDefaultHeadAimClampPitch = true;
constexpr bool kDefaultHeadRollEnabled = true;
constexpr bool kDefaultHeadRollInvert = false;
constexpr float kDefaultFrustumScale = 1.0f;
constexpr bool kDefaultHeadPositionEnabled = true;
constexpr float kDefaultHeadPositionScale = 1.0f;
constexpr bool kDefaultHeadPositionInvertHorizontal = true;
constexpr bool kDefaultRotationSmoothingEnabled = true;
constexpr float kDefaultRotationSmoothingWindowMs = 83.3333f; // user-tuned (the raw ~30Hz tick is 33.3ms)
constexpr bool kDefaultBoneHideEnabled = true;
constexpr float kDefaultBoneHideRangeStart = 12.0f;
constexpr float kDefaultBoneHideRangeEnd = -22.0f;
constexpr float kDefaultBoneHideRange2Start = 5.0f;
constexpr float kDefaultBoneHideRange2End = 5.0f;
// Tighter than the constant this replaces (5.0) -- see
// GetPlayerBoneDistanceThreshold's declaration comment: the confirmed true
// player-distance range (0.48-1.9) overlaps 1-2m false positives, so this
// is a starting point to dial in live, not a solved value.
constexpr float kDefaultPlayerBoneDistanceThreshold = 6.2f;

std::atomic<float> g_fovScale{kDefaultFovScale};
std::atomic<float> g_ipdScale{kDefaultIpdScale};
std::atomic<bool> g_headAimEnabled{kDefaultHeadAimEnabled};
std::atomic<float> g_headAimSensitivity{kDefaultHeadAimSensitivity};
std::atomic<bool> g_headAimInvertYaw{kDefaultHeadAimInvertYaw};
std::atomic<bool> g_headAimInvertPitch{kDefaultHeadAimInvertPitch};
std::atomic<bool> g_headAimClampPitch{kDefaultHeadAimClampPitch};
std::atomic<bool> g_headRollEnabled{kDefaultHeadRollEnabled};
std::atomic<bool> g_headRollInvert{kDefaultHeadRollInvert};
std::atomic<float> g_frustumScale{kDefaultFrustumScale};
std::atomic<bool> g_headPositionEnabled{kDefaultHeadPositionEnabled};
std::atomic<float> g_headPositionScale{kDefaultHeadPositionScale};
std::atomic<bool> g_headPositionInvertHorizontal{kDefaultHeadPositionInvertHorizontal};
std::atomic<bool> g_rotationSmoothingEnabled{kDefaultRotationSmoothingEnabled};
std::atomic<float> g_rotationSmoothingWindowMs{kDefaultRotationSmoothingWindowMs};
std::atomic<bool> g_boneHideEnabled{kDefaultBoneHideEnabled};
std::atomic<float> g_boneHideRangeStart{kDefaultBoneHideRangeStart};
std::atomic<float> g_boneHideRangeEnd{kDefaultBoneHideRangeEnd};
std::atomic<float> g_boneHideRange2Start{kDefaultBoneHideRange2Start};
std::atomic<float> g_boneHideRange2End{kDefaultBoneHideRange2End};
std::atomic<float> g_playerBoneDistanceThreshold{kDefaultPlayerBoneDistanceThreshold};
// Guards the settings FILE's read-modify-write access -- unrelated to the
// atomics above, which are already independently safe for concurrent
// get/set from the render/hotkey-poll threads.
std::mutex g_fileMutex;

std::string SettingsFilePath()
{
    return LogFilePath("mohwvr_settings.ini"); // same "next to the DLL" resolution logging.h already uses
}

void WriteSettingsFileLocked()
{
    std::string path = SettingsFilePath();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f)
    {
        MOHW_LOG(kLogFile, "WriteSettingsFileLocked: failed to open %s for writing", path.c_str());
        return;
    }
    fprintf(f, "; MOHWVR settings -- edit while the game is closed, or use the\n");
    fprintf(f, "; in-game hotkeys, which keep this file in sync automatically:\n");
    fprintf(f, ";   F5/F6  = FOV scale down/up\n");
    fprintf(f, ";   F7/F8  = IPD scale down/up\n");
    fprintf(f, ";   F12    = head-driven aim/movement on/off (TEMPORARILY DISABLED -- see\n");
    fprintf(f, ";            hooks/aiming_controller_hook.cpp's CheckAimingControllerHotkeys)\n");
    fprintf(f, ";   F2     = head-aim sensitivity up (TEMPORARILY DISABLED, same reason)\n");
    fprintf(f, ";   F3     = head-aim recenter\n");
    fprintf(f, ";   F4     = head-aim invert yaw toggle\n");
    fprintf(f, ";   F1     = head-aim invert pitch toggle\n");
    fprintf(f, ";   (HeadAimClampPitch below has no hotkey yet -- edit the value in\n");
    fprintf(f, ";    this file directly and relaunch to change it)\n");
    fprintf(f, "FovScale=%.4f\n", g_fovScale.load(std::memory_order_relaxed));
    fprintf(f, "IpdScale=%.4f\n", g_ipdScale.load(std::memory_order_relaxed));
    fprintf(f, "HeadAimEnabled=%d\n", g_headAimEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HeadAimSensitivity=%.4f\n", g_headAimSensitivity.load(std::memory_order_relaxed));
    fprintf(f, "HeadAimInvertYaw=%d\n", g_headAimInvertYaw.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HeadAimInvertPitch=%d\n", g_headAimInvertPitch.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HeadAimClampPitch=%d\n", g_headAimClampPitch.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HeadRollEnabled=%d\n", g_headRollEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HeadRollInvert=%d\n", g_headRollInvert.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "RotationSmoothingEnabled=%d\n", g_rotationSmoothingEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "RotationSmoothingWindowMs=%.4f\n", g_rotationSmoothingWindowMs.load(std::memory_order_relaxed));
    fprintf(f, "BoneHideEnabled=%d\n", g_boneHideEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "BoneHideRangeStart=%.4f\n", g_boneHideRangeStart.load(std::memory_order_relaxed));
    fprintf(f, "BoneHideRangeEnd=%.4f\n", g_boneHideRangeEnd.load(std::memory_order_relaxed));
    fprintf(f, "BoneHideRange2Start=%.4f\n", g_boneHideRange2Start.load(std::memory_order_relaxed));
    fprintf(f, "BoneHideRange2End=%.4f\n", g_boneHideRange2End.load(std::memory_order_relaxed));
    fprintf(f, "PlayerBoneDistanceThreshold=%.4f\n", g_playerBoneDistanceThreshold.load(std::memory_order_relaxed));
    fclose(f);
}

// Minimal, deliberately unambitious line-based Key=Value parser -- no
// sections, no quoting, no escaping. Sufficient for this file's tiny,
// fully-controlled scope (a file we ourselves wrote the shape of); not
// meant to be a general-purpose INI reader.
bool ParseFloatSetting(const std::string& line, const char* key, float* outValue)
{
    size_t keyLen = strlen(key);
    if (line.size() <= keyLen + 1)
        return false;
    if (line.compare(0, keyLen, key) != 0)
        return false;
    if (line[keyLen] != '=')
        return false;
    *outValue = static_cast<float>(atof(line.c_str() + keyLen + 1));
    return true;
}

bool ParseBoolSetting(const std::string& line, const char* key, bool* outValue)
{
    float asFloat = 0.0f;
    if (!ParseFloatSetting(line, key, &asFloat))
        return false;
    *outValue = (asFloat != 0.0f);
    return true;
}

} // namespace

void LoadSettings()
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    std::string path = SettingsFilePath();

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f)
    {
        MOHW_LOG(kLogFile,
                  "LoadSettings: %s not found, creating with defaults (FovScale=%.2f IpdScale=%.2f "
                  "HeadAimEnabled=%d HeadAimSensitivity=%.2f)",
                  path.c_str(), kDefaultFovScale, kDefaultIpdScale, kDefaultHeadAimEnabled ? 1 : 0,
                  kDefaultHeadAimSensitivity);
        WriteSettingsFileLocked();
        return;
    }

    char lineBuf[256];
    while (fgets(lineBuf, sizeof(lineBuf), f))
    {
        std::string line(lineBuf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        float value = 0.0f;
        bool boolValue = false;
        if (ParseFloatSetting(line, "FovScale", &value))
            g_fovScale.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "IpdScale", &value))
            g_ipdScale.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HeadAimEnabled", &boolValue))
            g_headAimEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HeadAimSensitivity", &value))
            g_headAimSensitivity.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HeadAimInvertYaw", &boolValue))
            g_headAimInvertYaw.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HeadAimInvertPitch", &boolValue))
            g_headAimInvertPitch.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HeadAimClampPitch", &boolValue))
            g_headAimClampPitch.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HeadRollEnabled", &boolValue))
            g_headRollEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HeadRollInvert", &boolValue))
            g_headRollInvert.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "FrustumScale", &value))
            g_frustumScale.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HeadPositionEnabled", &boolValue))
            g_headPositionEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HeadPositionScale", &value))
            g_headPositionScale.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HeadPositionInvertHorizontal", &boolValue))
            g_headPositionInvertHorizontal.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "RotationSmoothingEnabled", &boolValue))
            g_rotationSmoothingEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "RotationSmoothingWindowMs", &value))
            g_rotationSmoothingWindowMs.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "BoneHideEnabled", &boolValue))
            g_boneHideEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "BoneHideRangeStart", &value))
            g_boneHideRangeStart.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "BoneHideRangeEnd", &value))
            g_boneHideRangeEnd.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "BoneHideRange2Start", &value))
            g_boneHideRange2Start.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "BoneHideRange2End", &value))
            g_boneHideRange2End.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "PlayerBoneDistanceThreshold", &value))
            g_playerBoneDistanceThreshold.store(value, std::memory_order_relaxed);
    }
    fclose(f);

    MOHW_LOG(kLogFile,
              "LoadSettings: loaded from %s -- FovScale=%.2f IpdScale=%.2f HeadAimEnabled=%d HeadAimSensitivity=%.2f "
              "HeadAimInvertYaw=%d HeadAimInvertPitch=%d HeadAimClampPitch=%d",
              path.c_str(), g_fovScale.load(std::memory_order_relaxed), g_ipdScale.load(std::memory_order_relaxed),
              g_headAimEnabled.load(std::memory_order_relaxed) ? 1 : 0,
              g_headAimSensitivity.load(std::memory_order_relaxed),
              g_headAimInvertYaw.load(std::memory_order_relaxed) ? 1 : 0,
              g_headAimInvertPitch.load(std::memory_order_relaxed) ? 1 : 0,
              g_headAimClampPitch.load(std::memory_order_relaxed) ? 1 : 0);
}

float GetFovScale()
{
    return g_fovScale.load(std::memory_order_relaxed);
}

void SetFovScale(float value)
{
    g_fovScale.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetIpdScale()
{
    return g_ipdScale.load(std::memory_order_relaxed);
}

void SetIpdScale(float value)
{
    g_ipdScale.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetHeadAimEnabled()
{
    return g_headAimEnabled.load(std::memory_order_relaxed);
}

void SetHeadAimEnabled(bool value)
{
    g_headAimEnabled.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetHeadAimSensitivity()
{
    return g_headAimSensitivity.load(std::memory_order_relaxed);
}

void SetHeadAimSensitivity(float value)
{
    g_headAimSensitivity.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetHeadAimInvertYaw()
{
    return g_headAimInvertYaw.load(std::memory_order_relaxed);
}

void SetHeadAimInvertYaw(bool value)
{
    g_headAimInvertYaw.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetHeadAimInvertPitch()
{
    return g_headAimInvertPitch.load(std::memory_order_relaxed);
}

void SetHeadAimInvertPitch(bool value)
{
    g_headAimInvertPitch.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetHeadAimClampPitch()
{
    return g_headAimClampPitch.load(std::memory_order_relaxed);
}

void SetHeadAimClampPitch(bool value)
{
    g_headAimClampPitch.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetHeadRollEnabled()
{
    return g_headRollEnabled.load(std::memory_order_relaxed);
}

void SetHeadRollEnabled(bool value)
{
    g_headRollEnabled.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetHeadRollInvert()
{
    return g_headRollInvert.load(std::memory_order_relaxed);
}

void SetHeadRollInvert(bool value)
{
    g_headRollInvert.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetFrustumScale()
{
    return g_frustumScale.load(std::memory_order_relaxed);
}

void SetFrustumScale(float value)
{
    g_frustumScale.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetHeadPositionEnabled()
{
    return g_headPositionEnabled.load(std::memory_order_relaxed);
}

void SetHeadPositionEnabled(bool value)
{
    g_headPositionEnabled.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetHeadPositionScale()
{
    return g_headPositionScale.load(std::memory_order_relaxed);
}

void SetHeadPositionScale(float value)
{
    g_headPositionScale.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetHeadPositionInvertHorizontal()
{
    return g_headPositionInvertHorizontal.load(std::memory_order_relaxed);
}

void SetHeadPositionInvertHorizontal(bool value)
{
    g_headPositionInvertHorizontal.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetRotationSmoothingEnabled()
{
    return g_rotationSmoothingEnabled.load(std::memory_order_relaxed);
}

void SetRotationSmoothingEnabled(bool value)
{
    g_rotationSmoothingEnabled.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetRotationSmoothingWindowMs()
{
    return g_rotationSmoothingWindowMs.load(std::memory_order_relaxed);
}

void SetRotationSmoothingWindowMs(float value)
{
    g_rotationSmoothingWindowMs.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetBoneHideEnabled()
{
    return g_boneHideEnabled.load(std::memory_order_relaxed);
}

void SetBoneHideEnabled(bool value)
{
    g_boneHideEnabled.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetBoneHideRangeStart()
{
    return g_boneHideRangeStart.load(std::memory_order_relaxed);
}

void SetBoneHideRangeStart(float value)
{
    g_boneHideRangeStart.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetBoneHideRangeEnd()
{
    return g_boneHideRangeEnd.load(std::memory_order_relaxed);
}

void SetBoneHideRangeEnd(float value)
{
    g_boneHideRangeEnd.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetBoneHideRange2Start()
{
    return g_boneHideRange2Start.load(std::memory_order_relaxed);
}

void SetBoneHideRange2Start(float value)
{
    g_boneHideRange2Start.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetBoneHideRange2End()
{
    return g_boneHideRange2End.load(std::memory_order_relaxed);
}

void SetBoneHideRange2End(float value)
{
    g_boneHideRange2End.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

float GetPlayerBoneDistanceThreshold()
{
    return g_playerBoneDistanceThreshold.load(std::memory_order_relaxed);
}

void SetPlayerBoneDistanceThreshold(float value)
{
    g_playerBoneDistanceThreshold.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

} // namespace mohw
