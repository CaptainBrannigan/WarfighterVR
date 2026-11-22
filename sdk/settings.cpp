#include "settings.h"
#include "default_settings.h"
#include "mohw_offsets.h" // g_drivingExe: the driving missions keep exclusive fullscreen (GetKeepGameWindowed)

#include "logging.h" // reuses LogFilePath()'s "next to the DLL" resolution

#include <windows.h>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <mutex>
#include <vector>

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
// Where that screen fuses, meters (2026-11-08): the HUD's depth and IPD scale had it at 0.5 m, so the loading movies
// sat cross-eyed and too far inward in both eyes.
constexpr float kDefaultMenuScreenDepth = 2.5f;
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
constexpr bool kDefaultDotsOnlyWithGrip = false; // true = both dots only while the weapon grip is on
constexpr float kDefaultSightZeroDistance = 600.0f; // meters: the laser's max range
constexpr float kDefaultSightDotSizeDeg = 0.35f;
// Optic dot: at the weapon hand, raised by OpticHeight (the zero) and moved by OpticWindageOffset / OpticDistance, meters
// in the hand's own frame. Dot colours: red, or green when the ...Green toggle is on.
constexpr bool kDefaultOpticDotEnabled = true;
constexpr float kDefaultOpticHeight = 0.05f;  // + = dot up
constexpr float kDefaultOpticWindage = 0.0f;  // + = dot right
constexpr float kDefaultOpticDistance = 0.0f; // + = dot forward of the hand
constexpr bool kDefaultOpticDotGreen = false;
constexpr bool kDefaultRayDotGreen = true;
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
// The weapon drive's grip rotation (hooks/camera_matrix_test_hook.cpp GripInCamera): the gun turned about its hand
// socket, relative to the controller, degrees. (Its grip POSITION was a per-weapon setting too, until 2026-11-09: the
// gun is placed at its own hand socket now, so that's gone.)
constexpr float kDefaultWeaponGripPitchDeg = 0.0f;
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
// Global hand calibration (2026-10-16), applied to each hand's pose before anything uses it (openvr_direct/vr_input.cpp
// ApplyHandOffset): position right / up / forward (m) and rotation pitch / yaw / roll (deg) in the controller's frame,
// mirrored for the left hand.
std::atomic<float> g_handOffsetPos[3] = {{0.0f}, {0.0f}, {0.0f}};
std::atomic<float> g_handOffsetRotDeg[3] = {{0.0f}, {0.0f}, {0.0f}};
constexpr const char* kHandOffsetPosKeys[3] = {"HandOffsetRight", "HandOffsetUp", "HandOffsetForward"};
constexpr const char* kHandOffsetRotKeys[3] = {"HandOffsetPitch", "HandOffsetYaw", "HandOffsetRoll"};
// The gun held at its hand socket (2026-11-08, hooks/camera_matrix_test_hook.cpp GripInCamera): the weapon's own
// IK_Joint_RightHand put where the arm IK's weapon hand holds it (2026-11-09; at first it was the controller plus an
// offset of its own), plus this fine offset of the gun in the hand (right / up / forward, m, in the controller's frame,
// mirrored for the left hand). All guns. It was a setting (GunAtSocket) beside the profiles' grip positions until it
// proved right on every gun tried ("gun grip is fixed, no offset position necessary", 2026-11-09): now it's how the
// gun is held, and the profiles' positions are gone. The fine offset's keys were GunSocketOffset* while it was the
// socket's offset from the controller: renamed so those values, which meant something else, don't carry over.
std::atomic<float> g_gunSocketOffset[3] = {{0.0f}, {0.0f}, {0.0f}};
constexpr const char* kGunSocketOffsetKeys[3] = {"GunInHandOffsetRight", "GunInHandOffsetUp", "GunInHandOffsetForward"};
std::atomic<bool> g_armIkEnabled{true};
// The hands and the gun held in the weapon's settled pose while the player has control, no idle animations
// (2026-11-21, hooks/first_person_rig.cpp HELD POSE).
std::atomic<bool> g_holdHandPose{true};
std::atomic<float> g_leftHandIkPos[3] = {{0.0f}, {0.0f}, {0.0f}};
std::atomic<float> g_leftHandIkRotDeg[3] = {{0.0f}, {0.0f}, {0.0f}};
constexpr const char* kLeftHandIkPosKeys[3] = {"LeftHandIkRight", "LeftHandIkUp", "LeftHandIkForward"};
constexpr const char* kLeftHandIkRotKeys[3] = {"LeftHandIkPitch", "LeftHandIkYaw", "LeftHandIkRoll"};
std::atomic<bool> g_manualReloadEnabled{true};
std::atomic<float> g_magazinePouchRadius{0.12f};
// The magazine waiting in the pouch, turned {pitch, yaw, roll} degrees from the body's axes (2026-11-18, the user: "add
// a rotation for the mag in the pouch"). A magazine taken from it keeps that pose in the hand.
std::atomic<float> g_magazinePouchRot[3] = {{0.0f}, {0.0f}, {0.0f}};
// A magazine taken from the pouch (2026-11-21, settings.h GetMagazinePouchGrab): kPouchGrab*, a choice row.
std::atomic<float> g_magazinePouchGrab{static_cast<float>(kPouchGrabSnapKeepVertical)};
const char* const kPouchGrabChoices[] = {"Kept where grabbed", "Snapped, vertical from the grip", "Snapped to the hand"};
std::atomic<float> g_magazinePouch[3] = {{-0.14f}, {-0.42f}, {0.16f}};
std::atomic<bool> g_keepGameWindowed{true};
std::atomic<bool> g_nativeStereo{false}; // hooks/native_stereo.cpp, EXPERIMENTAL
std::atomic<bool> g_skyExactRays{true};  // hooks/sky_rays_fix.cpp
std::atomic<bool> g_screenEffectsBothEyes{true}; // hooks/screen_effect_diag.cpp: two-view, the left view's drawn last
std::atomic<bool> g_depthOfFieldTwoView{false};  // hooks/native_stereo.cpp: depth of field in two-view (doubles the image)
std::atomic<bool> g_renderScaleFromSteamVr{false}; // hooks/native_stereo.cpp: ResolutionScale from SteamVR's eye size
std::atomic<bool> g_eyeSizedBackbuffer{false};    // hooks/native_stereo.cpp: two-view backbuffer at SteamVR's eye size
std::atomic<bool> g_scriptedScenesFromLocks{true}; // hooks/input_restriction.cpp: the game's input locks = scripted scene
std::atomic<bool> g_showWeaponSockets{false};    // hooks/camera_matrix_test_hook.cpp: the weapon socket view
// Per weapon (kProfileFields, MagazineProfile).
std::atomic<float> g_magazineGrabOffset[3] = {{0.0f}, {-0.06f}, {0.0f}};
std::atomic<float> g_magazineGrabHalfSize[3] = {{0.08f}, {0.10f}, {0.08f}};
std::atomic<float> g_magazineAimPoint[3] = {{0.0f}, {0.20f}, {0.0f}};
std::atomic<float> g_magazineRange{0.25f};
std::atomic<float> g_magazineLockRange{0.06f};
std::atomic<float> g_magazineSeatRange{0.02f};
struct NamedFloat
{
    const char* key;
    std::atomic<float>* value;
};
const NamedFloat kMagazineProfileKeys[] = {
    {"MagazineGrabRight", &g_magazineGrabOffset[0]},    {"MagazineGrabUp", &g_magazineGrabOffset[1]},
    {"MagazineGrabForward", &g_magazineGrabOffset[2]},  {"MagazineGrabSizeRight", &g_magazineGrabHalfSize[0]},
    {"MagazineGrabSizeUp", &g_magazineGrabHalfSize[1]}, {"MagazineGrabSizeForward", &g_magazineGrabHalfSize[2]},
    {"MagazineAimRight", &g_magazineAimPoint[0]},       {"MagazineAimUp", &g_magazineAimPoint[1]},
    {"MagazineAimForward", &g_magazineAimPoint[2]},     {"MagazineRange", &g_magazineRange},
    {"MagazineLockRange", &g_magazineLockRange},        {"MagazineSeatRange", &g_magazineSeatRange},
};
// MAGAZINE PLATFORMS (2026-11-14, settings.h MagazinePlatform): every platform's values (guarded by g_fileMutex), and
// the equipped gun's platform's in the atomics the menu edits and the hooks read. The gun's choice is per weapon
// (g_weaponMagPlatform in kProfileFields, -1 = from its name).
enum PlatformField
{
    kPfHoldRight,
    kPfHoldUp,
    kPfHoldForward,
    kPfHoldPitch,
    kPfHoldYaw,
    kPfHoldRoll,
    kPfHinge,
    kPfTravel,
    kPfHingeForward,
    kPfHingeUp,
    kPfHingeDeg,
    kPfPullForce,
    kPfNeedsRelease,
    kPfCount
};
const char* const kMagazinePlatformNames[kMagazinePlatformCount] = {
    "AK", "AR-15 / 5.56", "AR-10 / 7.62", "SMG", "Bolt-action rifle", "LMG box", "Shotgun box", "Other"};
constexpr int kPlatformAk = 0, kPlatformAr15 = 1, kPlatformAr10 = 2, kPlatformSmg = 3, kPlatformBolt = 4,
              kPlatformLmg = 5, kPlatformShotgun = 6, kPlatformOther = 7;
// hold right / up / forward, pitch / yaw / roll, hinge, travel, hinge forward / up, hinge degrees, pull, release -- the
// hinge values are no longer read (2026-11-18: straight paths only), kept so the MagazinePlatform lines keep their
// layout; the hold is the pouch grab's snap again (2026-11-21).
constexpr float kPlatformDefaults[kMagazinePlatformCount][kPfCount] = {
    {0, 0, 0, 0, 0, 0, 0, 0.06f, 0.035f, 0.0f, 22.0f, 0.012f, 1}, // AK
    {0, 0, 0, 0, 0, 0, 0, 0.06f, 0.035f, 0.0f, 20.0f, 0.015f, 1}, // AR-15: straight up the magwell
    {0, 0, 0, 0, 0, 0, 0, 0.07f, 0.035f, 0.0f, 20.0f, 0.015f, 1}, // AR-10
    {0, 0, 0, 0, 0, 0, 0, 0.05f, 0.035f, 0.0f, 20.0f, 0.012f, 1}, // SMG
    {0, 0, 0, 0, 0, 0, 0, 0.04f, 0.035f, 0.0f, 20.0f, 0.012f, 1}, // bolt-action rifle
    {0, 0, 0, 0, 0, 0, 0, 0.08f, 0.035f, 0.0f, 20.0f, 0.02f, 1},  // LMG box
    {0, 0, 0, 0, 0, 0, 0, 0.06f, 0.035f, 0.0f, 20.0f, 0.015f, 1}, // shotgun box
    {0, 0, 0, 0, 0, 0, 0, 0.05f, 0.035f, 0.0f, 20.0f, 0.015f, 1}, // other
};
float g_platformValues[kMagazinePlatformCount][kPfCount] = {}; // guarded by g_fileMutex, from LoadSettings
std::atomic<float> g_magPlatform[kPfCount] = {};               // the equipped gun's platform's values
std::atomic<int> g_magPlatformIndex{kPlatformAk};              // whose values those are
bool g_magPlatformLoaded = false;                              // guarded by g_fileMutex
std::atomic<float> g_weaponMagPlatform{-1.0f};                 // per weapon: -1 = from its name, else the index
std::atomic<float> g_magazineGripBreak{0.25f};
// Scoped eye (2026-11-14, prototype, hooks/scope_view.h).
std::atomic<bool> g_scopeEyeEnabled{true};
std::atomic<float> g_scopeEyeBox{0.035f};       // m from the sight line
std::atomic<float> g_scopeEyeRelief{0.45f};     // m behind the hand along it
std::atomic<float> g_scopeApparentFov{24.0f};   // deg, the eyepiece's size in view
std::atomic<bool> g_scopeReticle{true};
std::atomic<bool> g_scopeLensPicture{true};     // the screen-space lens picture
// Per weapon (2026-11-17, the user: one scope set up per gun, "we need an offset for the scope"). The game's ADS zoom no
// longer stands in for a magnification: a gun has a scope when its profile gives one, and it works without ADS.
std::atomic<float> g_scopeMagnification{0.0f};  // 0 = no scope
std::atomic<float> g_scopeLensRight{0.0f};      // m, the lens disc's centre from the optic zero, + right
std::atomic<float> g_scopeLensUp{0.0f};         // m, + up
std::atomic<float> g_scopeLensForward{0.06f};   // m ahead of the hand
std::atomic<float> g_scopeLensRadius{0.018f};   // m
// 1 = the scope only while the game's flip magnifier is switched on (the user: "optics that have a secondary toggle, such
// as a flip to side magnifier that the scope only appears when that is toggled on in game"), 0 = always.
std::atomic<float> g_scopeNeedsMagnifier{0.0f};
const char* const kScopeShowChoices[] = {"Always", "Only with the game's magnifier on"};
const char kPlatformGroup[] = "Magazine platform";
char g_platformTitleBuffers[2][96] = {"Magazine platform: AK (every gun on it)", ""};
std::atomic<const char*> g_platformTitle{g_platformTitleBuffers[0]};
const char* const kYesNoChoices[] = {"No", "Yes"};

// A gun's platform from its blueprint name ("MOHW/Weapons/ASSAULT/AK103/spec/AK103__AKM").
int AutoMagazinePlatform(const std::string& key)
{
    std::string k = key;
    for (char& c : k)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    const auto has = [&k](const char* s) { return k.find(s) != std::string::npos; };
    if (has("AK103") || has("AKM") || has("AK47") || has("AK74") || has("SUCHKA") || has("RPK") || has("SVD") ||
        has("SAIGA"))
        return kPlatformAk;
    if (has("OBR_762") || has("SCAR_H") || has("HK417") || has("M110") || has("MK11") || has("SR25"))
        return kPlatformAr10;
    if (has("/SNIPER/"))
        return kPlatformBolt;
    if (has("/LMG/"))
        return kPlatformLmg;
    if (has("/SMG/"))
        return kPlatformSmg;
    if (has("/SHOTGUN/"))
        return kPlatformShotgun;
    if (has("/ASSAULT/") || has("M4") || has("HK416") || has("MK18") || has("OBR_556") || has("M16"))
        return kPlatformAr15;
    return kPlatformOther;
}
std::atomic<float> g_rightHandIkPos[3] = {{0.0f}, {0.0f}, {0.0f}};
std::atomic<float> g_rightHandIkRotDeg[3] = {{0.0f}, {0.0f}, {0.0f}};
std::atomic<float> g_shoulderOffset[2][3] = {{{0.0f}, {0.0f}, {-0.23f}}, {{0.0f}, {0.0f}, {0.0f}}}; // [0] left, [1] right
// Saved before the shoulders were separate (2026-11-01): read into both.
constexpr const char* kOldShoulderKeys[3] = {"ShoulderOutward", "ShoulderUp", "ShoulderForward"};
std::atomic<bool> g_naturalElbow{true};
std::atomic<float> g_elbowSwivelDeg{0.0f};
std::atomic<float> g_elbowFollow{0.75f};
std::atomic<float> g_elbowInKneeDeg{10.0f};
std::atomic<float> g_elbowInMaxDeg{25.0f};
std::atomic<float> g_elbowInNearBody{0.5f};
std::atomic<float> g_elbowOutKneeDeg{100.0f};
std::atomic<float> g_elbowOutMaxDeg{130.0f};
std::atomic<float> g_elbowSmoothingMs{50.0f};
std::atomic<float> g_forearmTwist{0.5f};
std::atomic<float> g_bodyOffset[3] = {{0.0f}, {0.0f}, {0.30f}};
std::atomic<bool> g_bodyUpright{false};
std::atomic<bool> g_stabilizeRig{true};
std::atomic<bool> g_showGrabZones{false};
const NamedFloat kArmIkKeys[] = {
    {"RightHandIkRight", &g_rightHandIkPos[0]},   {"RightHandIkUp", &g_rightHandIkPos[1]},
    {"RightHandIkForward", &g_rightHandIkPos[2]}, {"RightHandIkPitch", &g_rightHandIkRotDeg[0]},
    {"RightHandIkYaw", &g_rightHandIkRotDeg[1]},  {"RightHandIkRoll", &g_rightHandIkRotDeg[2]},
    {"LeftShoulderOutward", &g_shoulderOffset[0][0]},
    {"LeftShoulderUp", &g_shoulderOffset[0][1]},
    {"LeftShoulderForward", &g_shoulderOffset[0][2]},
    {"RightShoulderOutward", &g_shoulderOffset[1][0]},
    {"RightShoulderUp", &g_shoulderOffset[1][1]},
    {"RightShoulderForward", &g_shoulderOffset[1][2]},
    {"ElbowSwivel", &g_elbowSwivelDeg},
    {"ElbowFollow", &g_elbowFollow},              {"ElbowInKnee", &g_elbowInKneeDeg},
    {"ElbowInMax", &g_elbowInMaxDeg},             {"ElbowInNearBody", &g_elbowInNearBody},
    {"ElbowOutKnee", &g_elbowOutKneeDeg},         {"ElbowOutMax", &g_elbowOutMaxDeg},
    {"ElbowSmoothingMs", &g_elbowSmoothingMs},    {"ForearmTwist", &g_forearmTwist},
    {"MagazinePouchRadius", &g_magazinePouchRadius},
    {"MagazinePouchPitch", &g_magazinePouchRot[0]}, {"MagazinePouchYaw", &g_magazinePouchRot[1]},
    {"MagazinePouchRoll", &g_magazinePouchRot[2]},  {"MagazinePouchGrab", &g_magazinePouchGrab},
    {"BodyOffsetRight", &g_bodyOffset[0]},          {"BodyOffsetUp", &g_bodyOffset[1]},
    {"BodyOffsetForward", &g_bodyOffset[2]},
};
constexpr const char* kMagazinePouchKeys[3] = {"MagazinePouchX", "MagazinePouchY", "MagazinePouchZ"};
// Secondary grip offset (per weapon): where the off hand holds relative to the barrel line, in the gun's frame, metres
// -- the two-handed aim targets the off hand's point moved by it (openvr_direct/vr_hands.cpp AimAtOffHand).
std::atomic<float> g_secondaryGripRight{0.0f};
std::atomic<float> g_secondaryGripUp{0.0f};
std::atomic<float> g_secondaryGripForward{0.0f}; // 2026-11-21, the user: "create a secondary grip field offset forward"
std::atomic<bool> g_hudPlacementEnabled{kDefaultHudPlacementEnabled};
std::atomic<bool> g_hideReticle{kDefaultHideReticle};
// Body batch parts hidden while the gun follows the controller (hooks/camera_matrix_test_hook.cpp). Default = parts 2
// and 3, the arms and hands on the first rifle and the pistol; the order isn't the same in every level, nor the count.
std::atomic<bool> g_hideBodyPart[kMaxBodyParts] = {{false}, {true}, {true}};
constexpr int kDefaultHiddenBodyPartRows = 3; // rows 1..3 always shown and saved, so the default-on 2 and 3 can be turned off
std::atomic<int> g_bodyPartCountSeen{0};       // parts in the body batch now drawn, from SetBodyPartCountSeen
int BodyPartRowCount();
std::atomic<float> g_hudScale{kDefaultHudScale};
std::atomic<float> g_menuScreenScale{kDefaultMenuScreenScale};
std::atomic<float> g_menuScreenDepth{kDefaultMenuScreenDepth};
std::atomic<float> g_uiLayoutSize{1.0f}; // hooks/ui_pass.cpp: times the scale worked out from the resolution (1 = that)
// Marker test (2026-09-29): from this draw of the final 2D pass on, the full frame instead of the HUD rectangle; 0 = off.
std::atomic<float> g_drivingCameraIndex{0.0f};     // hooks/driving_camera_hook.cpp: 0 = auto, 1.. = ranked camera. Session-only
                                                   // (not saved): a leftover manual pick silently overrode auto (2026-10-14)
std::atomic<float> g_drivingCameraIndexTo{0.0f};   // end of the manual range (0 = only the "from" view); session-only
std::atomic<bool> g_drivingHeadLookInvert{false};
std::atomic<bool> g_drivingHeadLookAuto{true};
std::atomic<float> g_drivingStickLookRangeDeg{90.0f}; // head yaw at full stick; 0 = render-only yaw
std::atomic<float> g_drivingStickDeadzone{0.25f};     // the game's stick deadzone, skipped over (head look, steering wheel)
std::atomic<bool> g_drivingStereo{true};              // per-eye shift + true frustum, full view (not the floating screen)
std::atomic<bool> g_drivingLateNextEye{false};        // views rebuilt off the render thread get the next frame's eye
// Steering wheel (openvr_direct/vr_steering.cpp): grips hold a virtual wheel, its turn goes out as left-stick X.
std::atomic<bool> g_drivingSteeringWheel{true};
std::atomic<float> g_drivingWheelLockDeg{90.0f};  // wheel turn for full stick deflection
std::atomic<float> g_drivingWheelForward{0.45f};  // wheel centre from the head at recenter, metres
std::atomic<float> g_drivingWheelDown{0.35f};
std::atomic<float> g_drivingWheelRight{0.0f};
std::atomic<float> g_drivingWheelTiltDeg{25.0f};  // top of the wheel leaning away from the driver
std::atomic<float> g_drivingWheelRadius{0.18f};   // drawn wheel (openvr_direct/vr_overlay.cpp): rim radius, metres
std::atomic<float> g_drivingWheelThickness{0.03f}; // rim thickness, metres
std::atomic<float> g_drivingWheelOpacity{0.6f};   // 0 = not drawn
std::atomic<bool> g_specializedSequenceMotion{true}; // the master over every sequence's profile (kSequenceFields)
// The Zodiac Sequence's profile (hooks/boat_seat.h: the Basilan RHIB).
std::atomic<bool> g_boatSixDof{true};
std::atomic<bool> g_boatGunAim{true};
std::atomic<bool> g_boatWheel{true};
std::atomic<float> g_boatGunAimStrength{1.0f};
std::atomic<float> g_boatStickDeadzone{0.2f};
// Its own wheel, starting as the driving missions' (kSequenceFields).
std::atomic<float> g_boatWheelLockDeg{90.0f};
std::atomic<float> g_boatWheelForward{0.45f};
std::atomic<float> g_boatWheelDown{0.35f};
std::atomic<float> g_boatWheelRight{0.0f};
std::atomic<float> g_boatWheelTiltDeg{25.0f};
std::atomic<float> g_boatWheelRadius{0.18f};
std::atomic<float> g_boatWheelThickness{0.03f};
std::atomic<float> g_boatWheelOpacity{0.6f};
std::atomic<float> g_debugHideDraw{0.0f};   // final-pass draw number not drawn, to see what it is; 0 = off
std::atomic<float> g_hudOffsetXDeg{kDefaultHudOffsetXDeg};
std::atomic<float> g_hudOffsetYDeg{kDefaultHudOffsetYDeg};
std::atomic<float> g_hudIpdScale{kDefaultHudIpdScale};
std::atomic<float> g_hudDepth{kDefaultHudDepth};
// PER RENDER MODE (2026-11-12, settings.h RENDER MODE): the engine two-view mode's own values of the HUD's numbers (the
// plain ones above are the alternating-eye mode's). An ini without them starts both modes from the same values.
std::atomic<float> g_hudScaleTv{kDefaultHudScale};
std::atomic<float> g_menuScreenScaleTv{kDefaultMenuScreenScale};
std::atomic<float> g_menuScreenDepthTv{kDefaultMenuScreenDepth};
std::atomic<float> g_uiLayoutSizeTv{1.0f};
std::atomic<float> g_hudOffsetXDegTv{kDefaultHudOffsetXDeg};
std::atomic<float> g_hudOffsetYDegTv{kDefaultHudOffsetYDeg};
std::atomic<float> g_hudIpdScaleTv{kDefaultHudIpdScale};
std::atomic<float> g_hudDepthTv{kDefaultHudDepth};
struct PerModeSetting
{
    const char* key; // the alternating-eye key; the engine mode's is key + "TwoView"
    std::atomic<float>* alternating;
    std::atomic<float>* twoView;
};
const PerModeSetting kPerMode[] = {
    {"HudScale", &g_hudScale, &g_hudScaleTv},
    {"MenuScreenScale", &g_menuScreenScale, &g_menuScreenScaleTv},
    {"MenuScreenDepth", &g_menuScreenDepth, &g_menuScreenDepthTv},
    {"UiLayoutSize", &g_uiLayoutSize, &g_uiLayoutSizeTv},
    {"HudOffsetX", &g_hudOffsetXDeg, &g_hudOffsetXDegTv},
    {"HudOffsetY", &g_hudOffsetYDeg, &g_hudOffsetYDegTv},
    {"HudIpdScale", &g_hudIpdScale, &g_hudIpdScaleTv},
    {"HudDepth", &g_hudDepth, &g_hudDepthTv},
};
constexpr int kPerModeCount = static_cast<int>(sizeof(kPerMode) / sizeof(kPerMode[0]));
// The render mode each level was built with (NoteLevelRenderMode), for the dashboard's reload note.
std::atomic<bool> g_levelModeKnown{false};
std::atomic<bool> g_levelModeTwoView{false};
std::atomic<bool> g_removeShotSpread{kDefaultRemoveShotSpread};
std::atomic<float> g_controllerAimUpDeg{0.0f};    // sdk/motion_controller_aim.h ApplyControllerAimOffset
std::atomic<float> g_controllerAimRightDeg{0.0f};
std::atomic<float> g_adsSwayScale{0.0f};          // hooks/weapon_sway_patch.cpp: 0 = no ADS sway, 1 = the game's own
std::atomic<bool> g_fireViaMouse{kDefaultFireViaMouse};
std::atomic<bool> g_leftHanded{kDefaultLeftHanded};
std::atomic<bool> g_weaponDriveEnabled{kDefaultWeaponDriveEnabled};
std::atomic<bool> g_holsterHoldToKeep{kDefaultHolsterHoldToKeep};
std::atomic<float> g_holsterRadius{kDefaultHolsterRadius};
std::atomic<float> g_holsterDrop{kDefaultHolsterDrop};
std::atomic<bool> g_sightDotEnabled{kDefaultSightDotEnabled};
std::atomic<bool> g_dotsOnlyWithGrip{kDefaultDotsOnlyWithGrip};
std::atomic<bool> g_gripDoubleTapBumpers{true}; // openvr_direct/vr_input.cpp UpdateGripGesture
std::atomic<bool> g_thumbTapBumpers{true};      // openvr_direct/vr_input.cpp UpdateThumbTap
std::atomic<bool> g_gamepadWithoutFocus{false}; // hooks/xinput_hook.cpp GamepadFocusOnPresent
std::atomic<float> g_sightZeroDistance{kDefaultSightZeroDistance};
std::atomic<float> g_sightDotSizeDeg{kDefaultSightDotSizeDeg};
std::atomic<bool> g_opticDotEnabled{kDefaultOpticDotEnabled};
std::atomic<float> g_opticHeight{kDefaultOpticHeight};
std::atomic<float> g_opticWindage{kDefaultOpticWindage};
std::atomic<float> g_opticDistance{kDefaultOpticDistance};
std::atomic<bool> g_opticDotGreen{kDefaultOpticDotGreen};
std::atomic<bool> g_rayDotGreen{kDefaultRayDotGreen};
// Laser (hooks/laser_raycast.cpp, openvr_direct/vr_overlay.cpp): the aim-ray dot at the first surface the shot line hits,
// and a beam from the hand to it.
std::atomic<bool> g_laserBeamEnabled{true};
std::atomic<float> g_laserBeamOpacity{0.35f};
std::atomic<float> g_laserBeamWidthMm{4.0f};
std::atomic<float> g_laserRaycastEveryFrames{1.0f}; // the game raycast every N rendered frames
std::atomic<float> g_laserStartMeters{0.15f};       // per weapon: where the laser begins, forward of the hand (the muzzle)
std::atomic<float> g_laserBeamStartRight{0.0f};     // per weapon: the drawn beam's start, right of the shot line
std::atomic<float> g_laserBeamStartUp{0.0f};        // per weapon: the drawn beam's start, above the shot line
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

// ---- Specialized sequences (2026-11-22) ----------------------------------------------------------------------------
// The user: a toggle "Use 6DoF Motion For Specialized Sequences", and a profile per sequence, the first being the
// "Zodiac Sequence". The master, then each sequence's profile as keys named Sequence<name><setting>; a sequence's
// getters apply the master. Not switched as the weapon profiles are: a sequence has settings of its own kind, and all
// profiles can be edited from anywhere, so each has its own group in the menu.
// seed = where a setting starts in an ini without it (read after the whole ini, so the player's value of it).
struct SequenceField
{
    const char* key;
    std::atomic<bool>* toggle; // one of toggle / number
    std::atomic<float>* number;
    int decimals;
    const std::atomic<float>* seed;
};
const SequenceField kSequenceFields[] = {
    {"SpecializedSequenceMotion", &g_specializedSequenceMotion, nullptr, 0, nullptr},
    // The Zodiac Sequence.
    {"SequenceZodiacSixDof", &g_boatSixDof, nullptr, 0, nullptr},
    {"SequenceZodiacGunAim", &g_boatGunAim, nullptr, 0, nullptr},
    {"SequenceZodiacGunAimStrength", nullptr, &g_boatGunAimStrength, 2, nullptr},
    {"SequenceZodiacWheel", &g_boatWheel, nullptr, 0, nullptr},
    {"SequenceZodiacStickDeadzone", nullptr, &g_boatStickDeadzone, 2, nullptr},
    {"SequenceZodiacWheelLock", nullptr, &g_boatWheelLockDeg, 0, &g_drivingWheelLockDeg},
    {"SequenceZodiacWheelForward", nullptr, &g_boatWheelForward, 2, &g_drivingWheelForward},
    {"SequenceZodiacWheelDown", nullptr, &g_boatWheelDown, 2, &g_drivingWheelDown},
    {"SequenceZodiacWheelRight", nullptr, &g_boatWheelRight, 2, &g_drivingWheelRight},
    {"SequenceZodiacWheelTilt", nullptr, &g_boatWheelTiltDeg, 0, &g_drivingWheelTiltDeg},
    {"SequenceZodiacWheelRadius", nullptr, &g_boatWheelRadius, 2, &g_drivingWheelRadius},
    {"SequenceZodiacWheelThickness", nullptr, &g_boatWheelThickness, 3, &g_drivingWheelThickness},
    {"SequenceZodiacWheelOpacity", nullptr, &g_boatWheelOpacity, 2, &g_drivingWheelOpacity},
};
constexpr int kSequenceFieldCount = static_cast<int>(sizeof(kSequenceFields) / sizeof(kSequenceFields[0]));

// ---- Weapon profiles (2026-10-15) ----------------------------------------------------------------------------------
// The settings below are per weapon: they switch with the equipped weapon (SetWeaponProfile, from
// hooks/weapon_identity.cpp), and the menu rows edit the equipped weapon's profile. A weapon with no profile yet starts
// from the DEFAULT profile -- the ini's own keys for these settings -- which is also what applies while no weapon is
// identified (menus, the driving missions). A weapon's profile is only stored once it differs from the default. Saved
// as one "WeaponProfile2=<blueprint name>|v0|v1|..." line per weapon, values in kProfileFields order. All guarded by
// g_fileMutex.
// FORMAT 2 (2026-11-09): the grip position (right / up / back) was the first three values; the gun is held at its own
// hand socket now, so they're gone. Lines in the old format ("WeaponProfile=") are still read, their first three
// values skipped (kOldProfileGripValues), and saved again in the new one.
constexpr int kOldProfileGripValues = 3;
std::atomic<float>* const kProfileFields[] = {
    &g_weaponGripPitchDeg,  &g_weaponGripYawDeg,   &g_weaponGripRollDeg, &g_opticHeight, &g_opticWindage,
    &g_opticDistance,       &g_controllerAimUpDeg, &g_controllerAimRightDeg,
    // Appended 2026-10-15 (lines saved before take the default for these).
    &g_secondaryGripRight, &g_secondaryGripUp, &g_laserStartMeters,
    &g_laserBeamStartRight, &g_laserBeamStartUp,
    // Appended 2026-11-01.
    &g_magazineGrabOffset[0], &g_magazineGrabOffset[1], &g_magazineGrabOffset[2],
    &g_magazineGrabHalfSize[0], &g_magazineGrabHalfSize[1], &g_magazineGrabHalfSize[2],
    &g_magazineAimPoint[0], &g_magazineAimPoint[1], &g_magazineAimPoint[2],
    &g_magazineRange, &g_magazineLockRange, &g_magazineSeatRange,
    // Appended 2026-11-14: the gun's magazine platform (-1 = from its name), its scope's magnification (0 = no scope;
    // until 2026-11-17 it meant the game's ADS zoom).
    &g_weaponMagPlatform, &g_scopeMagnification,
    // Appended 2026-11-17: the scope's lens on this gun (forward and radius were global settings until then: the ini's
    // ScopeLensForward / ScopeLensRadius become the default profile's).
    &g_scopeLensRight, &g_scopeLensUp, &g_scopeLensForward, &g_scopeLensRadius,
    // Appended 2026-11-17: the scope only with the game's flip magnifier on.
    &g_scopeNeedsMagnifier,
    // Appended 2026-11-21: the secondary grip offset's forward part.
    &g_secondaryGripForward,
};
constexpr int kProfileFieldCount = static_cast<int>(sizeof(kProfileFields) / sizeof(kProfileFields[0]));
constexpr int kGripRollField = 2; // &g_weaponGripRollDeg's place in kProfileFields
struct WeaponProfile
{
    float v[kProfileFieldCount] = {};
};
std::map<std::string, WeaponProfile> g_weaponProfiles;
WeaponProfile g_defaultProfile;
std::string g_profileKey; // whose values the settings hold now; "" = the default profile

// The menu's title for the profile rows, "Weapon profile: <weapon>": two buffers, the pointer swapped once the new
// title is written (the overlay keeps the group pointer while drawing).
const char kProfileGroup[] = "Weapon profile";
char g_profileTitleBuffers[2][96] = {"Weapon profile: default (no weapon)", ""};
std::atomic<const char*> g_profileTitle{g_profileTitleBuffers[0]};

WeaponProfile ReadProfileFields()
{
    WeaponProfile p;
    for (int i = 0; i < kProfileFieldCount; ++i)
        p.v[i] = kProfileFields[i]->load(std::memory_order_relaxed);
    return p;
}

// The settings' current values back into the profile they belong to.
void CaptureProfileLocked()
{
    WeaponProfile now = ReadProfileFields();
    if (g_profileKey.empty())
    {
        g_defaultProfile = now;
        return;
    }
    auto it = g_weaponProfiles.find(g_profileKey);
    if (it != g_weaponProfiles.end())
        it->second = now;
    else if (memcmp(now.v, g_defaultProfile.v, sizeof(now.v)) != 0)
        g_weaponProfiles[g_profileKey] = now;
}

// What the ini's own key for a profile setting holds: the default profile's value.
float DefaultProfileValue(const std::atomic<float>* field)
{
    for (int i = 0; i < kProfileFieldCount; ++i)
        if (kProfileFields[i] == field)
            return g_defaultProfile.v[i];
    return field->load(std::memory_order_relaxed);
}

// "WeaponProfile2=<key>|v0|v1|..." -> g_weaponProfiles. Missing values take the default's. The old format,
// "WeaponProfile=", has the grip position first: skipped.
constexpr char kProfilePrefix[] = "WeaponProfile2=";
constexpr char kOldProfilePrefix[] = "WeaponProfile=";
bool IsWeaponProfileLine(const std::string& line)
{
    return line.compare(0, sizeof(kProfilePrefix) - 1, kProfilePrefix) == 0 ||
           line.compare(0, sizeof(kOldProfilePrefix) - 1, kOldProfilePrefix) == 0;
}

bool ParseWeaponProfileLine(const std::string& line)
{
    int skip = 0;
    std::string rest;
    if (line.compare(0, sizeof(kProfilePrefix) - 1, kProfilePrefix) == 0)
        rest = line.substr(sizeof(kProfilePrefix) - 1);
    else if (line.compare(0, sizeof(kOldProfilePrefix) - 1, kOldProfilePrefix) == 0)
    {
        rest = line.substr(sizeof(kOldProfilePrefix) - 1);
        skip = kOldProfileGripValues;
    }
    else
        return false;
    size_t bar = rest.find('|');
    std::string key = rest.substr(0, bar);
    if (key.empty())
        return true;
    for (int i = 0; i < skip && bar != std::string::npos; ++i)
        bar = rest.find('|', bar + 1);
    WeaponProfile p = g_defaultProfile;
    for (int i = 0; i < kProfileFieldCount && bar != std::string::npos; ++i)
    {
        size_t next = rest.find('|', bar + 1);
        p.v[i] = static_cast<float>(atof(rest.substr(bar + 1, next - bar - 1).c_str()));
        bar = next;
    }
    g_weaponProfiles[key] = p;
    return true;
}

// ---- Magazine platforms (2026-11-14) -----------------------------------------------------------------------------
// The atomics' current values back into their platform's row.
void CapturePlatformLocked()
{
    if (!g_magPlatformLoaded)
        return;
    const int at = g_magPlatformIndex.load(std::memory_order_relaxed);
    for (int f = 0; f < kPfCount; ++f)
        g_platformValues[at][f] = g_magPlatform[f].load(std::memory_order_relaxed);
}

// The equipped gun's platform (its profile's choice, or from its name) into the atomics, and the menu's title.
void ResolvePlatformLocked()
{
    int want = static_cast<int>(lroundf(g_weaponMagPlatform.load(std::memory_order_relaxed)));
    const bool automatic = want < 0 || want >= kMagazinePlatformCount;
    if (automatic)
        want = AutoMagazinePlatform(g_profileKey);
    if (!g_magPlatformLoaded || want != g_magPlatformIndex.load(std::memory_order_relaxed))
    {
        CapturePlatformLocked();
        for (int f = 0; f < kPfCount; ++f)
            g_magPlatform[f].store(g_platformValues[want][f], std::memory_order_relaxed);
        g_magPlatformIndex.store(want, std::memory_order_relaxed);
        g_magPlatformLoaded = true;
        MOHW_LOG(kLogFile, "magazine platform: %s (%s)", kMagazinePlatformNames[want], automatic ? "from the name" : "set");
    }
    const char* shown = g_platformTitle.load(std::memory_order_acquire);
    char* buffer = shown == g_platformTitleBuffers[0] ? g_platformTitleBuffers[1] : g_platformTitleBuffers[0];
    snprintf(buffer, sizeof(g_platformTitleBuffers[0]), "Magazine platform: %s (every gun on it)",
             kMagazinePlatformNames[want]);
    g_platformTitle.store(buffer, std::memory_order_release);
}

// "MagazinePlatform<i>=v0,v1,..." (kPfCount values; missing ones keep the default).
bool ParsePlatformLine(const std::string& line)
{
    if (line.compare(0, 16, "MagazinePlatform") != 0 || line.size() < 18 || !isdigit(static_cast<unsigned char>(line[16])) ||
        line[17] != '=')
        return false;
    const int i = line[16] - '0';
    if (i < 0 || i >= kMagazinePlatformCount)
        return true;
    const char* p = line.c_str() + 18;
    for (int f = 0; f < kPfCount && *p; ++f)
    {
        char* end = nullptr;
        const float v = strtof(p, &end);
        if (end == p)
            break;
        g_platformValues[i][f] = v;
        p = *end == ',' ? end + 1 : end;
    }
    return true;
}

void WriteSettingsFileLocked()
{
    CaptureProfileLocked(); // the default keys and the profile lines below take the latest edits
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
    for (int i = 0; i < 3; ++i)
    {
        fprintf(f, "%s=%.4f\n", kHandOffsetPosKeys[i], g_handOffsetPos[i].load(std::memory_order_relaxed));
        fprintf(f, "%s=%.2f\n", kHandOffsetRotKeys[i], g_handOffsetRotDeg[i].load(std::memory_order_relaxed));
        fprintf(f, "%s=%.4f\n", kLeftHandIkPosKeys[i], g_leftHandIkPos[i].load(std::memory_order_relaxed));
        fprintf(f, "%s=%.2f\n", kLeftHandIkRotKeys[i], g_leftHandIkRotDeg[i].load(std::memory_order_relaxed));
        fprintf(f, "%s=%.4f\n", kMagazinePouchKeys[i], g_magazinePouch[i].load(std::memory_order_relaxed));
    }
    for (int i = 0; i < 3; ++i)
        fprintf(f, "%s=%.4f\n", kGunSocketOffsetKeys[i], g_gunSocketOffset[i].load(std::memory_order_relaxed));
    fprintf(f, "SecondaryGripRight=%.4f\n", DefaultProfileValue(&g_secondaryGripRight));
    fprintf(f, "SecondaryGripUp=%.4f\n", DefaultProfileValue(&g_secondaryGripUp));
    fprintf(f, "SecondaryGripForward=%.4f\n", DefaultProfileValue(&g_secondaryGripForward));
    fprintf(f, "HudPlacementEnabled=%d\n", g_hudPlacementEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HideReticle=%d\n", g_hideReticle.load(std::memory_order_relaxed) ? 1 : 0);
    for (int i = 0, n = BodyPartRowCount(); i < n; ++i)
        fprintf(f, "HideBodyPart%d=%d\n", i + 1, g_hideBodyPart[i].load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HudScale=%.4f\n", g_hudScale.load(std::memory_order_relaxed));
    fprintf(f, "MenuScreenScale=%.4f\n", g_menuScreenScale.load(std::memory_order_relaxed));
    fprintf(f, "UiLayoutSize=%.4f\n", g_uiLayoutSize.load(std::memory_order_relaxed));
    fprintf(f, "MenuScreenDepth=%.4f\n", g_menuScreenDepth.load(std::memory_order_relaxed));
    fprintf(f, "DrivingHeadLookInvert=%d\n", g_drivingHeadLookInvert.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "DrivingHeadLookAuto=%d\n", g_drivingHeadLookAuto.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "DrivingStickLookRange=%.0f\n", g_drivingStickLookRangeDeg.load(std::memory_order_relaxed));
    fprintf(f, "DrivingStickDeadzone=%.2f\n", g_drivingStickDeadzone.load(std::memory_order_relaxed));
    fprintf(f, "DrivingStereo=%d\n", g_drivingStereo.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "DrivingGameThreadNextEye=%d\n", g_drivingLateNextEye.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "DrivingSteeringWheel=%d\n", g_drivingSteeringWheel.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "DrivingWheelLock=%.0f\n", g_drivingWheelLockDeg.load(std::memory_order_relaxed));
    fprintf(f, "DrivingWheelForward=%.2f\n", g_drivingWheelForward.load(std::memory_order_relaxed));
    fprintf(f, "DrivingWheelDown=%.2f\n", g_drivingWheelDown.load(std::memory_order_relaxed));
    fprintf(f, "DrivingWheelRight=%.2f\n", g_drivingWheelRight.load(std::memory_order_relaxed));
    fprintf(f, "DrivingWheelTilt=%.0f\n", g_drivingWheelTiltDeg.load(std::memory_order_relaxed));
    fprintf(f, "DrivingWheelRadius=%.2f\n", g_drivingWheelRadius.load(std::memory_order_relaxed));
    fprintf(f, "DrivingWheelThickness=%.3f\n", g_drivingWheelThickness.load(std::memory_order_relaxed));
    fprintf(f, "DrivingWheelOpacity=%.2f\n", g_drivingWheelOpacity.load(std::memory_order_relaxed));
    for (const SequenceField& s : kSequenceFields)
    {
        if (s.toggle)
            fprintf(f, "%s=%d\n", s.key, s.toggle->load(std::memory_order_relaxed) ? 1 : 0);
        else
            fprintf(f, "%s=%.*f\n", s.key, s.decimals, s.number->load(std::memory_order_relaxed));
    }
    fprintf(f, "DebugHideDraw=%.0f\n", g_debugHideDraw.load(std::memory_order_relaxed));
    fprintf(f, "HudIpdScale=%.4f\n", g_hudIpdScale.load(std::memory_order_relaxed));
    fprintf(f, "HudDepth=%.4f\n", g_hudDepth.load(std::memory_order_relaxed));
    fprintf(f, "HudOffsetX=%.4f\n", g_hudOffsetXDeg.load(std::memory_order_relaxed));
    fprintf(f, "HudOffsetY=%.4f\n", g_hudOffsetYDeg.load(std::memory_order_relaxed));
    for (const PerModeSetting& p : kPerMode) // the engine two-view mode's own values
        fprintf(f, "%sTwoView=%.4f\n", p.key, p.twoView->load(std::memory_order_relaxed));
    fprintf(f, "RemoveShotSpread=%d\n", g_removeShotSpread.load(std::memory_order_relaxed) ? 1 : 0);
    // Per-weapon settings (weapon profiles): these keys hold the default profile.
    fprintf(f, "ControllerAimUp=%.2f\n", DefaultProfileValue(&g_controllerAimUpDeg));
    fprintf(f, "ControllerAimRight=%.2f\n", DefaultProfileValue(&g_controllerAimRightDeg));
    fprintf(f, "AdsSwayScale=%.2f\n", g_adsSwayScale.load(std::memory_order_relaxed));
    fprintf(f, "FireViaMouse=%d\n", g_fireViaMouse.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "LeftHanded=%d\n", g_leftHanded.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "WeaponDriveEnabled=%d\n", g_weaponDriveEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "ArmIkEnabled=%d\n", g_armIkEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "HoldHandPose=%d\n", g_holdHandPose.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "ManualReloadEnabled=%d\n", g_manualReloadEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "NaturalElbow=%d\n", g_naturalElbow.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "BodyUpright=%d\n", g_bodyUpright.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "StabilizeRig=%d\n", g_stabilizeRig.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "ShowGrabZones=%d\n", g_showGrabZones.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "KeepGameWindowed=%d\n", g_keepGameWindowed.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "NativeStereo=%d\n", g_nativeStereo.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "SkyExactRays=%d\n", g_skyExactRays.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "ScreenEffectsBothEyes=%d\n", g_screenEffectsBothEyes.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "DepthOfFieldTwoView=%d\n", g_depthOfFieldTwoView.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "RenderScaleFromSteamVr=%d\n", g_renderScaleFromSteamVr.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "EyeSizedBackbuffer=%d\n", g_eyeSizedBackbuffer.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "ScriptedScenesFromLocks=%d\n", g_scriptedScenesFromLocks.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "WriteLogFiles=%d\n", g_logsEnabled.load(std::memory_order_relaxed) ? 1 : 0); // sdk/logging.h
    fprintf(f, "ShowWeaponSockets=%d\n", g_showWeaponSockets.load(std::memory_order_relaxed) ? 1 : 0);
    for (const NamedFloat& n : kArmIkKeys)
        fprintf(f, "%s=%.4f\n", n.key, n.value->load(std::memory_order_relaxed));
    for (const NamedFloat& n : kMagazineProfileKeys)
        fprintf(f, "%s=%.4f\n", n.key, DefaultProfileValue(n.value));
    fprintf(f, "MagazinePlatformChoice=%.0f\n", DefaultProfileValue(&g_weaponMagPlatform));
    fprintf(f, "MagazineGripBreak=%.4f\n", g_magazineGripBreak.load(std::memory_order_relaxed));
    fprintf(f, "ScopeEyeEnabled=%d\n", g_scopeEyeEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "ScopeEyeBox=%.4f\n", g_scopeEyeBox.load(std::memory_order_relaxed));
    fprintf(f, "ScopeEyeRelief=%.4f\n", g_scopeEyeRelief.load(std::memory_order_relaxed));
    fprintf(f, "ScopeApparentFov=%.2f\n", g_scopeApparentFov.load(std::memory_order_relaxed));
    fprintf(f, "ScopeReticle=%d\n", g_scopeReticle.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "ScopeMagnification=%.2f\n", DefaultProfileValue(&g_scopeMagnification));
    fprintf(f, "ScopeLensPicture=%d\n", g_scopeLensPicture.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "ScopeLensRight=%.4f\n", DefaultProfileValue(&g_scopeLensRight));
    fprintf(f, "ScopeLensUp=%.4f\n", DefaultProfileValue(&g_scopeLensUp));
    fprintf(f, "ScopeLensForward=%.4f\n", DefaultProfileValue(&g_scopeLensForward));
    fprintf(f, "ScopeLensRadius=%.4f\n", DefaultProfileValue(&g_scopeLensRadius));
    fprintf(f, "ScopeNeedsMagnifier=%.0f\n", DefaultProfileValue(&g_scopeNeedsMagnifier));
    CapturePlatformLocked();
    fprintf(f, "; MagazinePlatform<n>: in-hand right,up,forward (m), pitch,yaw,roll (deg), hinge (0 straight / 1 hinge),\n"
               "; travel (m), hinge forward,up (m), hinge angle (deg), pull force (m), needs release (0/1). n:");
    for (int i = 0; i < kMagazinePlatformCount; ++i)
        fprintf(f, " %d %s%s", i, kMagazinePlatformNames[i], i + 1 < kMagazinePlatformCount ? "," : "\n");
    for (int i = 0; i < kMagazinePlatformCount; ++i)
    {
        fprintf(f, "MagazinePlatform%d=", i);
        for (int k = 0; k < kPfCount; ++k)
            fprintf(f, "%.4f%s", g_platformValues[i][k], k + 1 < kPfCount ? "," : "\n");
    }
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
    fprintf(f, "DotsOnlyWithGrip=%d\n", g_dotsOnlyWithGrip.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "GripDoubleTapBumpers=%d\n", g_gripDoubleTapBumpers.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "ThumbTapBumpers=%d\n", g_thumbTapBumpers.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "GamepadWithoutFocus=%d\n", g_gamepadWithoutFocus.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "SightZeroDistance=%.4f\n", g_sightZeroDistance.load(std::memory_order_relaxed));
    fprintf(f, "SightDotSize=%.4f\n", g_sightDotSizeDeg.load(std::memory_order_relaxed));
    fprintf(f, "OpticDotEnabled=%d\n", g_opticDotEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "OpticHeight=%.4f\n", DefaultProfileValue(&g_opticHeight));
    fprintf(f, "OpticWindageOffset=%.4f\n", DefaultProfileValue(&g_opticWindage));
    fprintf(f, "OpticDistance=%.4f\n", DefaultProfileValue(&g_opticDistance));
    fprintf(f, "OpticDotGreen=%d\n", g_opticDotGreen.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "RayDotGreen=%d\n", g_rayDotGreen.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "LaserBeam=%d\n", g_laserBeamEnabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "LaserBeamOpacity=%.2f\n", g_laserBeamOpacity.load(std::memory_order_relaxed));
    fprintf(f, "LaserBeamWidth=%.1f\n", g_laserBeamWidthMm.load(std::memory_order_relaxed));
    fprintf(f, "LaserRaycastEveryFrames=%.0f\n", g_laserRaycastEveryFrames.load(std::memory_order_relaxed));
    fprintf(f, "LaserStart=%.3f\n", DefaultProfileValue(&g_laserStartMeters));
    fprintf(f, "LaserBeamStartRight=%.3f\n", DefaultProfileValue(&g_laserBeamStartRight));
    fprintf(f, "LaserBeamStartUp=%.3f\n", DefaultProfileValue(&g_laserBeamStartUp));
    fprintf(f, "WeaponGripPitch=%.4f\n", DefaultProfileValue(&g_weaponGripPitchDeg));
    fprintf(f, "WeaponGripYaw=%.4f\n", DefaultProfileValue(&g_weaponGripYawDeg));
    fprintf(f, "WeaponGripRoll=%.4f\n", DefaultProfileValue(&g_weaponGripRollDeg));
    // Weapon profiles (format 2): grip pitch/yaw/roll, optic height/windage/forward, controller aim up/right, then the
    // rest of kProfileFields.
    for (const auto& profile : g_weaponProfiles)
    {
        fprintf(f, "%s%s", kProfilePrefix, profile.first.c_str());
        for (int i = 0; i < kProfileFieldCount; ++i)
            fprintf(f, "|%.4f", profile.second.v[i]);
        fprintf(f, "\n");
    }
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

// kSequenceFields: apart from LoadSettings' else-if chain, which is near the compiler's nesting limit.
bool ParseSequenceSetting(const std::string& line, bool (&seen)[kSequenceFieldCount])
{
    for (int i = 0; i < kSequenceFieldCount; ++i)
    {
        const SequenceField& s = kSequenceFields[i];
        float value = 0.0f;
        if (!ParseFloatSetting(line, s.key, &value))
            continue;
        if (s.toggle)
            s.toggle->store(value != 0.0f, std::memory_order_relaxed);
        else
            s.number->store(value, std::memory_order_relaxed);
        seen[i] = true;
        return true;
    }
    return false;
}

// HideBodyPart1..kMaxBodyParts.
bool ParseBodyPartSetting(const std::string& line)
{
    if (line.compare(0, 12, "HideBodyPart") != 0)
        return false;
    for (int i = 0; i < kMaxBodyParts; ++i)
    {
        char key[32];
        snprintf(key, sizeof(key), "HideBodyPart%d", i + 1);
        bool value = false;
        if (ParseBoolSetting(line, key, &value))
        {
            g_hideBodyPart[i].store(value, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

// Body part rows to show and save: every part of the body batch now drawn, plus any part set to hidden beyond that
// (so a setting made in another level stays visible), and the default-hidden ones.
int BodyPartRowCount()
{
    int n = g_bodyPartCountSeen.load(std::memory_order_relaxed);
    for (int i = 0; i < kMaxBodyParts; ++i)
        if (g_hideBodyPart[i].load(std::memory_order_relaxed) && i + 1 > n)
            n = i + 1;
    return n < kDefaultHiddenBodyPartRows ? kDefaultHiddenBodyPartRows : n;
}

} // namespace

void LoadLogSwitch()
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    FILE* f = nullptr;
    if (fopen_s(&f, SettingsFilePath().c_str(), "r") != 0 || !f)
        return; // first launch: the default (off)
    char lineBuf[512];
    while (fgets(lineBuf, sizeof(lineBuf), f))
    {
        std::string line(lineBuf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        bool on = false;
        if (ParseBoolSetting(line, "WriteLogFiles", &on))
            g_logsEnabled.store(on, std::memory_order_relaxed);
    }
    fclose(f);
}

void LoadSettings()
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    std::string path = SettingsFilePath();
    memcpy(g_platformValues, kPlatformDefaults, sizeof(g_platformValues)); // the ini's MagazinePlatform lines override

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f)
    {
        // First launch: start from the built-in defaults (sdk/default_settings.h), written out and read like any ini.
        FILE* out = nullptr;
        if (fopen_s(&out, path.c_str(), "w") == 0 && out)
        {
            fputs(kBuiltInSettings, out);
            fclose(out);
        }
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
        MOHW_LOG(kLogFile, "LoadSettings: %s not found, created from the built-in defaults", path.c_str());
    }

    std::vector<std::string> profileLines;
    bool twoViewSeen[kPerModeCount] = {};
    bool sequenceSeen[kSequenceFieldCount] = {};
    bool layoutSizeSeen = false; // UiLayoutSize, new on 2026-11-22: without it the ini is from before
    char lineBuf[512];
    while (fgets(lineBuf, sizeof(lineBuf), f))
    {
        std::string line(lineBuf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        float value = 0.0f;
        bool boolValue = false;
        bool perMode = false;
        for (int i = 0; i < kPerModeCount && !perMode; ++i)
        {
            const std::string key = std::string(kPerMode[i].key) + "TwoView";
            if (ParseFloatSetting(line, key.c_str(), &value))
            {
                kPerMode[i].twoView->store(value, std::memory_order_relaxed);
                twoViewSeen[i] = true;
                perMode = true;
            }
        }
        if (perMode || ParseSequenceSetting(line, sequenceSeen))
            continue;
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
        else if (ParseBodyPartSetting(line))
        {
        }
        else if (ParseFloatSetting(line, "HudScale", &value))
            g_hudScale.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "MenuScreenScale", &value))
            g_menuScreenScale.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "UiLayoutSize", &value))
        {
            g_uiLayoutSize.store(value, std::memory_order_relaxed);
            layoutSizeSeen = true;
        }
        else if (ParseFloatSetting(line, "MenuScreenDepth", &value))
            g_menuScreenDepth.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "DrivingHeadLookInvert", &boolValue))
            g_drivingHeadLookInvert.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "DrivingHeadLookAuto", &boolValue))
            g_drivingHeadLookAuto.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingStickLookRange", &value))
            g_drivingStickLookRangeDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingStickDeadzone", &value))
            g_drivingStickDeadzone.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "DrivingStereo", &boolValue))
            g_drivingStereo.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "DrivingGameThreadNextEye", &boolValue))
            g_drivingLateNextEye.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "DrivingSteeringWheel", &boolValue))
            g_drivingSteeringWheel.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingWheelLock", &value))
            g_drivingWheelLockDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingWheelForward", &value))
            g_drivingWheelForward.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingWheelDown", &value))
            g_drivingWheelDown.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingWheelRight", &value))
            g_drivingWheelRight.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingWheelTilt", &value))
            g_drivingWheelTiltDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingWheelRadius", &value))
            g_drivingWheelRadius.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingWheelThickness", &value))
            g_drivingWheelThickness.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DrivingWheelOpacity", &value))
            g_drivingWheelOpacity.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "DebugHideDraw", &value))
            g_debugHideDraw.store(value, std::memory_order_relaxed);
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
        else if (ParseFloatSetting(line, "ControllerAimUp", &value))
            g_controllerAimUpDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ControllerAimRight", &value))
            g_controllerAimRightDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "AdsSwayScale", &value))
            g_adsSwayScale.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "FireViaMouse", &boolValue))
            g_fireViaMouse.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "LeftHanded", &boolValue))
            g_leftHanded.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "WeaponDriveEnabled", &boolValue))
            g_weaponDriveEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ArmIkEnabled", &boolValue))
            g_armIkEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "HoldHandPose", &boolValue))
            g_holdHandPose.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ManualReloadEnabled", &boolValue))
            g_manualReloadEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "NaturalElbow", &boolValue))
            g_naturalElbow.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "BodyUpright", &boolValue))
            g_bodyUpright.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "StabilizeRig", &boolValue))
            g_stabilizeRig.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ShowGrabZones", &boolValue))
            g_showGrabZones.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "KeepGameWindowed", &boolValue))
            g_keepGameWindowed.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "NativeStereo", &boolValue))
            g_nativeStereo.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "SkyExactRays", &boolValue))
            g_skyExactRays.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ScreenEffectsBothEyes", &boolValue))
            g_screenEffectsBothEyes.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "DepthOfFieldTwoView", &boolValue))
            g_depthOfFieldTwoView.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "RenderScaleFromSteamVr", &boolValue))
            g_renderScaleFromSteamVr.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "EyeSizedBackbuffer", &boolValue))
            g_eyeSizedBackbuffer.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ScriptedScenesFromLocks", &boolValue))
            g_scriptedScenesFromLocks.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "WriteLogFiles", &boolValue))
            g_logsEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ShowWeaponSockets", &boolValue))
            g_showWeaponSockets.store(boolValue, std::memory_order_relaxed);
        else if ([&] {
                     for (int k = 0; k < 3; ++k)
                         if (ParseFloatSetting(line, kOldShoulderKeys[k], &value))
                         {
                             g_shoulderOffset[0][k].store(value, std::memory_order_relaxed);
                             g_shoulderOffset[1][k].store(value, std::memory_order_relaxed);
                             return true;
                         }
                     for (const NamedFloat& n : kArmIkKeys)
                         if (ParseFloatSetting(line, n.key, &value))
                         {
                             n.value->store(value, std::memory_order_relaxed);
                             return true;
                         }
                     for (const NamedFloat& n : kMagazineProfileKeys)
                         if (ParseFloatSetting(line, n.key, &value))
                         {
                             n.value->store(value, std::memory_order_relaxed);
                             return true;
                         }
                     return false;
                 }())
        {
        }
        else if (ParseBoolSetting(line, "HolsterHoldToKeep", &boolValue))
            g_holsterHoldToKeep.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HolsterRadius", &value))
            g_holsterRadius.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "HolsterDrop", &value))
            g_holsterDrop.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "SightDotEnabled", &boolValue))
            g_sightDotEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "DotsOnlyWithGrip", &boolValue))
            g_dotsOnlyWithGrip.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "GripDoubleTapBumpers", &boolValue))
            g_gripDoubleTapBumpers.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ThumbTapBumpers", &boolValue))
            g_thumbTapBumpers.store(boolValue, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "GamepadWithoutFocus", &boolValue))
            g_gamepadWithoutFocus.store(boolValue, std::memory_order_relaxed);
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
        else if (ParseBoolSetting(line, "LaserBeam", &boolValue))
            g_laserBeamEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "LaserBeamOpacity", &value))
            g_laserBeamOpacity.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "LaserBeamWidth", &value))
            g_laserBeamWidthMm.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "LaserRaycastEveryFrames", &value))
            g_laserRaycastEveryFrames.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "LaserStart", &value))
            g_laserStartMeters.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "LaserBeamStartRight", &value))
            g_laserBeamStartRight.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "LaserBeamStartUp", &value))
            g_laserBeamStartUp.store(value, std::memory_order_relaxed);
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
        else if ([&] {
                     for (int i = 0; i < 3; ++i)
                     {
                         std::atomic<float>* fields[5] = {&g_handOffsetPos[i], &g_handOffsetRotDeg[i],
                                                          &g_leftHandIkPos[i], &g_leftHandIkRotDeg[i],
                                                          &g_magazinePouch[i]};
                         const char* keys[5] = {kHandOffsetPosKeys[i], kHandOffsetRotKeys[i], kLeftHandIkPosKeys[i],
                                                kLeftHandIkRotKeys[i], kMagazinePouchKeys[i]};
                         for (int k = 0; k < 5; ++k)
                             if (ParseFloatSetting(line, keys[k], &value))
                             {
                                 fields[k]->store(value, std::memory_order_relaxed);
                                 return true;
                             }
                     }
                     return false;
                 }())
        {
        }
        else if ([&] {
                     for (int i = 0; i < 3; ++i)
                         if (ParseFloatSetting(line, kGunSocketOffsetKeys[i], &value))
                         {
                             g_gunSocketOffset[i].store(value, std::memory_order_relaxed);
                             return true;
                         }
                     return false;
                 }())
        {
        }
        else if (ParseFloatSetting(line, "MagazinePlatformChoice", &value))
            g_weaponMagPlatform.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "MagazineGripBreak", &value))
            g_magazineGripBreak.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ScopeEyeEnabled", &boolValue))
            g_scopeEyeEnabled.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ScopeEyeBox", &value))
            g_scopeEyeBox.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ScopeEyeRelief", &value))
            g_scopeEyeRelief.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ScopeApparentFov", &value))
            g_scopeApparentFov.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ScopeReticle", &boolValue))
            g_scopeReticle.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ScopeMagnification", &value))
            g_scopeMagnification.store(value, std::memory_order_relaxed);
        else if (ParseBoolSetting(line, "ScopeLensPicture", &boolValue))
            g_scopeLensPicture.store(boolValue, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ScopeLensRight", &value))
            g_scopeLensRight.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ScopeLensUp", &value))
            g_scopeLensUp.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ScopeNeedsMagnifier", &value))
            g_scopeNeedsMagnifier.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ScopeLensForward", &value))
            g_scopeLensForward.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "ScopeLensRadius", &value))
            g_scopeLensRadius.store(value, std::memory_order_relaxed);
        else if (ParsePlatformLine(line))
        {
        }
        else if (ParseFloatSetting(line, "SecondaryGripRight", &value))
            g_secondaryGripRight.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "SecondaryGripUp", &value))
            g_secondaryGripUp.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "SecondaryGripForward", &value))
            g_secondaryGripForward.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "VrStickFullDeflection", &value))
            g_vrStickFullDeflection.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "VrTurnSpeed", &value))
            g_vrTurnSpeed.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "WeaponGripPitch", &value))
            g_weaponGripPitchDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "WeaponGripYaw", &value))
            g_weaponGripYawDeg.store(value, std::memory_order_relaxed);
        else if (ParseFloatSetting(line, "WeaponGripRoll", &value))
            g_weaponGripRollDeg.store(value, std::memory_order_relaxed);
        else if (IsWeaponProfileLine(line))
            profileLines.push_back(line); // after the loop, once the default profile is known
    }
    fclose(f);
    // An ini from before the per-mode HUD values: the engine mode starts from the same values.
    for (int i = 0; i < kPerModeCount; ++i)
        if (!twoViewSeen[i])
            kPerMode[i].twoView->store(kPerMode[i].alternating->load(std::memory_order_relaxed), std::memory_order_relaxed);
    // An ini from before a sequence's profile: that sequence's wheel starts as this ini's driving wheel.
    for (int i = 0; i < kSequenceFieldCount; ++i)
        if (!sequenceSeen[i] && kSequenceFields[i].seed)
            kSequenceFields[i].number->store(kSequenceFields[i].seed->load(std::memory_order_relaxed),
                                             std::memory_order_relaxed);
    // The ini's own values for the per-weapon settings are the default profile; then the weapons' own.
    // THE OLD -10 DEG GRIP ROLL (2026-11-22, the user: "no grip should have roll unless a user specifies"): earlier
    // builds' built-in default, so every ini they made holds it, and every profile copied from it. Taken out once, from
    // an ini of before then (no UiLayoutSize yet); any other roll, and a -10 set after, is the user's.
    const auto oldRollArtifact = [](float deg) { return fabsf(deg + 10.0f) < 0.0005f; };
    if (!layoutSizeSeen && oldRollArtifact(g_weaponGripRollDeg.load(std::memory_order_relaxed)))
        g_weaponGripRollDeg.store(0.0f, std::memory_order_relaxed);
    g_defaultProfile = ReadProfileFields();
    g_profileKey.clear();
    g_weaponProfiles.clear();
    for (const std::string& profileLine : profileLines)
        ParseWeaponProfileLine(profileLine);
    if (!layoutSizeSeen)
        for (auto& profile : g_weaponProfiles)
            if (oldRollArtifact(profile.second.v[kGripRollField]))
                profile.second.v[kGripRollField] = 0.0f;
    // The built-in weapon profiles (sdk/default_settings.h) for weapons this ini has none for -- an older ini gets the
    // profiles tuned since. Kept in memory; written out with the next save.
    int builtInAdded = 0;
    for (const char* p = kBuiltInSettings; *p;)
    {
        const char* end = strchr(p, '\n');
        std::string line = end ? std::string(p, end) : std::string(p);
        p = end ? end + 1 : p + line.size();
        if (!IsWeaponProfileLine(line))
            continue;
        const size_t keyStart = line.find('=') + 1;
        std::string key = line.substr(keyStart, line.find('|') - keyStart);
        if (!key.empty() && g_weaponProfiles.find(key) == g_weaponProfiles.end() && ParseWeaponProfileLine(line))
            ++builtInAdded;
    }
    if (builtInAdded > 0)
        MOHW_LOG(kLogFile, "LoadSettings: %d built-in weapon profile(s) added", builtInAdded);
    g_magPlatformLoaded = false;
    ResolvePlatformLocked(); // no weapon yet: the default profile's choice

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

void GetHandOffset(float position[3], float rotationDeg[3])
{
    for (int i = 0; i < 3; ++i)
    {
        position[i] = g_handOffsetPos[i].load(std::memory_order_relaxed);
        rotationDeg[i] = g_handOffsetRotDeg[i].load(std::memory_order_relaxed);
    }
}

void GetSecondaryGripOffset(float* right, float* up, float* forward)
{
    *right = g_secondaryGripRight.load(std::memory_order_relaxed);
    *up = g_secondaryGripUp.load(std::memory_order_relaxed);
    *forward = g_secondaryGripForward.load(std::memory_order_relaxed);
}

bool GetHudPlacementEnabled()
{
    return g_hudPlacementEnabled.load(std::memory_order_relaxed);
}

bool GetHideReticle()
{
    return g_hideReticle.load(std::memory_order_relaxed);
}

bool GetHideBodyPart(int index)
{
    return index >= 0 && index < kMaxBodyParts && g_hideBodyPart[index].load(std::memory_order_relaxed);
}

void SetBodyPartCountSeen(int count)
{
    g_bodyPartCountSeen.store(count < 0 ? 0 : (count > kMaxBodyParts ? kMaxBodyParts : count), std::memory_order_relaxed);
}

namespace {
// The render mode whose HUD values are in force: the selected one -- in the driving missions too, since they render in
// either mode (2026-11-18, hooks/native_stereo.h).
float ModeValue(const std::atomic<float>& alternating, const std::atomic<float>& twoView)
{
    const bool tv = g_nativeStereo.load(std::memory_order_relaxed);
    return (tv ? twoView : alternating).load(std::memory_order_relaxed);
}
} // namespace

float GetHudScale()
{
    return ModeValue(g_hudScale, g_hudScaleTv);
}

float GetMenuScreenScale()
{
    return ModeValue(g_menuScreenScale, g_menuScreenScaleTv);
}

namespace {
// The automatic UI layout scale (2026-11-22): the author's, 1.5 on a 5824 wide screen (two eyes of 2912), kept as the
// same layout width at any resolution, about 3883. By the width, since a menu's screen in the headset is as wide as
// MenuScreenScale allows whatever the image's shape (draw_trace_diag.cpp ComputeHudRectForEye, keepAspect): the same
// layout width is the same apparent size.
constexpr float kUiLayoutBaseScale = 1.5f;
constexpr float kUiLayoutBaseWidth = 5824.0f;
} // namespace

float GetUiLayoutScale(int screenWidth)
{
    if (screenWidth <= 0)
        return 1.0f;
    return ModeValue(g_uiLayoutSize, g_uiLayoutSizeTv) * kUiLayoutBaseScale *
           (static_cast<float>(screenWidth) / kUiLayoutBaseWidth);
}

float GetMenuScreenDepth()
{
    const float d = ModeValue(g_menuScreenDepth, g_menuScreenDepthTv);
    return d >= 0.3f && d <= 20.0f ? d : kDefaultMenuScreenDepth;
}

void NoteLevelRenderMode(bool twoView)
{
    g_levelModeTwoView.store(twoView, std::memory_order_relaxed);
    g_levelModeKnown.store(true, std::memory_order_relaxed);
}

int GetDrivingCameraIndex()
{
    return static_cast<int>(g_drivingCameraIndex.load(std::memory_order_relaxed) + 0.5f);
}

int GetDrivingCameraIndexTo()
{
    return static_cast<int>(g_drivingCameraIndexTo.load(std::memory_order_relaxed) + 0.5f);
}

bool GetDrivingHeadLookInvert()
{
    return g_drivingHeadLookInvert.load(std::memory_order_relaxed);
}

bool GetDrivingHeadLookAuto()
{
    return g_drivingHeadLookAuto.load(std::memory_order_relaxed);
}

float GetDrivingStickLookRangeDeg()
{
    return g_drivingStickLookRangeDeg.load(std::memory_order_relaxed);
}

float GetDrivingStickDeadzone()
{
    return g_drivingStickDeadzone.load(std::memory_order_relaxed);
}

bool GetDrivingStereo()
{
    return g_drivingStereo.load(std::memory_order_relaxed);
}

bool GetDrivingLateNextEye()
{
    return g_drivingLateNextEye.load(std::memory_order_relaxed);
}

bool GetSpecializedSequenceMotion()
{
    return g_specializedSequenceMotion.load(std::memory_order_relaxed);
}

bool GetBoatSixDof()
{
    return GetSpecializedSequenceMotion() && g_boatSixDof.load(std::memory_order_relaxed);
}

bool GetBoatGunAim()
{
    return g_boatGunAim.load(std::memory_order_relaxed);
}

bool GetBoatWheel()
{
    return g_boatWheel.load(std::memory_order_relaxed);
}

float GetBoatGunAimStrength()
{
    return g_boatGunAimStrength.load(std::memory_order_relaxed);
}

float GetBoatStickDeadzone()
{
    return g_boatStickDeadzone.load(std::memory_order_relaxed);
}

float GetBoatWheelLockDeg()
{
    return g_boatWheelLockDeg.load(std::memory_order_relaxed);
}

void GetBoatWheelPlacement(float* forward, float* down, float* right, float* tiltDeg)
{
    *forward = g_boatWheelForward.load(std::memory_order_relaxed);
    *down = g_boatWheelDown.load(std::memory_order_relaxed);
    *right = g_boatWheelRight.load(std::memory_order_relaxed);
    *tiltDeg = g_boatWheelTiltDeg.load(std::memory_order_relaxed);
}

void GetBoatWheelLook(float* radius, float* thickness, float* opacity)
{
    *radius = g_boatWheelRadius.load(std::memory_order_relaxed);
    *thickness = g_boatWheelThickness.load(std::memory_order_relaxed);
    *opacity = g_boatWheelOpacity.load(std::memory_order_relaxed);
}

bool GetDrivingSteeringWheel()
{
    return g_drivingSteeringWheel.load(std::memory_order_relaxed);
}

float GetDrivingWheelLockDeg()
{
    return g_drivingWheelLockDeg.load(std::memory_order_relaxed);
}

void GetDrivingWheelPlacement(float* forward, float* down, float* right, float* tiltDeg)
{
    *forward = g_drivingWheelForward.load(std::memory_order_relaxed);
    *down = g_drivingWheelDown.load(std::memory_order_relaxed);
    *right = g_drivingWheelRight.load(std::memory_order_relaxed);
    *tiltDeg = g_drivingWheelTiltDeg.load(std::memory_order_relaxed);
}

void GetDrivingWheelLook(float* radius, float* thickness, float* opacity)
{
    *radius = g_drivingWheelRadius.load(std::memory_order_relaxed);
    *thickness = g_drivingWheelThickness.load(std::memory_order_relaxed);
    *opacity = g_drivingWheelOpacity.load(std::memory_order_relaxed);
}


int GetDebugHideDraw()
{
    return static_cast<int>(g_debugHideDraw.load(std::memory_order_relaxed) + 0.5f);
}


void GetHudOffsetDeg(float* x, float* y)
{
    *x = ModeValue(g_hudOffsetXDeg, g_hudOffsetXDegTv);
    *y = ModeValue(g_hudOffsetYDeg, g_hudOffsetYDegTv);
}

float GetHudIpdScale()
{
    return ModeValue(g_hudIpdScale, g_hudIpdScaleTv);
}

float GetHudDepth()
{
    return ModeValue(g_hudDepth, g_hudDepthTv);
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

bool GetArmIkEnabled()
{
    return g_armIkEnabled.load(std::memory_order_relaxed);
}

bool GetHoldHandPose()
{
    return g_holdHandPose.load(std::memory_order_relaxed);
}

void GetLeftHandIkOffset(float position[3], float rotationDeg[3])
{
    for (int i = 0; i < 3; ++i)
    {
        position[i] = g_leftHandIkPos[i].load(std::memory_order_relaxed);
        rotationDeg[i] = g_leftHandIkRotDeg[i].load(std::memory_order_relaxed);
    }
}

bool GetManualReloadEnabled()
{
    return g_manualReloadEnabled.load(std::memory_order_relaxed);
}

float GetMagazinePouchRadius()
{
    return g_magazinePouchRadius.load(std::memory_order_relaxed);
}

// Not in the driving missions (2026-11-18): game\MOHWD.exe ran black and at 1-10 fps on the desktop and in the headset,
// sound and input still going, and the setting arrived (2026-11-01) after its last good run -- an older build that may
// not take a fullscreen request answered without being carried out. It takes the display as it did when it worked;
// the logs it now always writes (sdk/logging.h g_drivingLogs) will say whether that was it.
bool GetKeepGameWindowed()
{
    return g_keepGameWindowed.load(std::memory_order_relaxed) && !g_drivingExe;
}

bool GetNativeStereoEnabled()
{
    return g_nativeStereo.load(std::memory_order_relaxed);
}

bool GetSkyExactRaysEnabled()
{
    return g_skyExactRays.load(std::memory_order_relaxed);
}

bool GetScreenEffectsBothEyes()
{
    return g_screenEffectsBothEyes.load(std::memory_order_relaxed);
}

bool GetDepthOfFieldTwoView()
{
    return g_depthOfFieldTwoView.load(std::memory_order_relaxed);
}

bool GetRenderScaleFromSteamVr()
{
    return g_renderScaleFromSteamVr.load(std::memory_order_relaxed);
}

bool GetEyeSizedBackbuffer()
{
    return g_eyeSizedBackbuffer.load(std::memory_order_relaxed);
}

bool GetScriptedScenesFromLocks()
{
    return g_scriptedScenesFromLocks.load(std::memory_order_relaxed);
}

bool GetShowWeaponSockets()
{
    return g_showWeaponSockets.load(std::memory_order_relaxed);
}

void GetGunSocketOffset(float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = g_gunSocketOffset[i].load(std::memory_order_relaxed);
}

MagazineProfile GetMagazineProfile()
{
    MagazineProfile p{};
    for (int i = 0; i < 3; ++i)
    {
        p.grabOffset[i] = g_magazineGrabOffset[i].load(std::memory_order_relaxed);
        p.grabHalfSize[i] = g_magazineGrabHalfSize[i].load(std::memory_order_relaxed);
        p.aimPoint[i] = g_magazineAimPoint[i].load(std::memory_order_relaxed);
    }
    p.range = g_magazineRange.load(std::memory_order_relaxed);
    p.lockRange = g_magazineLockRange.load(std::memory_order_relaxed);
    p.seatRange = g_magazineSeatRange.load(std::memory_order_relaxed);
    return p;
}

MagazinePlatform GetMagazinePlatform()
{
    MagazinePlatform p{};
    int at = g_magPlatformIndex.load(std::memory_order_relaxed);
    p.index = at >= 0 && at < kMagazinePlatformCount ? at : kPlatformOther;
    p.name = kMagazinePlatformNames[p.index];
    float v[kPfCount];
    for (int f = 0; f < kPfCount; ++f)
        v[f] = g_magPlatform[f].load(std::memory_order_relaxed);
    for (int k = 0; k < 3; ++k)
    {
        p.holdPos[k] = v[kPfHoldRight + k];
        p.holdDeg[k] = v[kPfHoldPitch + k];
    }
    p.travel = v[kPfTravel] < 0.005f ? 0.005f : v[kPfTravel];
    p.pullForce = v[kPfPullForce] < 0.0f ? 0.0f : v[kPfPullForce];
    p.needsRelease = v[kPfNeedsRelease] >= 0.5f;
    return p;
}

const char* GetMagazinePlatformName(int index)
{
    return index >= 0 && index < kMagazinePlatformCount ? kMagazinePlatformNames[index] : "?";
}

float GetMagazineGripBreak()
{
    const float v = g_magazineGripBreak.load(std::memory_order_relaxed);
    return v < 0.05f ? 0.05f : v;
}

ScopeSettings GetScopeSettings()
{
    ScopeSettings s{};
    s.enabled = g_scopeEyeEnabled.load(std::memory_order_relaxed);
    s.eyeBox = g_scopeEyeBox.load(std::memory_order_relaxed);
    s.relief = g_scopeEyeRelief.load(std::memory_order_relaxed);
    s.apparentFovDeg = g_scopeApparentFov.load(std::memory_order_relaxed);
    s.reticle = g_scopeReticle.load(std::memory_order_relaxed);
    s.magnification = g_scopeMagnification.load(std::memory_order_relaxed);
    s.lensPicture = g_scopeLensPicture.load(std::memory_order_relaxed);
    s.lensRight = g_scopeLensRight.load(std::memory_order_relaxed);
    s.lensUp = g_scopeLensUp.load(std::memory_order_relaxed);
    s.lensForward = g_scopeLensForward.load(std::memory_order_relaxed);
    s.lensRadius = g_scopeLensRadius.load(std::memory_order_relaxed);
    s.needsMagnifier = g_scopeNeedsMagnifier.load(std::memory_order_relaxed) >= 0.5f;
    return s;
}

void GetBodyOffset(float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = g_bodyOffset[i].load(std::memory_order_relaxed);
}

bool GetBodyUpright()
{
    return g_bodyUpright.load(std::memory_order_relaxed);
}

bool GetStabilizeRig()
{
    return g_stabilizeRig.load(std::memory_order_relaxed);
}

bool GetShowGrabZones()
{
    return g_showGrabZones.load(std::memory_order_relaxed);
}

ArmIkSettings GetArmIkSettings()
{
    ArmIkSettings s{};
    for (int i = 0; i < 3; ++i)
    {
        s.rightHandPos[i] = g_rightHandIkPos[i].load(std::memory_order_relaxed);
        s.rightHandDeg[i] = g_rightHandIkRotDeg[i].load(std::memory_order_relaxed);
        s.shoulder[0][i] = g_shoulderOffset[0][i].load(std::memory_order_relaxed);
        s.shoulder[1][i] = g_shoulderOffset[1][i].load(std::memory_order_relaxed);
    }
    s.naturalElbow = g_naturalElbow.load(std::memory_order_relaxed);
    s.elbowSwivelDeg = g_elbowSwivelDeg.load(std::memory_order_relaxed);
    s.follow = g_elbowFollow.load(std::memory_order_relaxed);
    s.inKneeDeg = g_elbowInKneeDeg.load(std::memory_order_relaxed);
    s.inMaxDeg = g_elbowInMaxDeg.load(std::memory_order_relaxed);
    s.inNearBody = g_elbowInNearBody.load(std::memory_order_relaxed);
    s.outKneeDeg = g_elbowOutKneeDeg.load(std::memory_order_relaxed);
    s.outMaxDeg = g_elbowOutMaxDeg.load(std::memory_order_relaxed);
    s.smoothingMs = g_elbowSmoothingMs.load(std::memory_order_relaxed);
    s.forearmTwist = g_forearmTwist.load(std::memory_order_relaxed);
    return s;
}

void GetMagazinePouchOffset(float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = g_magazinePouch[i].load(std::memory_order_relaxed);
}

void GetMagazinePouchRotation(float deg[3])
{
    for (int i = 0; i < 3; ++i)
        deg[i] = g_magazinePouchRot[i].load(std::memory_order_relaxed);
}

int GetMagazinePouchGrab()
{
    const int mode = static_cast<int>(g_magazinePouchGrab.load(std::memory_order_relaxed) + 0.5f);
    return mode < kPouchGrabAsGrabbed ? kPouchGrabAsGrabbed : mode > kPouchGrabSnap ? kPouchGrabSnap : mode;
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

bool GetDotsOnlyWithGrip()
{
    return g_dotsOnlyWithGrip.load(std::memory_order_relaxed);
}

void GetControllerAimOffsetDeg(float* up, float* right)
{
    *up = g_controllerAimUpDeg.load(std::memory_order_relaxed);
    *right = g_controllerAimRightDeg.load(std::memory_order_relaxed);
}

float GetAdsSwayScale()
{
    return g_adsSwayScale.load(std::memory_order_relaxed);
}

bool GetThumbTapBumpers()
{
    return g_thumbTapBumpers.load(std::memory_order_relaxed);
}

bool GetGamepadWithoutFocus()
{
    return g_gamepadWithoutFocus.load(std::memory_order_relaxed);
}

bool GetGripDoubleTapBumpers()
{
    return g_gripDoubleTapBumpers.load(std::memory_order_relaxed);
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

bool GetLaserBeamEnabled()
{
    return g_laserBeamEnabled.load(std::memory_order_relaxed);
}

float GetLaserBeamOpacity()
{
    float v = g_laserBeamOpacity.load(std::memory_order_relaxed);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float GetLaserBeamWidthMeters()
{
    float mm = g_laserBeamWidthMm.load(std::memory_order_relaxed);
    return (mm < 0.5f ? 0.5f : mm) * 0.001f;
}

float GetLaserStartMeters()
{
    float v = g_laserStartMeters.load(std::memory_order_relaxed);
    return v < 0.0f ? 0.0f : (v > 2.0f ? 2.0f : v);
}

void GetLaserBeamStartOffset(float* right, float* up)
{
    *right = g_laserBeamStartRight.load(std::memory_order_relaxed);
    *up = g_laserBeamStartUp.load(std::memory_order_relaxed);
}

int GetLaserRaycastEveryFrames()
{
    int n = static_cast<int>(g_laserRaycastEveryFrames.load(std::memory_order_relaxed) + 0.5f);
    return n < 1 ? 1 : (n > 30 ? 30 : n);
}

void GetWeaponGripRotationDeg(float out[3])
{
    out[0] = g_weaponGripPitchDeg.load(std::memory_order_relaxed);
    out[1] = g_weaponGripYawDeg.load(std::memory_order_relaxed);
    out[2] = g_weaponGripRollDeg.load(std::memory_order_relaxed);
}

// ---- Menu table (2026-09-26, for openvr_direct/vr_overlay.cpp's SteamVR dashboard tab) ----------------------------
namespace {

constexpr int kRowBothModes = 0;
constexpr int kRowAlternatingEye = 1;
constexpr int kRowTwoView = 2;

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
    // A toggle shown as a two-way choice (the render mode): the text for off and for on.
    const char* offText = nullptr;
    const char* onText = nullptr;
    int mode = kRowBothModes; // which render mode's menu shows the row
    // A number shown as a choice (2026-11-14): choiceTexts[value], steps cycle 0..choiceCount-1.
    const char* const* choiceTexts = nullptr;
    int choiceCount = 0;
    bool platformChoice = false; // the gun's magazine platform: -1 = auto, then each platform by name
};

// Game actions a holster can be set to: every keyboard-bound action in the game's own profile, filled in by
// openvr_direct/vr_hands.cpp (SetHolsterActionChoices) once it has read the profile. Guarded by g_holsterMutex.
std::vector<std::string> g_holsterChoices;

const char kZodiacGroup[] = "Sequence profile: Zodiac Sequence";

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
    // RENDER MODE (2026-11-12): one DLL, two renderers; a change applies fully at the next level load (its note row
    // follows while the level was built with the other mode, see MenuRows).
    {"View", "Render mode", &g_nativeStereo, nullptr, 0, 0, 0, 0, "", -1, "Alternating eye", "Engine two-view"},
    {"View", "IPD scale", nullptr, &g_ipdScale, 0.5f, 2.0f, 0.02f, 2, "x"},
    {"View", "World scale (frustum)", nullptr, &g_frustumScale, 0.5f, 2.0f, 0.02f, 2, "x"},
    {"View", "Rotation smoothing (keep on)", &g_rotationSmoothingEnabled, nullptr, 0, 0, 0, 0, ""},
    {"View", "Smoothing window", nullptr, &g_rotationSmoothingWindowMs, 10.0f, 200.0f, 1.0f, 1, "ms"},
    {"View", "Keep game out of exclusive fullscreen", &g_keepGameWindowed, nullptr, 0, 0, 0, 0, ""},
    {"View", "Exact sky rays (clouds match both eyes)", &g_skyExactRays, nullptr, 0, 0, 0, 0, ""},
    {"View", "Screen effects (dust) in both eyes", &g_screenEffectsBothEyes, nullptr, 0, 0, 0, 0, "", -1, nullptr, nullptr,
     kRowTwoView},
    {"View", "Depth of field (doubles the image)", &g_depthOfFieldTwoView, nullptr, 0, 0, 0, 0, "", -1, nullptr, nullptr,
     kRowTwoView},
    {"View", "3D render scale from SteamVR resolution (experimental)", &g_renderScaleFromSteamVr, nullptr, 0, 0, 0, 0, ""},
    {"View", "Backbuffer at SteamVR eye size (experimental)", &g_eyeSizedBackbuffer, nullptr, 0, 0, 0, 0, "", -1, nullptr,
     nullptr, kRowTwoView},
    {"View", "Scripted scenes follow the game's input locks", &g_scriptedScenesFromLocks, nullptr, 0, 0, 0, 0, ""},
    // HUD right after View (2026-11-12, the user). The numbers are per render mode: each has one row per mode, and only
    // the selected mode's shows.
    {"HUD", "HUD placement", &g_hudPlacementEnabled, nullptr, 0, 0, 0, 0, ""},
    {"HUD", "Menu / movie screen size", nullptr, &g_menuScreenScale, 0.3f, 1.2f, 0.01f, 2, "x", -1, nullptr, nullptr,
     kRowAlternatingEye},
    {"HUD", "Menu / movie screen size", nullptr, &g_menuScreenScaleTv, 0.3f, 1.2f, 0.01f, 2, "x", -1, nullptr, nullptr,
     kRowTwoView},
    {"HUD", "Menu / movie screen distance", nullptr, &g_menuScreenDepth, 0.5f, 10.0f, 0.1f, 1, "m", -1, nullptr, nullptr,
     kRowAlternatingEye},
    {"HUD", "Menu / movie screen distance", nullptr, &g_menuScreenDepthTv, 0.5f, 10.0f, 0.1f, 1, "m", -1, nullptr, nullptr,
     kRowTwoView},
    {"HUD", "Hud Element Scale (1 = auto by resolution)", nullptr, &g_uiLayoutSize, 0.3f, 2.0f, 0.05f, 2, "x", -1, nullptr,
     nullptr, kRowAlternatingEye},
    {"HUD", "Hud Element Scale (1 = auto by resolution)", nullptr, &g_uiLayoutSizeTv, 0.3f, 2.0f, 0.05f, 2, "x", -1, nullptr,
     nullptr, kRowTwoView},
    {"HUD", "Debug: hide draw # (0 off)", nullptr, &g_debugHideDraw, 0.0f, 128.0f, 1.0f, 0, ""},
    {"HUD", "Hide reticle (may clip pause-menu items)", &g_hideReticle, nullptr, 0, 0, 0, 0, ""},
    {"HUD", "HUD scale", nullptr, &g_hudScale, 0.3f, 1.2f, 0.01f, 2, "x", -1, nullptr, nullptr, kRowAlternatingEye},
    {"HUD", "HUD scale", nullptr, &g_hudScaleTv, 0.3f, 1.2f, 0.01f, 2, "x", -1, nullptr, nullptr, kRowTwoView},
    {"HUD", "HUD depth", nullptr, &g_hudDepth, 0.1f, 10.0f, 0.1f, 1, "m", -1, nullptr, nullptr, kRowAlternatingEye},
    {"HUD", "HUD depth", nullptr, &g_hudDepthTv, 0.1f, 10.0f, 0.1f, 1, "m", -1, nullptr, nullptr, kRowTwoView},
    {"HUD", "HUD IPD (x render IPD)", nullptr, &g_hudIpdScale, 0.0f, 4.0f, 0.02f, 2, "x", -1, nullptr, nullptr,
     kRowAlternatingEye},
    {"HUD", "HUD IPD (x render IPD)", nullptr, &g_hudIpdScaleTv, 0.0f, 4.0f, 0.02f, 2, "x", -1, nullptr, nullptr,
     kRowTwoView},
    {"HUD", "HUD offset right", nullptr, &g_hudOffsetXDeg, -30.0f, 30.0f, 0.5f, 1, "deg", -1, nullptr, nullptr,
     kRowAlternatingEye},
    {"HUD", "HUD offset right", nullptr, &g_hudOffsetXDegTv, -30.0f, 30.0f, 0.5f, 1, "deg", -1, nullptr, nullptr,
     kRowTwoView},
    {"HUD", "HUD offset up", nullptr, &g_hudOffsetYDeg, -30.0f, 30.0f, 0.5f, 1, "deg", -1, nullptr, nullptr,
     kRowAlternatingEye},
    {"HUD", "HUD offset up", nullptr, &g_hudOffsetYDegTv, -30.0f, 30.0f, 0.5f, 1, "deg", -1, nullptr, nullptr,
     kRowTwoView},
    {"Controls", "Stick turn speed", nullptr, &g_vrTurnSpeed, 0.2f, 3.0f, 0.05f, 2, "x"},
    {"Controls", "Stick full deflection", nullptr, &g_vrStickFullDeflection, 0.5f, 1.0f, 0.01f, 2, ""},
    {"Controls", "Two-hand grab radius", nullptr, &g_twoHandGrabRadius, 0.05f, 0.5f, 0.01f, 2, "m"},
    {"Controls", "Two-hand reach", nullptr, &g_twoHandReach, 0.2f, 1.5f, 0.05f, 2, "m"},
    {"Controls", "Remove shot spread", &g_removeShotSpread, nullptr, 0, 0, 0, 0, ""},
    {"Controls", "ADS sway (0 = none, 1 = game's)", nullptr, &g_adsSwayScale, 0.0f, 1.0f, 0.05f, 2, "x"},
    {"Controls", "Fire / ADS as mouse (off = RT / LT)", &g_fireViaMouse, nullptr, 0, 0, 0, 0, ""},
    {"Controls", "Thumbrest double-tap = LB / RB", &g_thumbTapBumpers, nullptr, 0, 0, 0, 0, ""},
    {"Controls", "Grip double-tap = LB / RB (no thumbrest)", &g_gripDoubleTapBumpers, nullptr, 0, 0, 0, 0, ""},
    {"Controls", "Controllers work without window focus", &g_gamepadWithoutFocus, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Left-handed", &g_leftHanded, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Hold grip to keep weapon", &g_holsterHoldToKeep, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Holster size", nullptr, &g_holsterRadius, 0.05f, 0.4f, 0.01f, 2, "m"},
    {"Hands", "Holster height offset (+ lower)", nullptr, &g_holsterDrop, -0.4f, 0.4f, 0.01f, 2, "m"},
    {"Hands", "Hand offset right (all weapons)", nullptr, &g_handOffsetPos[0], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Hands", "Hand offset up", nullptr, &g_handOffsetPos[1], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Hands", "Hand offset forward", nullptr, &g_handOffsetPos[2], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Hands", "Hand rotation pitch (+ up)", nullptr, &g_handOffsetRotDeg[0], -45.0f, 45.0f, 0.5f, 1, "deg"},
    {"Hands", "Hand rotation yaw (+ right)", nullptr, &g_handOffsetRotDeg[1], -45.0f, 45.0f, 0.5f, 1, "deg"},
    {"Hands", "Hand rotation roll (+ right)", nullptr, &g_handOffsetRotDeg[2], -45.0f, 45.0f, 0.5f, 1, "deg"},
    {"Hands", "Gun offset in the hand right (all guns)", nullptr, &g_gunSocketOffset[0], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Hands", "Gun offset in the hand up", nullptr, &g_gunSocketOffset[1], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Hands", "Gun offset in the hand forward", nullptr, &g_gunSocketOffset[2], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Hands", "Show weapon sockets (debug lines)", &g_showWeaponSockets, nullptr, 0, 0, 0, 0, ""},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Show grab zones (setup)", &g_showGrabZones, nullptr, 0, 0, 0, 0, ""},
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
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Mag pouch X", nullptr, &g_magazinePouch[0], -0.6f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Mag pouch Y", nullptr, &g_magazinePouch[1], -1.2f, 0.3f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Mag pouch Z", nullptr, &g_magazinePouch[2], -0.5f, 0.6f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Mag pouch grab radius", nullptr, &g_magazinePouchRadius, 0.03f, 0.4f, 0.01f, 2, "m"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Mag in pouch pitch", nullptr, &g_magazinePouchRot[0], -180.0f, 180.0f, 5.0f, 0, "deg"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Mag in pouch yaw", nullptr, &g_magazinePouchRot[1], -180.0f, 180.0f, 5.0f, 0, "deg"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Mag in pouch roll", nullptr, &g_magazinePouchRot[2], -180.0f, 180.0f, 5.0f, 0, "deg"},
    {"Holsters (X = right, Y = up, Z = forward, m from head)", "Mag taken from the pouch", nullptr, &g_magazinePouchGrab,
     0.0f, 2.0f, 1.0f, 0, "", -1, nullptr, nullptr, kRowBothModes, kPouchGrabChoices, 3},
    {"Holster actions", "Stomach", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterStomach},
    {"Holster actions", "Hips", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterHip},
    {"Holster actions", "Chest", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterChest},
    {"Holster actions", "Left shoulder", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterLeftShoulder},
    {"Holster actions", "Right shoulder", nullptr, nullptr, 0, 0, 0, 0, "", kHolsterRightShoulder},
    {"Hands", "Dots only with weapon grip", &g_dotsOnlyWithGrip, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Optic dot", &g_opticDotEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Optic dot green (off = red)", &g_opticDotGreen, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Aim ray dot", &g_sightDotEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Aim ray dot green (off = red)", &g_rayDotGreen, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Laser max range (dot when nothing's hit)", nullptr, &g_sightZeroDistance, 5.0f, 1000.0f, 5.0f, 0, "m"},
    {"Hands", "Laser beam", &g_laserBeamEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Hands", "Laser beam opacity", nullptr, &g_laserBeamOpacity, 0.05f, 1.0f, 0.05f, 2, ""},
    {"Hands", "Laser beam width", nullptr, &g_laserBeamWidthMm, 1.0f, 20.0f, 0.5f, 1, "mm"},
    {"Hands", "Laser raycast every N frames", nullptr, &g_laserRaycastEveryFrames, 1.0f, 30.0f, 1.0f, 0, ""},
    {"Hands", "Dot size", nullptr, &g_sightDotSizeDeg, 0.05f, 2.0f, 0.05f, 2, "deg"},
    {"Arm IK", "Arms follow the controllers", &g_armIkEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Arm IK", "Hold hands and gun still (no idle animations)", &g_holdHandPose, nullptr, 0, 0, 0, 0, ""},
    {"Arm IK", "Right hand right (on its controller)", nullptr, &g_rightHandIkPos[0], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Arm IK", "Right hand up", nullptr, &g_rightHandIkPos[1], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Arm IK", "Right hand forward", nullptr, &g_rightHandIkPos[2], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Arm IK", "Right hand pitch", nullptr, &g_rightHandIkRotDeg[0], -180.0f, 180.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Right hand yaw", nullptr, &g_rightHandIkRotDeg[1], -180.0f, 180.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Right hand roll", nullptr, &g_rightHandIkRotDeg[2], -180.0f, 180.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Left hand right", nullptr, &g_leftHandIkPos[0], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Arm IK", "Left hand up", nullptr, &g_leftHandIkPos[1], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Arm IK", "Left hand forward", nullptr, &g_leftHandIkPos[2], -0.3f, 0.3f, 0.005f, 3, "m"},
    {"Arm IK", "Left hand pitch", nullptr, &g_leftHandIkRotDeg[0], -180.0f, 180.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Left hand yaw", nullptr, &g_leftHandIkRotDeg[1], -180.0f, 180.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Left hand roll", nullptr, &g_leftHandIkRotDeg[2], -180.0f, 180.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Left shoulder outward", nullptr, &g_shoulderOffset[0][0], -0.5f, 0.5f, 0.005f, 3, "m"},
    {"Arm IK", "Left shoulder up", nullptr, &g_shoulderOffset[0][1], -1.0f, 1.0f, 0.005f, 3, "m"},
    {"Arm IK", "Left shoulder forward", nullptr, &g_shoulderOffset[0][2], -1.0f, 1.0f, 0.005f, 3, "m"},
    {"Arm IK", "Right shoulder outward", nullptr, &g_shoulderOffset[1][0], -0.5f, 0.5f, 0.005f, 3, "m"},
    {"Arm IK", "Right shoulder up", nullptr, &g_shoulderOffset[1][1], -1.0f, 1.0f, 0.005f, 3, "m"},
    {"Arm IK", "Right shoulder forward", nullptr, &g_shoulderOffset[1][2], -1.0f, 1.0f, 0.005f, 3, "m"},
    {"Arm IK", "Forearm twist share", nullptr, &g_forearmTwist, 0.0f, 1.0f, 0.05f, 2, ""},
    {"Arm IK", "Natural elbow model", &g_naturalElbow, nullptr, 0, 0, 0, 0, ""},
    {"Arm IK", "Elbow swivel (natural model off)", nullptr, &g_elbowSwivelDeg, -90.0f, 90.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Elbow follows wrist roll", nullptr, &g_elbowFollow, 0.0f, 1.0f, 0.05f, 2, ""},
    {"Arm IK", "Elbow inward: exact up to", nullptr, &g_elbowInKneeDeg, 0.0f, 150.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Elbow inward: at most", nullptr, &g_elbowInMaxDeg, 0.0f, 170.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Elbow inward near the body", nullptr, &g_elbowInNearBody, 0.0f, 1.0f, 0.05f, 2, "x"},
    {"Arm IK", "Elbow outward: exact up to", nullptr, &g_elbowOutKneeDeg, 0.0f, 150.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Elbow outward: at most", nullptr, &g_elbowOutMaxDeg, 0.0f, 170.0f, 1.0f, 0, "deg"},
    {"Arm IK", "Elbow smoothing", nullptr, &g_elbowSmoothingMs, 0.0f, 300.0f, 10.0f, 0, "ms"},
    {"Body", "Body right", nullptr, &g_bodyOffset[0], -1.0f, 1.0f, 0.005f, 3, "m"},
    {"Body", "Body up", nullptr, &g_bodyOffset[1], -2.0f, 2.0f, 0.005f, 3, "m"},
    {"Body", "Body forward", nullptr, &g_bodyOffset[2], -2.0f, 2.0f, 0.005f, 3, "m"},
    {"Body", "Keep body upright (no tilt with the view)", &g_bodyUpright, nullptr, 0, 0, 0, 0, ""},
    {"Body", "Keep body and gun steady on head turns", &g_stabilizeRig, nullptr, 0, 0, 0, 0, ""},
    {"Weapon", "Gun + shots follow controller (Numpad .)", &g_weaponDriveEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Weapon", "Manual reload", &g_manualReloadEnabled, nullptr, 0, 0, 0, 0, ""},
    {"Weapon", "Magazine grip lets go beyond", nullptr, &g_magazineGripBreak, 0.05f, 1.0f, 0.01f, 2, "m"},
    // SCOPED EYE (2026-11-14, prototype): engine two-view only.
    {"Scope (prototype, engine two-view)", "Scoped eye: look through the scope", &g_scopeEyeEnabled, nullptr, 0, 0, 0, 0, "",
     -1, nullptr, nullptr, kRowTwoView},
    {"Scope (prototype, engine two-view)", "Eye box (how close to the sight line)", nullptr, &g_scopeEyeBox, 0.01f, 0.1f,
     0.005f, 3, "m", -1, nullptr, nullptr, kRowTwoView},
    {"Scope (prototype, engine two-view)", "Eye relief (how far behind the hand)", nullptr, &g_scopeEyeRelief, 0.1f, 1.0f,
     0.01f, 2, "m", -1, nullptr, nullptr, kRowTwoView},
    {"Scope (prototype, engine two-view)", "Eyepiece size in view", nullptr, &g_scopeApparentFov, 8.0f, 60.0f, 1.0f, 0,
     "deg", -1, nullptr, nullptr, kRowTwoView},
    {"Scope (prototype, engine two-view)", "Crosshair on the impact point (holographic)", &g_scopeReticle, nullptr, 0, 0,
     0, 0, "", -1, nullptr, nullptr, kRowTwoView},
    {"Scope (prototype, engine two-view)", "Lens picture (screen space, both eyes)", &g_scopeLensPicture, nullptr, 0, 0, 0,
     0, "", -1, nullptr, nullptr, kRowTwoView},
    // The equipped weapon's profile (SetWeaponProfile): the group title names the weapon.
    {kProfileGroup, "Grip pitch", nullptr, &g_weaponGripPitchDeg, -180.0f, 180.0f, 1.0f, 0, "deg"},
    {kProfileGroup, "Grip yaw", nullptr, &g_weaponGripYawDeg, -180.0f, 180.0f, 1.0f, 0, "deg"},
    {kProfileGroup, "Grip roll", nullptr, &g_weaponGripRollDeg, -180.0f, 180.0f, 1.0f, 0, "deg"},
    {kProfileGroup, "Optic zero / height (+ up)", nullptr, &g_opticHeight, -0.3f, 0.3f, 0.001f, 3, "m"},
    {kProfileGroup, "Optic windage (+ right)", nullptr, &g_opticWindage, -0.3f, 0.3f, 0.001f, 3, "m"},
    {kProfileGroup, "Optic forward (from the hand)", nullptr, &g_opticDistance, 0.0f, 300.0f, 0.01f, 2, "m"},
    {kProfileGroup, "Controller aim offset up", nullptr, &g_controllerAimUpDeg, -10.0f, 10.0f, 0.1f, 1, "deg"},
    {kProfileGroup, "Controller aim offset right", nullptr, &g_controllerAimRightDeg, -10.0f, 10.0f, 0.1f, 1, "deg"},
    {kProfileGroup, "Magazine grab box right", nullptr, &g_magazineGrabOffset[0], -0.4f, 0.4f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine grab box up", nullptr, &g_magazineGrabOffset[1], -0.4f, 0.4f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine grab box forward", nullptr, &g_magazineGrabOffset[2], -0.4f, 0.4f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine grab box half width", nullptr, &g_magazineGrabHalfSize[0], 0.01f, 0.4f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine grab box half height", nullptr, &g_magazineGrabHalfSize[1], 0.01f, 0.4f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine grab box half depth", nullptr, &g_magazineGrabHalfSize[2], 0.01f, 0.4f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine aim point right", nullptr, &g_magazineAimPoint[0], -0.5f, 0.5f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine aim point up", nullptr, &g_magazineAimPoint[1], -0.5f, 1.0f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine aim point forward", nullptr, &g_magazineAimPoint[2], -0.5f, 0.5f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine starts turning at", nullptr, &g_magazineRange, 0.02f, 0.8f, 0.01f, 2, "m"},
    {kProfileGroup, "Magazine locks horizontal at", nullptr, &g_magazineLockRange, 0.005f, 0.3f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine seats within", nullptr, &g_magazineSeatRange, 0.002f, 0.1f, 0.002f, 3, "m"},
    {kProfileGroup, "Secondary grip offset right", nullptr, &g_secondaryGripRight, -0.3f, 0.3f, 0.005f, 3, "m"},
    {kProfileGroup, "Secondary grip offset up", nullptr, &g_secondaryGripUp, -0.3f, 0.3f, 0.005f, 3, "m"},
    {kProfileGroup, "Secondary grip offset forward", nullptr, &g_secondaryGripForward, -0.3f, 0.3f, 0.005f, 3, "m"},
    {kProfileGroup, "Laser start (forward of the hand)", nullptr, &g_laserStartMeters, 0.0f, 1.5f, 0.01f, 2, "m"},
    {kProfileGroup, "Beam start right (visual)", nullptr, &g_laserBeamStartRight, -0.3f, 0.3f, 0.005f, 3, "m"},
    {kProfileGroup, "Beam start up (visual)", nullptr, &g_laserBeamStartUp, -0.3f, 0.3f, 0.005f, 3, "m"},
    {kProfileGroup, "Magazine platform", nullptr, &g_weaponMagPlatform, -1.0f, kMagazinePlatformCount - 1.0f, 1.0f, 0, "",
     -1, nullptr, nullptr, kRowBothModes, nullptr, 0, true},
    {kProfileGroup, "Scope magnification (0 = no scope)", nullptr, &g_scopeMagnification, 0.0f, 30.0f, 0.25f, 2, "x"},
    {kProfileGroup, "Scope shows", nullptr, &g_scopeNeedsMagnifier, 0.0f, 1.0f, 1.0f, 0, "", -1, nullptr, nullptr,
     kRowBothModes, kScopeShowChoices, 2},
    {kProfileGroup, "Scope lens right (from the optic zero)", nullptr, &g_scopeLensRight, -0.1f, 0.1f, 0.001f, 3, "m"},
    {kProfileGroup, "Scope lens up (from the optic zero)", nullptr, &g_scopeLensUp, -0.1f, 0.1f, 0.001f, 3, "m"},
    {kProfileGroup, "Scope lens forward (from the hand)", nullptr, &g_scopeLensForward, -0.2f, 0.5f, 0.005f, 3, "m"},
    {kProfileGroup, "Scope lens radius", nullptr, &g_scopeLensRadius, 0.005f, 0.05f, 0.001f, 3, "m"},
    // The equipped gun's magazine platform (2026-11-14): shared by every gun on it; the title names it.
    // The pouch grab's snap (2026-11-21, "Mag taken from the pouch"): the magazine's top from the controller, left hand
    // (mirrored in the right). With "Snapped, vertical from the grip", up is an offset from where the grip had it.
    {kPlatformGroup, "Mag in hand X (right)", nullptr, &g_magPlatform[kPfHoldRight], -0.3f, 0.3f, 0.005f, 3, "m"},
    {kPlatformGroup, "Mag in hand Y (up; offset from the grip)", nullptr, &g_magPlatform[kPfHoldUp], -0.3f, 0.3f, 0.005f,
     3, "m"},
    {kPlatformGroup, "Mag in hand Z (forward)", nullptr, &g_magPlatform[kPfHoldForward], -0.3f, 0.3f, 0.005f, 3, "m"},
    {kPlatformGroup, "Mag in hand pitch", nullptr, &g_magPlatform[kPfHoldPitch], -180.0f, 180.0f, 5.0f, 0, "deg"},
    {kPlatformGroup, "Mag in hand yaw", nullptr, &g_magPlatform[kPfHoldYaw], -180.0f, 180.0f, 5.0f, 0, "deg"},
    {kPlatformGroup, "Mag in hand roll", nullptr, &g_magPlatform[kPfHoldRoll], -180.0f, 180.0f, 5.0f, 0, "deg"},
    {kPlatformGroup, "Travel to clear the magwell", nullptr, &g_magPlatform[kPfTravel], 0.005f, 0.25f, 0.005f, 3, "m"},
    {kPlatformGroup, "Pull force out (stretch before it moves)", nullptr, &g_magPlatform[kPfPullForce], 0.0f, 0.1f, 0.005f,
     3, "m"},
    {kPlatformGroup, "Needs the magazine release button", nullptr, &g_magPlatform[kPfNeedsRelease], 0.0f, 1.0f, 1.0f, 0,
     "", -1, nullptr, nullptr, kRowBothModes, kYesNoChoices, 2},
    {"Driving", "Head look", &g_drivingHeadLookAuto, nullptr, 0, 0, 0, 0, ""},
    {"Driving", "Invert head look", &g_drivingHeadLookInvert, nullptr, 0, 0, 0, 0, ""},
    {"Driving", "Stereo (full view)", &g_drivingStereo, nullptr, 0, 0, 0, 0, ""},
    {"Driving", "Stereo uses next eye (swap)", &g_drivingLateNextEye, nullptr, 0, 0, 0, 0, ""},
    {"Driving", "Stick look range (0 = render yaw)", nullptr, &g_drivingStickLookRangeDeg, 0.0f, 180.0f, 5.0f, 0, "deg"},
    {"Driving", "Stick deadzone compensation", nullptr, &g_drivingStickDeadzone, 0.0f, 0.5f, 0.01f, 2, ""},
    {"Driving", "Steering wheel (hold grip)", &g_drivingSteeringWheel, nullptr, 0, 0, 0, 0, ""},
    {"Driving", "Wheel turn for full lock", nullptr, &g_drivingWheelLockDeg, 20.0f, 540.0f, 5.0f, 0, "deg"},
    {"Driving", "Wheel distance forward", nullptr, &g_drivingWheelForward, 0.1f, 1.0f, 0.01f, 2, "m"},
    {"Driving", "Wheel below eyes", nullptr, &g_drivingWheelDown, 0.0f, 1.0f, 0.01f, 2, "m"},
    {"Driving", "Wheel offset right", nullptr, &g_drivingWheelRight, -0.5f, 0.5f, 0.01f, 2, "m"},
    {"Driving", "Wheel tilt (top away)", nullptr, &g_drivingWheelTiltDeg, 0.0f, 80.0f, 5.0f, 0, "deg"},
    {"Driving", "Wheel radius (drawn)", nullptr, &g_drivingWheelRadius, 0.08f, 0.4f, 0.01f, 2, "m"},
    {"Driving", "Wheel thickness (drawn)", nullptr, &g_drivingWheelThickness, 0.005f, 0.1f, 0.005f, 3, "m"},
    {"Driving", "Wheel opacity (0 = hidden)", nullptr, &g_drivingWheelOpacity, 0.0f, 1.0f, 0.05f, 2, ""},
    {"Driving", "Debug: manual views from # (0 = auto)", nullptr, &g_drivingCameraIndex, 0.0f, 64.0f, 1.0f, 0, ""},
    {"Driving", "Debug: manual views to # (0 = only from)", nullptr, &g_drivingCameraIndexTo, 0.0f, 64.0f, 1.0f, 0, ""},
    // The master, then a group per sequence (kSequenceFields), titled "Sequence profile: <sequence>".
    {"Specialized sequences", "Use 6DoF Motion For Specialized Sequences", &g_specializedSequenceMotion, nullptr, 0, 0, 0, 0,
     ""},
    {kZodiacGroup, "6DoF, hands on wheel / gun", &g_boatSixDof, nullptr, 0, 0, 0, 0, ""},
    {kZodiacGroup, "Grenade launcher: grips lever the gun", &g_boatGunAim, nullptr, 0, 0, 0, 0, ""},
    {kZodiacGroup, "Grenade launcher: follow strength", nullptr, &g_boatGunAimStrength, 0.2f, 3.0f, 0.1f, 1, "x"},
    {kZodiacGroup, "Steering wheel (hold grip)", &g_boatWheel, nullptr, 0, 0, 0, 0, ""},
    {kZodiacGroup, "Stick deadzone compensation", nullptr, &g_boatStickDeadzone, 0.0f, 0.5f, 0.01f, 2, ""},
    {kZodiacGroup, "Wheel turn for full lock", nullptr, &g_boatWheelLockDeg, 20.0f, 540.0f, 5.0f, 0, "deg"},
    {kZodiacGroup, "Wheel distance forward", nullptr, &g_boatWheelForward, 0.1f, 1.0f, 0.01f, 2, "m"},
    {kZodiacGroup, "Wheel below eyes", nullptr, &g_boatWheelDown, 0.0f, 1.0f, 0.01f, 2, "m"},
    {kZodiacGroup, "Wheel offset right", nullptr, &g_boatWheelRight, -0.5f, 0.5f, 0.01f, 2, "m"},
    {kZodiacGroup, "Wheel tilt (top away)", nullptr, &g_boatWheelTiltDeg, 0.0f, 80.0f, 5.0f, 0, "deg"},
    {kZodiacGroup, "Wheel radius (drawn)", nullptr, &g_boatWheelRadius, 0.08f, 0.4f, 0.01f, 2, "m"},
    {kZodiacGroup, "Wheel thickness (drawn)", nullptr, &g_boatWheelThickness, 0.005f, 0.1f, 0.005f, 3, "m"},
    {kZodiacGroup, "Wheel opacity (0 = hidden)", nullptr, &g_boatWheelOpacity, 0.0f, 1.0f, 0.05f, 2, ""},
    // LOG FILES (2026-11-14, sdk/logging.h): off by default, on for a bug report.
    {"Diagnostics", "Write log files (for bug reports; costs frames)", &g_logsEnabled, nullptr, 0, 0, 0, 0, ""},
};
constexpr int kMenuCount = static_cast<int>(sizeof(kMenu) / sizeof(kMenu[0]));

// Menu rows after the table: one toggle per body part (BodyPartRowCount). The group title carries the live part count;
// its buffer is only written by the menu (overlay) thread, the only caller, and stays put across rows.
char g_bodyPartGroup[96];

bool GetBodyPartMenuInfo(int part, MenuSettingInfo* out)
{
    if (part < 0 || part >= BodyPartRowCount() || part >= kMaxBodyParts)
        return false;
    int seen = g_bodyPartCountSeen.load(std::memory_order_relaxed);
    if (seen > 0)
        snprintf(g_bodyPartGroup, sizeof(g_bodyPartGroup), "Weapon: body parts hidden (%d in this level)", seen);
    else
        snprintf(g_bodyPartGroup, sizeof(g_bodyPartGroup), "Weapon: body parts hidden (no body drawn yet)");
    // Names from the first rifle / pistol (2026-09-19); other levels may order the parts differently.
    const char* knownAs[] = {" (legs, 1st rifle)", " (arms, 1st rifle)", " (hands, 1st rifle)"};
    snprintf(out->labelBuffer, sizeof(out->labelBuffer), "Hide part %d%s%s", part + 1, part < 3 ? knownAs[part] : "",
             seen > 0 && part >= seen ? " - not here" : "");
    out->group = g_bodyPartGroup;
    out->label = out->labelBuffer;
    out->isToggle = true;
    out->isChoice = false;
    out->isNote = false;
    out->value = g_hideBodyPart[part].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    out->step = 0.0f;
    out->decimals = 0;
    out->unit = "";
    out->text[0] = '\0';
    return true;
}

// THE ROWS ON SHOW (2026-11-12): the table's rows for the selected render mode (MenuEntry::mode), the render mode's
// reload note right after its row while it's due, then the body parts. Rebuilt on every call (the table is small); the
// overlay thread is the only caller, and the only one changing the render mode from the menu.
enum class RowKind
{
    Entry,
    Note,
    BodyPart
};
struct MenuRow
{
    RowKind kind;
    int i; // the table entry (Entry, Note: the render mode's) or the body part
};

bool RowShown(const MenuEntry& e)
{
    if (e.mode == kRowBothModes)
        return true;
    return e.mode == (g_nativeStereo.load(std::memory_order_relaxed) ? kRowTwoView : kRowAlternatingEye);
}

// The level in play was built with the other render mode: the change applies fully at the next level load.
bool ReloadNoteDue()
{
    return g_levelModeKnown.load(std::memory_order_relaxed) &&
           g_levelModeTwoView.load(std::memory_order_relaxed) != g_nativeStereo.load(std::memory_order_relaxed);
}

// Row `index` (out), or with index < 0 the number of rows (total).
bool FindMenuRow(int index, MenuRow* out, int* total)
{
    const bool note = ReloadNoteDue();
    int n = 0;
    for (int i = 0; i < kMenuCount; ++i)
    {
        if (!RowShown(kMenu[i]))
            continue;
        if (n++ == index)
        {
            *out = MenuRow{RowKind::Entry, i};
            return true;
        }
        if (note && kMenu[i].toggle == &g_nativeStereo && kMenu[i].onText && n++ == index)
        {
            *out = MenuRow{RowKind::Note, i};
            return true;
        }
    }
    const int parts = BodyPartRowCount();
    if (index >= n && index < n + parts)
    {
        *out = MenuRow{RowKind::BodyPart, index - n};
        return true;
    }
    if (total)
        *total = n + parts;
    return false;
}

} // namespace

int GetMenuSettingCount()
{
    MenuRow unused{};
    int total = 0;
    FindMenuRow(-1, &unused, &total);
    return total;
}

bool GetMenuSettingInfo(int index, MenuSettingInfo* out)
{
    MenuRow row{};
    if (index < 0 || !out || !FindMenuRow(index, &row, nullptr))
        return false;
    if (row.kind == RowKind::BodyPart)
        return GetBodyPartMenuInfo(row.i, out);
    const MenuEntry& e = kMenu[row.i];
    if (row.kind == RowKind::Note)
    {
        out->group = e.group;
        out->label = "Changed: reload the level to apply";
        out->isToggle = false;
        out->isChoice = false;
        out->isNote = true;
        out->value = 0.0f;
        out->step = 0.0f;
        out->decimals = 0;
        out->unit = "";
        out->text[0] = '\0';
        return true;
    }
    out->group = e.group == kProfileGroup    ? g_profileTitle.load(std::memory_order_acquire)
                 : e.group == kPlatformGroup ? g_platformTitle.load(std::memory_order_acquire)
                                             : e.group;
    out->label = e.label;
    out->isToggle = e.toggle != nullptr && !e.onText;
    out->isChoice = e.holsterZone >= 0 || (e.toggle != nullptr && e.onText != nullptr) || e.choiceTexts != nullptr ||
                    e.platformChoice;
    out->isNote = false;
    out->value = e.toggle ? (e.toggle->load(std::memory_order_relaxed) ? 1.0f : 0.0f)
                          : (e.number ? e.number->load(std::memory_order_relaxed) : 0.0f);
    out->step = e.step;
    out->decimals = e.decimals;
    out->unit = e.unit;
    out->text[0] = '\0';
    if (e.toggle && e.onText)
    {
        snprintf(out->text, sizeof(out->text), "%s", out->value != 0.0f ? e.onText : (e.offText ? e.offText : ""));
        return true;
    }
    if (e.choiceTexts)
    {
        int i = static_cast<int>(lroundf(out->value));
        i = i < 0 ? 0 : i >= e.choiceCount ? e.choiceCount - 1 : i;
        snprintf(out->text, sizeof(out->text), "%s", e.choiceTexts[i]);
        return true;
    }
    if (e.platformChoice)
    {
        const int i = static_cast<int>(lroundf(out->value));
        if (i < 0 || i >= kMagazinePlatformCount)
            snprintf(out->text, sizeof(out->text), "Auto: %s",
                     GetMagazinePlatformName(g_magPlatformIndex.load(std::memory_order_relaxed)));
        else
            snprintf(out->text, sizeof(out->text), "%s", kMagazinePlatformNames[i]);
        return true;
    }
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

void SetWeaponProfile(const char* key, const char* display)
{
    std::string next = key ? key : "";
    std::lock_guard<std::mutex> lock(g_fileMutex);
    if (next == g_profileKey)
        return;
    CaptureProfileLocked(); // the outgoing weapon's edits (in memory; the file is written on the next edit)
    g_profileKey = next;
    auto it = next.empty() ? g_weaponProfiles.end() : g_weaponProfiles.find(next);
    bool own = it != g_weaponProfiles.end();
    const WeaponProfile& p = own ? it->second : g_defaultProfile;
    for (int i = 0; i < kProfileFieldCount; ++i)
        kProfileFields[i]->store(p.v[i], std::memory_order_relaxed);
    // Title into the buffer not on show, then publish it.
    const char* shown = g_profileTitle.load(std::memory_order_acquire);
    char* buffer = shown == g_profileTitleBuffers[0] ? g_profileTitleBuffers[1] : g_profileTitleBuffers[0];
    if (next.empty())
        snprintf(buffer, sizeof(g_profileTitleBuffers[0]), "Weapon profile: default (no weapon)");
    else
        snprintf(buffer, sizeof(g_profileTitleBuffers[0]), "Weapon profile: %s%s", display && *display ? display : next.c_str(),
                 own ? "" : " (from default)");
    g_profileTitle.store(buffer, std::memory_order_release);
    MOHW_LOG(kLogFile, "weapon profile: %s -> %s", next.empty() ? "default" : next.c_str(),
              own ? "its own values" : "the default values");
    ResolvePlatformLocked(); // the new gun's magazine platform
}

void SetHolsterActionChoices(const std::vector<std::string>& actions)
{
    std::lock_guard<std::mutex> lock(g_holsterMutex);
    g_holsterChoices = actions;
}

void AdjustMenuSetting(int index, int steps)
{
    MenuRow row{};
    if (index < 0 || steps == 0 || !FindMenuRow(index, &row, nullptr) || row.kind == RowKind::Note)
        return;
    if (row.kind == RowKind::BodyPart)
    {
        int part = row.i;
        if (part >= kMaxBodyParts)
            return;
        g_hideBodyPart[part].store(!g_hideBodyPart[part].load(std::memory_order_relaxed), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_fileMutex);
        WriteSettingsFileLocked();
        return;
    }
    const MenuEntry& e = kMenu[row.i];
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
    else if (e.choiceTexts || e.platformChoice)
    {
        // Cycle, wrapping: the platform choice runs auto (-1), then each platform.
        const int lo = e.platformChoice ? -1 : 0;
        const int hi = e.platformChoice ? kMagazinePlatformCount - 1 : e.choiceCount - 1;
        int v = static_cast<int>(lroundf(e.number->load(std::memory_order_relaxed))) + (steps > 0 ? 1 : -1);
        v = v > hi ? lo : v < lo ? hi : v;
        e.number->store(static_cast<float>(v), std::memory_order_relaxed);
        if (e.platformChoice)
        {
            std::lock_guard<std::mutex> lock(g_fileMutex);
            ResolvePlatformLocked();
            WriteSettingsFileLocked();
            return;
        }
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
