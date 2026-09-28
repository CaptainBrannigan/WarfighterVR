#include "settings.h"

#include "logging.h" // reuses LogFilePath()'s "next to the DLL" resolution

#include <windows.h>
#include <atomic>
#include <cmath>
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
constexpr float kDefaultVrTurnSpeed = 1.0f; // hooks/xinput_hook.cpp
constexpr float kDefaultVrStickFullDeflection = 0.9f; // hooks/xinput_hook.cpp outer deadzone
// Two-handed aim (openvr_direct/vr_input.cpp): how close to the rifle's line the left hand must be, and how far along
// it from the right hand, when the off-hand grip is pressed. Meters.
constexpr float kDefaultTwoHandGrabRadius = 0.15f;
constexpr float kDefaultTwoHandReach = 0.8f;
// HUD placement (hooks/draw_trace_diag.cpp): the game's full-screen HUD squeezed into HudScale of each eye's real view,
// separated by HudIpdScale times the rendering IPD (1 = the world's own IPD, at a 2 m reference; 0 = infinity).
constexpr bool kDefaultHudPlacementEnabled = true;
constexpr bool kDefaultHideReticle = true; // hooks/draw_trace_diag.cpp: the controller aims, the screen reticle doesn't
constexpr float kDefaultHudScale = 0.75f;
constexpr float kDefaultMenuScreenScale = 0.75f; // whole-2D frames: movies, menus, loading screens
constexpr float kDefaultHudOffsetXDeg = 0.0f; // + = right
constexpr float kDefaultHudOffsetYDeg = 0.0f; // + = up
constexpr float kDefaultHudIpdScale = 1.0f; // x the rendering IPD; 0 = HUD at infinity
constexpr float kDefaultHudDepth = 0.1f;    // meters
// Shot spread removal (sdk/motion_controller_aim.h's ShotSpreadRemover).
constexpr bool kDefaultRemoveShotSpread = true;
// Fire / ADS as mouse clicks (hooks/controller_trigger_hook.cpp) instead of the virtual pad's RT / LT.
constexpr bool kDefaultFireViaMouse = false;
// Hands and holsters (openvr_direct/vr_hands.cpp) and the sight dot (openvr_direct/vr_overlay.cpp).
constexpr bool kDefaultLeftHanded = false;
constexpr bool kDefaultWeaponDriveEnabled = true; // hooks/camera_matrix_test_hook.cpp, Numpad .
constexpr bool kDefaultHolsterHoldToKeep = false; // false = sticky: a grabbed weapon stays in that hand
constexpr float kDefaultHolsterRadius = 0.15f;    // meters
constexpr float kDefaultHolsterDrop = 0.0f;       // meters, + moves every zone down
constexpr bool kDefaultSightDotEnabled = true;
constexpr float kDefaultSightZeroDistance = 25.0f; // meters
constexpr float kDefaultSightDotSizeDeg = 0.35f;
// Optic dot: at the weapon hand, raised by OpticHeight (the zero) and moved by OpticWindageOffset / OpticDistance, meters
// in the hand's own frame. Dot colours: red, or green when the ...Green toggle is on.
constexpr bool kDefaultOpticDotEnabled = true;
constexpr float kDefaultOpticHeight = 0.05f;  // + = dot up
constexpr float kDefaultOpticWindage = 0.0f;  // + = dot right
constexpr float kDefaultOpticDistance = 0.0f; // + = dot forward of the hand
constexpr bool kDefaultOpticDotGreen = false;
constexpr bool kDefaultRayDotGreen = true;
// Aim ray dot angular correction, degrees from the controller's forward: the shot-spread remover isn't always exact.
constexpr float kDefaultRayDotElevationDeg = 0.0f; // + = up
constexpr float kDefaultRayDotWindageDeg = 0.0f;   // + = right
// Holster zone -> game action (a GstKeyBinding.infantry concept name from the game's profile; empty = unassigned).
const char* const kHolsterZoneKeys[kHolsterZoneCount] = {"HolsterStomach", "HolsterHip", "HolsterChest",
                                                         "HolsterLeftShoulder", "HolsterRightShoulder"};
// Holster zone centres, offsets from the head in the body frame, meters: {right, up, forward}. Stomach and hip are
// mirrored pairs (X = distance to each side).
const char* const kHolsterPosKeys[kHolsterZoneCount] = {"HolsterStomachPos", "HolsterHipPos", "HolsterChestPos",
                                                        "HolsterLeftShoulderPos", "HolsterRightShoulderPos"};
constexpr float kDefaultHolsterPos[kHolsterZoneCount][3] = {
    {0.10f, -0.55f, 0.12f},  // stomach (each side)
    {0.22f, -0.78f, 0.00f},  // hip (each side)
    {0.00f, -0.33f, 0.14f},  // chest
    {-0.20f, -0.10f, -0.10f}, // left shoulder
    {0.20f, -0.10f, -0.10f},  // right shoulder
};
const char* const kDefaultHolsterActions[kHolsterZoneCount] = {"ConceptSelectInventoryItem1",
                                                               "ConceptSelectInventoryItem2", "ConceptThrowGrenade",
                                                               "ConceptMeleeAttack", "ConceptMeleeAttack"};
// Weapon drive grip point (hooks/camera_matrix_test_hook.cpp): where the controller sits on the gun, in the gun's
// rest view (right, up, back; meters). Default = the average of three live "hold it where the gun sits" captures.
constexpr float kDefaultWeaponGripRight = 0.13f;
constexpr float kDefaultWeaponGripUp = -0.20f;
constexpr float kDefaultWeaponGripBack = -0.51f;
constexpr float kDefaultWeaponGripPitchDeg = 0.0f; // gun rotation relative to the controller, degrees
constexpr float kDefaultWeaponGripYawDeg = 0.0f;
constexpr float kDefaultWeaponGripRollDeg = 0.0f;

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
std::atomic<float> g_vrTurnSpeed{kDefaultVrTurnSpeed};
std::atomic<float> g_vrStickFullDeflection{kDefaultVrStickFullDeflection};
std::atomic<float> g_twoHandGrabRadius{kDefaultTwoHandGrabRadius};
std::atomic<float> g_twoHandReach{kDefaultTwoHandReach};
std::atomic<bool> g_hudPlacementEnabled{kDefaultHudPlacementEnabled};
std::atomic<bool> g_hideReticle{kDefaultHideReticle};
std::atomic<float> g_hudScale{kDefaultHudScale};
std::atomic<float> g_menuScreenScale{kDefaultMenuScreenScale};
std::atomic<float> g_hudOffsetXDeg{kDefaultHudOffsetXDeg};
std::atomic<float> g_hudOffsetYDeg{kDefaultHudOffsetYDeg};
std::atomic<float> g_hudIpdScale{kDefaultHudIpdScale};
std::atomic<float> g_hudDepth{kDefaultHudDepth};
std::atomic<bool> g_removeShotSpread{kDefaultRemoveShotSpread};
std::atomic<bool> g_fireViaMouse{kDefaultFireViaMouse};
std::atomic<bool> g_leftHanded{kDefaultLeftHanded};
std::atomic<bool> g_weaponDriveEnabled{kDefaultWeaponDriveEnabled};
std::atomic<bool> g_holsterHoldToKeep{kDefaultHolsterHoldToKeep};
std::atomic<float> g_holsterRadius{kDefaultHolsterRadius};
std::atomic<float> g_holsterDrop{kDefaultHolsterDrop};
std::atomic<bool> g_sightDotEnabled{kDefaultSightDotEnabled};
std::atomic<float> g_sightZeroDistance{kDefaultSightZeroDistance};
std::atomic<float> g_sightDotSizeDeg{kDefaultSightDotSizeDeg};
std::atomic<bool> g_opticDotEnabled{kDefaultOpticDotEnabled};
std::atomic<float> g_opticHeight{kDefaultOpticHeight};
std::atomic<float> g_opticWindage{kDefaultOpticWindage};
std::atomic<float> g_opticDistance{kDefaultOpticDistance};
std::atomic<bool> g_opticDotGreen{kDefaultOpticDotGreen};
std::atomic<bool> g_rayDotGreen{kDefaultRayDotGreen};
std::atomic<float> g_rayDotElevationDeg{kDefaultRayDotElevationDeg};
std::atomic<float> g_rayDotWindageDeg{kDefaultRayDotWindageDeg};
std::atomic<float> g_holsterPos[kHolsterZoneCount][3] = {
    {kDefaultHolsterPos[0][0], kDefaultHolsterPos[0][1], kDefaultHolsterPos[0][2]},
    {kDefaultHolsterPos[1][0], kDefaultHolsterPos[1][1], kDefaultHolsterPos[1][2]},
    {kDefaultHolsterPos[2][0], kDefaultHolsterPos[2][1], kDefaultHolsterPos[2][2]},
    {kDefaultHolsterPos[3][0], kDefaultHolsterPos[3][1], kDefaultHolsterPos[3][2]},
    {kDefaultHolsterPos[4][0], kDefaultHolsterPos[4][1], kDefaultHolsterPos[4][2]},
};
std::mutex g_holsterMutex;
std::string g_holsterActions[kHolsterZoneCount] = {kDefaultHolsterActions[0], kDefaultHolsterActions[1],
                                                   kDefaultHolsterActions[2], kDefaultHolsterActions[3],
                                                   kDefaultHolsterActions[4]}; // guarded by g_holsterMutex
std::atomic<float> g_weaponGripRight{kDefaultWeaponGripRight};
std::atomic<float> g_weaponGripUp{kDefaultWeaponGripUp};
std::atomic<float> g_weaponGripBack{kDefaultWeaponGripBack};
std::atomic<float> g_weaponGripPitchDeg{kDefaultWeaponGripPitchDeg};
std::atomic<float> g_weaponGripYawDeg{kDefaultWeaponGripYawDeg};
std::atomic<float> g_weaponGripRollDeg{kDefaultWeaponGripRollDeg};
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
    fprintf(f, "VrTurnSpeed=%.4f\n", g_vrTurnSpeed.load(std::memory_order_relaxed));
    fprintf(f, "VrStickFullDeflection=%.4f\n", g_vrStickFullDeflection.load(std::memory_order_relaxed));
    fprintf(f, "TwoHandGrabRadius=%.4f\n", g_twoHandGrabRadius.load(std::memory_order_relaxed));
    fprintf(f, "TwoHandReach=%.4f\n", g_twoHandReach.load(std::memory_order_relaxed));
    fprintf(f, "HudPlacementEnabled=%d\n", g_hudPlacementEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HideReticle=%d\n", g_hideReticle.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HudScale=%.4f\n", g_hudScale.load(std::memory_order_relaxed));
    fprintf(f, "MenuScreenScale=%.4f\n", g_menuScreenScale.load(std::memory_order_relaxed));
    fprintf(f, "HudIpdScale=%.4f\n", g_hudIpdScale.load(std::memory_order_relaxed));
    fprintf(f, "HudDepth=%.4f\n", g_hudDepth.load(std::memory_order_relaxed));
    fprintf(f, "HudOffsetX=%.4f\n", g_hudOffsetXDeg.load(std::memory_order_relaxed));
    fprintf(f, "HudOffsetY=%.4f\n", g_hudOffsetYDeg.load(std::memory_order_relaxed));
    fprintf(f, "RemoveShotSpread=%d\n", g_removeShotSpread.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "FireViaMouse=%d\n", g_fireViaMouse.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "LeftHanded=%d\n", g_leftHanded.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "WeaponDriveEnabled=%d\n", g_weaponDriveEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HolsterHoldToKeep=%d\n", g_holsterHoldToKeep.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HolsterRadius=%.4f\n", g_holsterRadius.load(std::memory_order_relaxed));
    fprintf(f, "HolsterDrop=%.4f\n", g_holsterDrop.load(std::memory_order_relaxed));
    {
        // Game action names (GstKeyBinding.infantry.<name> in Documents\MOHW\settings\PROF_SAVE_profile); empty =
        // unassigned. The key is looked up from the game's own bindings at launch.
        std::lock_guard<std::mutex> holsterLock(g_holsterMutex);
        for (int i = 0; i < kHolsterZoneCount; ++i)
            fprintf(f, "%s=%s\n", kHolsterZoneKeys[i], g_holsterActions[i].c_str());
    }
    // Zone centres: right,up,forward from the head, meters (stomach/hip X = each side).
    for (int i = 0; i < kHolsterZoneCount; ++i)
        fprintf(f, "%s=%.4f,%.4f,%.4f\n", kHolsterPosKeys[i], g_holsterPos[i][0].load(std::memory_order_relaxed),
                g_holsterPos[i][1].load(std::memory_order_relaxed), g_holsterPos[i][2].load(std::memory_order_relaxed));
    fprintf(f, "SightDotEnabled=%d\n", g_sightDotEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "SightZeroDistance=%.4f\n", g_sightZeroDistance.load(std::memory_order_relaxed));
    fprintf(f, "SightDotSize=%.4f\n", g_sightDotSizeDeg.load(std::memory_order_relaxed));
    fprintf(f, "OpticDotEnabled=%d\n", g_opticDotEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "OpticHeight=%.4f\n", g_opticHeight.load(std::memory_order_relaxed));
    fprintf(f, "OpticWindageOffset=%.4f\n", g_opticWindage.load(std::memory_order_relaxed));
    fprintf(f, "OpticDistance=%.4f\n", g_opticDistance.load(std::memory_order_relaxed));
    fprintf(f, "OpticDotGreen=%d\n", g_opticDotGreen.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "RayDotGreen=%d\n", g_rayDotGreen.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "RayDotElevation=%.4f\n", g_rayDotElevationDeg.load(std::memory_order_relaxed));
    fprintf(f, "RayDotWindage=%.4f\n", g_rayDotWindageDeg.load(std::memory_order_relaxed));
    fprintf(f, "WeaponGripRight=%.4f\n", g_weaponGripRight.load(std::memory_order_relaxed));
    fprintf(f, "WeaponGripUp=%.4f\n", g_weaponGripUp.load(std::memory_order_relaxed));
    fprintf(f, "WeaponGripBack=%.4f\n", g_weaponGripBack.load(std::memory_order_relaxed));
    fprintf(f, "WeaponGripPitch=%.4f\n", g_weaponGripPitchDeg.load(std::memory_order_relaxed));
    fprintf(f, "WeaponGripYaw=%.4f\n", g_weaponGripYawDeg.load(std::memory_order_relaxed));
    fprintf(f, "WeaponGripRoll=%.4f\n", g_weaponGripRollDeg.load(std::memory_order_relaxed));
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

bool ParseStringSetting(const std::string& line, const char* key, std::string* outValue)
{
    size_t keyLen = strlen(key);
    if (line.size() < keyLen + 1 || line.compare(0, keyLen, key) != 0 || line[keyLen] != '=')
        return false;
    *outValue = line.substr(keyLen + 1);
    while (!outValue->empty() && (outValue->back() == ' ' || outValue->back() == '\t'))
        outValue->pop_back();
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
        if (ParseFloatSetting(line, "IpdScale", &value))
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
        else if (ParseBoolSetting(line, "HudPlacementEnabled", &boolValue))
            g_hudPlacementEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HideReticle", &boolValue))
            g_hideReticle.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HudScale", &value))
            g_hudScale.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "MenuScreenScale", &value))
            g_menuScreenScale.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HudOffsetX", &value))
            g_hudOffsetXDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HudOffsetY", &value))
            g_hudOffsetYDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HudIpdScale", &value))
            g_hudIpdScale.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HudDepth", &value))
            g_hudDepth.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "RemoveShotSpread", &boolValue))
            g_removeShotSpread.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "FireViaMouse", &boolValue))
            g_fireViaMouse.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "LeftHanded", &boolValue))
            g_leftHanded.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "WeaponDriveEnabled", &boolValue))
            g_weaponDriveEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HolsterHoldToKeep", &boolValue))
            g_holsterHoldToKeep.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HolsterRadius", &value))
            g_holsterRadius.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HolsterDrop", &value))
            g_holsterDrop.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "SightDotEnabled", &boolValue))
            g_sightDotEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "SightZeroDistance", &value))
            g_sightZeroDistance.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "SightDotSize", &value))
            g_sightDotSizeDeg.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "OpticDotEnabled", &boolValue))
            g_opticDotEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "OpticHeight", &value))
            g_opticHeight.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "OpticWindageOffset", &value))
            g_opticWindage.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "OpticDistance", &value))
            g_opticDistance.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "OpticDotGreen", &boolValue))
            g_opticDotGreen.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "RayDotGreen", &boolValue))
            g_rayDotGreen.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "RayDotElevation", &value))
            g_rayDotElevationDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "RayDotWindage", &value))
            g_rayDotWindageDeg.store(value, std::memory_order_relaxed);
        else if ([&] {
                     std::string text;
                     for (int i = 0; i < kHolsterZoneCount; ++i)
                     {
                         if (ParseStringSetting(line, kHolsterZoneKeys[i], &text))
                         {
                             std::lock_guard<std::mutex> holsterLock(g_holsterMutex);
                             g_holsterActions[i] = text;
                             return true;
                         }
                         float xyz[3];
                         if (ParseStringSetting(line, kHolsterPosKeys[i], &text) &&
                             sscanf_s(text.c_str(), "%f,%f,%f", &xyz[0], &xyz[1], &xyz[2]) == 3)
                         {
                             for (int k = 0; k < 3; ++k)
                                 g_holsterPos[i][k].store(xyz[k], std::memory_order_relaxed);
                             return true;
                         }
                     }
                     return false;
                 }())
        {
        }
        else if (ParseFloatSetting(line, "TwoHandGrabRadius", &value))
            g_twoHandGrabRadius.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "TwoHandReach", &value))
            g_twoHandReach.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "VrStickFullDeflection", &value))
            g_vrStickFullDeflection.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "VrTurnSpeed", &value))
            g_vrTurnSpeed.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "WeaponGripRight", &value))
            g_weaponGripRight.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "WeaponGripUp", &value))
            g_weaponGripUp.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "WeaponGripPitch", &value))
            g_weaponGripPitchDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "WeaponGripYaw", &value))
            g_weaponGripYawDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "WeaponGripRoll", &value))
            g_weaponGripRollDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "WeaponGripBack", &value))
            g_weaponGripBack.store(value, std::memory_order_relaxed);
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

    // Rewrite with every current key, so an ini from an older build picks up settings added since (at their
    // defaults) instead of only gaining them the next time a hotkey happens to save. Loaded values are kept as-is.
    WriteSettingsFileLocked();
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

float GetVrTurnSpeed()
{
    return g_vrTurnSpeed.load(std::memory_order_relaxed);
}

float GetVrStickFullDeflection()
{
    return g_vrStickFullDeflection.load(std::memory_order_relaxed);
}

float GetTwoHandGrabRadius()
{
    return g_twoHandGrabRadius.load(std::memory_order_relaxed);
}

float GetTwoHandReach()
{
    return g_twoHandReach.load(std::memory_order_relaxed);
}

bool GetHudPlacementEnabled()
{
    return g_hudPlacementEnabled.load(std::memory_order_relaxed);
}

bool GetHideReticle()
{
    return g_hideReticle.load(std::memory_order_relaxed);
}

float GetHudScale()
{
    return g_hudScale.load(std::memory_order_relaxed);
}

float GetMenuScreenScale()
{
    return g_menuScreenScale.load(std::memory_order_relaxed);
}


void GetHudOffsetDeg(float* x, float* y)
{
    *x = g_hudOffsetXDeg.load(std::memory_order_relaxed);
    *y = g_hudOffsetYDeg.load(std::memory_order_relaxed);
}

float GetHudIpdScale()
{
    return g_hudIpdScale.load(std::memory_order_relaxed);
}

float GetHudDepth()
{
    return g_hudDepth.load(std::memory_order_relaxed);
}

bool GetRemoveShotSpread()
{
    return g_removeShotSpread.load(std::memory_order_relaxed);
}

bool GetFireViaMouse()
{
    return g_fireViaMouse.load(std::memory_order_relaxed);
}

bool GetLeftHanded()
{
    return g_leftHanded.load(std::memory_order_relaxed);
}

bool GetWeaponDriveEnabled()
{
    return g_weaponDriveEnabled.load(std::memory_order_relaxed);
}

void SetWeaponDriveEnabled(bool value)
{
    g_weaponDriveEnabled.store(value, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

bool GetHolsterHoldToKeep()
{
    return g_holsterHoldToKeep.load(std::memory_order_relaxed);
}

float GetHolsterRadius()
{
    return g_holsterRadius.load(std::memory_order_relaxed);
}

float GetHolsterDrop()
{
    return g_holsterDrop.load(std::memory_order_relaxed);
}

void GetHolsterOffset(int zone, float out[3])
{
    for (int k = 0; k < 3; ++k)
        out[k] = (zone >= 0 && zone < kHolsterZoneCount) ? g_holsterPos[zone][k].load(std::memory_order_relaxed) : 0.0f;
}

std::string GetHolsterAction(int zone)
{
    if (zone < 0 || zone >= kHolsterZoneCount)
        return std::string();
    std::lock_guard<std::mutex> lock(g_holsterMutex);
    return g_holsterActions[zone];
}

bool GetSightDotEnabled()
{
    return g_sightDotEnabled.load(std::memory_order_relaxed);
}

float GetSightZeroDistance()
{
    return g_sightZeroDistance.load(std::memory_order_relaxed);
}

float GetSightDotSizeDeg()
{
    return g_sightDotSizeDeg.load(std::memory_order_relaxed);
}

bool GetOpticDotEnabled()
{
    return g_opticDotEnabled.load(std::memory_order_relaxed);
}

void GetOpticOffset(float* height, float* windage)
{
    *height = g_opticHeight.load(std::memory_order_relaxed);
    *windage = g_opticWindage.load(std::memory_order_relaxed);
}

float GetOpticDistance()
{
    return g_opticDistance.load(std::memory_order_relaxed);
}

bool GetOpticDotGreen()
{
    return g_opticDotGreen.load(std::memory_order_relaxed);
}

bool GetRayDotGreen()
{
    return g_rayDotGreen.load(std::memory_order_relaxed);
}

void GetRayDotOffsetDeg(float* elevation, float* windage)
{
    *elevation = g_rayDotElevationDeg.load(std::memory_order_relaxed);
    *windage = g_rayDotWindageDeg.load(std::memory_order_relaxed);
}

void GetWeaponGripOffset(float out[3])
{
    out[0] = g_weaponGripRight.load(std::memory_order_relaxed);
    out[1] = g_weaponGripUp.load(std::memory_order_relaxed);
    out[2] = g_weaponGripBack.load(std::memory_order_relaxed);
}

void GetWeaponGripRotationDeg(float out[3])
{
    out[0] = g_weaponGripPitchDeg.load(std::memory_order_relaxed);
    out[1] = g_weaponGripYawDeg.load(std::memory_order_relaxed);
    out[2] = g_weaponGripRollDeg.load(std::memory_order_relaxed);
}

// ---- Menu table (2026-09-26, for openvr_direct/vr_overlay.cpp's SteamVR dashboard tab) ----------------------------
namespace {

struct MenuEntry
{
    const char* group;
    const char* label;
    std::atomic<bool>* toggle; // exactly one of toggle / number / holsterZone is set
    std::atomic<float>* number;
    float minValue, maxValue, step;
    int decimals;
    const char* unit;
    int holsterZone = -1; // a choice entry: this zone's game action, cycled through g_holsterChoices
};

// Game actions a holster can be set to: every keyboard-bound action in the game's own profile, filled in by
// openvr_direct/vr_hands.cpp (SetHolsterActionChoices) once it has read the profile. Guarded by g_holsterMutex.
std::vector<std::string> g_holsterChoices;

// Order = display order; entries of a group must be contiguous.
const MenuEntry kMenu[] = {
    {"Head", "Head aim", &g_headAimEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Head", "Head aim sensitivity", nullptr, &g_headAimSensitivity, 0.1f, 3.0f, 0.05f, 2, ""},
    {"Head", "Invert head yaw", &g_headAimInvertYaw, nullptr, 0, 0, 0, 0, ""},
    {"Head", "Invert head pitch", &g_headAimInvertPitch, nullptr, 0, 0, 0, 0, ""},
    {"Head", "Clamp pitch to game limits", &g_headAimClampPitch, nullptr, 0, 0, 0, 0, ""},
    {"Head", "Head roll", &g_headRollEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Head", "Invert head roll", &g_headRollInvert, nullptr, 0, 0, 0, 0, ""},
    {"Head", "Head position", &g_headPositionEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Head", "Head position scale", nullptr, &g_headPositionScale, 0.1f, 3.0f, 0.05f, 2, "x"},
    {"Head", "Invert head position (horizontal)", &g_headPositionInvertHorizontal, nullptr, 0, 0, 0, 0, ""},
    {"View", "IPD scale", nullptr, &g_ipdScale, 0.5f, 2.0f, 0.02f, 2, "x"},
    {"View", "World scale (frustum)", nullptr, &g_frustumScale, 0.5f, 2.0f, 0.02f, 2, "x"},
    {"View", "Rotation smoothing (keep on)", &g_rotationSmoothingEnabled, nullptr, 0, 0, 0, 0, ""},
    {"View", "Smoothing window", nullptr, &g_rotationSmoothingWindowMs, 10.0f, 200.0f, 1.0f, 1, "ms"},
    {"Controls", "Stick turn speed", nullptr, &g_vrTurnSpeed, 0.2f, 3.0f, 0.05f, 2, "x"},
    {"Controls", "Stick full deflection", nullptr, &g_vrStickFullDeflection, 0.5f, 1.0f, 0.01f, 2, ""},
    {"Controls", "Two-hand grab radius", nullptr, &g_twoHandGrabRadius, 0.05f, 0.5f, 0.01f, 2, "m"},
    {"Controls", "Two-hand reach", nullptr, &g_twoHandReach, 0.2f, 1.5f, 0.05f, 2, "m"},
    {"Controls", "Remove shot spread", &g_removeShotSpread, nullptr, 0, 0, 0, 0, ""},
    {"Controls", "Fire / ADS as mouse (off = RT / LT)", &g_fireViaMouse, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Left-handed", &g_leftHanded, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Hold grip to keep weapon", &g_holsterHoldToKeep, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Holster size", nullptr, &g_holsterRadius, 0.05f, 0.4f, 0.01f, 2, "m"},
    {"Hands", "Holster height offset (+ lower)", nullptr, &g_holsterDrop, -0.4f, 0.4f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Stomach X (each side)", nullptr, &g_holsterPos[0][0], 0.0f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Stomach Y", nullptr, &g_holsterPos[0][1], -1.2f, 0.3f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Stomach Z", nullptr, &g_holsterPos[0][2], -0.5f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Hip X (each side)", nullptr, &g_holsterPos[1][0], 0.0f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Hip Y", nullptr, &g_holsterPos[1][1], -1.2f, 0.3f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Hip Z", nullptr, &g_holsterPos[1][2], -0.5f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Chest X", nullptr, &g_holsterPos[2][0], -0.6f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Chest Y", nullptr, &g_holsterPos[2][1], -1.2f, 0.3f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Chest Z", nullptr, &g_holsterPos[2][2], -0.5f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Left shoulder X", nullptr, &g_holsterPos[3][0], -0.6f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Left shoulder Y", nullptr, &g_holsterPos[3][1], -1.2f, 0.3f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Left shoulder Z", nullptr, &g_holsterPos[3][2], -0.5f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Right shoulder X", nullptr, &g_holsterPos[4][0], -0.6f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Right shoulder Y", nullptr, &g_holsterPos[4][1], -1.2f, 0.3f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Right shoulder Z", nullptr, &g_holsterPos[4][2], -0.5f, 0.6f, 0.01f, 2, "m"},
    {"Holster actions", "Stomach", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterStomach},
    {"Holster actions", "Hips", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterHip},
    {"Holster actions", "Chest", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterChest},
    {"Holster actions", "Left shoulder", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterLeftShoulder},
    {"Holster actions", "Right shoulder", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterRightShoulder},
    {"Hands", "Optic dot", &g_opticDotEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Optic dot green (off = red)", &g_opticDotGreen, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Optic zero / height (+ up)", nullptr, &g_opticHeight, -0.3f, 0.3f, 0.001f, 3, "m"},
    {"Hands", "Optic windage (+ right)", nullptr, &g_opticWindage, -0.3f, 0.3f, 0.001f, 3, "m"},
    {"Hands", "Optic forward (from the hand)", nullptr, &g_opticDistance, 0.0f, 300.0f, 0.01f, 2, "m"},
    {"Hands", "Aim ray dot", &g_sightDotEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Aim ray dot green (off = red)", &g_rayDotGreen, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Aim ray up (spread fix isn't exact)", nullptr, &g_rayDotElevationDeg, -5.0f, 5.0f, 0.05f, 2, "deg"},
    {"Hands", "Aim ray right (spread fix isn't exact)", nullptr, &g_rayDotWindageDeg, -5.0f, 5.0f, 0.05f, 2, "deg"},
    {"Hands", "Aim ray dot distance", nullptr, &g_sightZeroDistance, 1.0f, 300.0f, 1.0f, 0, "m"},
    {"Hands", "Dot size", nullptr, &g_sightDotSizeDeg, 0.05f, 2.0f, 0.05f, 2, "deg"},
    {"Weapon", "Gun follows controller (Numpad .)", &g_weaponDriveEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Weapon", "Grip offset right", nullptr, &g_weaponGripRight, -0.5f, 0.5f, 0.005f, 3, "m"},
    {"Weapon", "Grip offset up", nullptr, &g_weaponGripUp, -0.5f, 0.5f, 0.005f, 3, "m"},
    {"Weapon", "Grip offset back", nullptr, &g_weaponGripBack, -1.0f, 0.5f, 0.005f, 3, "m"},
    {"Weapon", "Grip pitch", nullptr, &g_weaponGripPitchDeg, -180.0f, 180.0f, 1.0f, 0, "deg"},
    {"Weapon", "Grip yaw", nullptr, &g_weaponGripYawDeg, -180.0f, 180.0f, 1.0f, 0, "deg"},
    {"Weapon", "Grip roll", nullptr, &g_weaponGripRollDeg, -180.0f, 180.0f, 1.0f, 0, "deg"},
    {"HUD", "HUD placement", &g_hudPlacementEnabled, nullptr, 0, 0, 0, 0, ""},
    {"HUD", "Menu / movie screen size", nullptr, &g_menuScreenScale, 0.3f, 1.2f, 0.01f, 2, "x"},
    {"HUD", "Hide reticle (may clip pause-menu items)", &g_hideReticle, nullptr, 0, 0, 0, 0, ""},
    {"HUD", "HUD scale", nullptr, &g_hudScale, 0.3f, 1.2f, 0.01f, 2, "x"},
    {"HUD", "HUD depth", nullptr, &g_hudDepth, 0.1f, 10.0f, 0.1f, 1, "m"},
    {"HUD", "HUD IPD (x render IPD)", nullptr, &g_hudIpdScale, 0.0f, 4.0f, 0.02f, 2, "x"},
    {"HUD", "HUD offset right", nullptr, &g_hudOffsetXDeg, -30.0f, 30.0f, 0.5f, 1, "deg"},
    {"HUD", "HUD offset up", nullptr, &g_hudOffsetYDeg, -30.0f, 30.0f, 0.5f, 1, "deg"},
};
constexpr int kMenuCount = static_cast<int>(sizeof(kMenu) / sizeof(kMenu[0]));

} // namespace

int GetMenuSettingCount()
{
    return kMenuCount;
}

bool GetMenuSettingInfo(int index, MenuSettingInfo* out)
{
    if (index < 0 || index >= kMenuCount || !out)
        return false;
    const MenuEntry& e = kMenu[index];
    out->group = e.group;
    out->label = e.label;
    out->isToggle = e.toggle != nullptr;
    out->isChoice = e.holsterZone >= 0;
    out->value = e.toggle ? (e.toggle->load(std::memory_order_relaxed) ? 1.0f : 0.0f)
                          : (e.number ? e.number->load(std::memory_order_relaxed) : 0.0f);
    out->step = e.step;
    out->decimals = e.decimals;
    out->unit = e.unit;
    out->text[0] = '\0';
    if (out->isChoice)
    {
        std::string action = GetHolsterAction(e.holsterZone);
        const char* shown = action.empty() ? "(none)" : action.c_str();
        if (strncmp(shown, "Concept", 7) == 0) // the game's own prefix, just noise in a menu
            shown += 7;
        snprintf(out->text, sizeof(out->text), "%s", shown);
    }
    return true;
}

void SetHolsterActionChoices(const std::vector<std::string>& actions)
{
    std::lock_guard<std::mutex> lock(g_holsterMutex);
    g_holsterChoices = actions;
}

void AdjustMenuSetting(int index, int steps)
{
    if (index < 0 || index >= kMenuCount || steps == 0)
        return;
    const MenuEntry& e = kMenu[index];
    if (e.holsterZone >= 0)
    {
        // Cycle through "(none)" + the game's keyboard-bound actions.
        std::lock_guard<std::mutex> holsterLock(g_holsterMutex);
        std::vector<std::string> options;
        options.push_back(std::string());
        options.insert(options.end(), g_holsterChoices.begin(), g_holsterChoices.end());
        std::string& current = g_holsterActions[e.holsterZone];
        int at = 0;
        for (int i = 0; i < static_cast<int>(options.size()); ++i)
            if (options[i] == current)
                at = i;
        int n = static_cast<int>(options.size());
        current = options[((at + (steps > 0 ? 1 : -1)) % n + n) % n];
    }
    else if (e.toggle)
    {
        e.toggle->store(!e.toggle->load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    else
    {
        float v = e.number->load(std::memory_order_relaxed) + e.step * static_cast<float>(steps);
        // Snap to the step grid so repeated clicks don't accumulate float noise.
        v = roundf(v / e.step) * e.step;
        if (v < e.minValue)
            v = e.minValue;
        if (v > e.maxValue)
            v = e.maxValue;
        e.number->store(v, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteSettingsFileLocked();
}

} // namespace mohw
