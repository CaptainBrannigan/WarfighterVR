#pragma once

#include <string>
#include <vector>
// Lightweight persistent settings, player-adjustable via the existing debug
// hotkeys (F5-F8 today) -- values now survive a relaunch instead of
// resetting to compile-time defaults every session. Plain text key=value
// file, no external dependency, loosely modeled on bioshock-vr's
// vrpreset.ini approach (docs/motion_controls_research.md) but far smaller
// in scope for now. Lives in sdk/ rather than hooks/ since it's
// cross-cutting infrastructure, not a hook on the game itself.
//
// Scope: FOV scale and IPD scale (2026-08-04, the working baseline -- see
// docs/STATUS.md's Current status), plus head-driven aim/movement enable +
// sensitivity (2026-08-04, hooks/gameplay_input_hook.cpp). Designed to
// grow -- a controller-driven-aim toggle is planned once motion-controller
// work lands (see docs/ROADMAP.md's Motion controls section) as a new
// field in the same file, not new infrastructure.

namespace mohw {

// Reads mohwvr_settings.ini from next to the DLL if present; creates it
// with current (baseline) defaults if missing. Call once at startup, after
// ResolveModule() so the DLL's own directory is resolvable the same way
// sdk/logging.h's LogFilePath() already does.
void LoadSettings();

float GetFovScale();
void SetFovScale(float value); // updates the in-memory value AND persists to disk immediately

float GetIpdScale();
void SetIpdScale(float value);

// Head-driven aim/movement (hooks/gameplay_input_hook.cpp) -- defaults to
// DISABLED (unlike FOV/IPD, which default to the confirmed-working
// baseline): this is a new, not-yet-independently-verified hook on
// previously-unhooked gameplay-critical code, so a fresh install shouldn't
// silently start altering aim/movement without the player opting in via F12.
bool GetHeadAimEnabled();
void SetHeadAimEnabled(bool value);

float GetHeadAimSensitivity();
void SetHeadAimSensitivity(float value);

// Sign flips for hooks/aiming_controller_hook.cpp's yaw/pitch delta -- the
// HMD-orientation-to-yaw/pitch extraction (sdk/vr_math.h's QuatToYawPitch)
// and the axis mapping onto AimingController's fields are both
// best-guess/not yet independently live-validated (see that hook's header
// comment), so these exist to let a sign mistake be corrected with a
// keypress instead of a rebuild -- and so a future settings menu has a real
// toggle to expose rather than a recompile-only constant. Both default to
// false (no inversion); flip live if head-look ends up backwards.
bool GetHeadAimInvertYaw();
void SetHeadAimInvertYaw(bool value);

bool GetHeadAimInvertPitch();
void SetHeadAimInvertPitch(bool value);

// Whether hooks/aiming_controller_hook.cpp clamps its output pitch to
// AimingController's own live-read pitch limits (see that hook's
// kFallbackPitchClampMinDeg/MaxDeg). Defaults true (respect the game's own
// limits, safer/more consistent with normal gameplay); a player who wants
// unrestricted head pitch in VR can turn this off.
bool GetHeadAimClampPitch();
void SetHeadAimClampPitch(bool value);

// Render-side head ROLL (2026-08-22, hooks/fov_scale_hook.cpp) -- distinct
// from HeadAimInvertYaw/Pitch above, which are AimingController's own
// (gameplay-only) fields. Roll has no AimingController equivalent at all
// (that object only ever exposed yaw/pitch) -- this augments the camera
// transform directly, on top of whatever the game's native
// AimingController-to-camera sync already produced, additive and
// independent of yaw/pitch. Defaults OFF: a brand new, not yet
// live-validated feature, unlike the other axes here which only got
// flipped to default-on after live confirmation.
bool GetHeadRollEnabled();
void SetHeadRollEnabled(bool value);

bool GetHeadRollInvert();
void SetHeadRollInvert(bool value);

// Head POSITION (2026-09-21): real head translation (relative to the last recenter) moves the in-game camera.
// Scale 1.0 = 1 meter of real movement -> 1 game unit (very likely 1 m, see project memory). See hooks/head_position.cpp.
// True-per-eye-frustum view mode (companion mode 3): multiplier on the headset's real frustum the game renders. 1.0 = exactly the
// real FOV (1:1, undistorted); >1 renders a wider world into the same view (looks smaller), <1 narrower (looks magnified). F5/F6 in that mode.
float GetFrustumScale();
void SetFrustumScale(float value);

bool GetHeadPositionEnabled();
void SetHeadPositionEnabled(bool value);
float GetHeadPositionScale();
void SetHeadPositionScale(float value);
// Inverts the horizontal (forward + lateral) head-position axes together, i.e. a 180 degree turn of the XR->game alignment.
// Confirmed needed live 2026-09-21 (user: forward and lateral were both backwards); vertical is unaffected.
bool GetHeadPositionInvertHorizontal();
void SetHeadPositionInvertHorizontal(bool value);

// Render-side camera rotation smoothing (2026-08-23, hooks/fov_scale_hook.cpp's
// SmoothCameraRotation) -- live-diagnosed fix for MOHW's own gameplay
// simulation tick running at a fixed ~30Hz (confirmed via
// AimingControllerUpdate's own call-rate counter) while render runs at
// ~75-90Hz: the native camera-transform sync only meaningfully changes on
// those 30Hz ticks, so several consecutive render frames see an identical,
// frozen rotation before jumping all at once -- a staircase, perceived as
// head-turn jitter. Interpolates the OUTPUT rotation between the last two
// distinct ticks instead of snapping. Defaults OFF, same as every other
// brand-new/not-yet-live-validated feature in this project.
bool GetRotationSmoothingEnabled();
void SetRotationSmoothingEnabled(bool value);

// How long (in milliseconds) SmoothCameraRotation spreads a detected tick's
// rotation change over, i.e. roughly how many render frames the transition
// gets interpolated across at the current frame rate (window / frame time).
// Defaults to 1000/30 (~33.3ms) to match the live-measured ~30Hz gameplay
// tick exactly -- see GetRotationSmoothingEnabled's declaration comment.
// Live-tunable (PageUp/PageDown) so shorter (snappier, closer to the
// original staircase) vs. longer (smoother, more perceived lag) can be
// A/B'd directly instead of guessed at.
float GetRotationSmoothingWindowMs();
void SetRotationSmoothingWindowMs(float value);

// Live proof-of-concept for motion-controls groundwork (2026-08-23,
// hooks/constantbuffer_hook.cpp) -- zeroes out a RANGE of bones' 48-byte
// entries in the player's own confirmed bone-matrix buffer (see
// GetKnownPlayerBoneBuffer's declaration comment) before it reaches the
// GPU, to visually confirm the write path actually affects what's
// rendered. A range rather than a single index: any one bone's influence
// is blended with its neighbors' weights on shared vertices, so a lone
// zeroed bone can be too subtle to notice -- zeroing a wide range (e.g.
// the first half, indices 0-29) gives a much more obvious visual result,
// and narrowing the range down from there is how individual bones get
// identified once a broad test shows which half/quarter matters. Defaults
// OFF, range 0-29 (the first half of the 60-bone array).
bool GetBoneHideEnabled();
void SetBoneHideEnabled(bool value);

// Both stored as float like every other numeric setting here; cast to int
// when using. Inclusive range [start, end].
float GetBoneHideRangeStart();
void SetBoneHideRangeStart(float value);

float GetBoneHideRangeEnd();
void SetBoneHideRangeEnd(float value);

// Second, independent hide range (2026-08-26) -- ApplyBoneHideIfEnabled
// hides bones in EITHER [BoneHideRangeStart,End] OR [BoneHideRange2Start,
// End]. Needed because a single contiguous range can't express "hide the
// wrist/arm bones but keep the hand/finger bones visible" when those live
// in two disjoint sub-bands within one entity's buffer (confirmed live:
// right hand's wrist sits near the low end of its ~4-20 block, left
// wrist near the low end of its ~21-35 block -- two separate low bands,
// not one contiguous range). Defaults equal to the primary range's
// defaults so a fresh install behaves identically until explicitly set.
float GetBoneHideRange2Start();
void SetBoneHideRange2Start(float value);

float GetBoneHideRange2End();
void SetBoneHideRange2End(float value);

// Distance (world units) from bone-0's translation to the live camera
// position, used by hooks/constantbuffer_hook.cpp's player-bone-buffer
// disambiguation. Live-tunable (bracket keys) rather than a fixed
// constant: confirmed live (2026-08-25) that the player's OWN true
// distance varies frame to frame (observed 0.48-1.9 units), which
// overlaps the range at which nearby objects sharing the same buffer
// (an adjacent NPC, even a trashcan) also get caught -- no single fixed
// value cleanly separates the two, so this needs to be dialed in
// interactively per-scene rather than hardcoded. Starts tighter than the
// original 5.0 default given that finding.
float GetPlayerBoneDistanceThreshold();
void SetPlayerBoneDistanceThreshold(float value);

// Scales the Turn stick before it's handed to the game as the virtual gamepad's right stick (hooks/xinput_hook.cpp).
// Ini-only (VrTurnSpeed). Values above 1 reach full deflection sooner; the game's own controller look sensitivity
// still sets the fastest possible turn.
float GetVrTurnSpeed();

// Outer deadzone for both VR sticks (hooks/xinput_hook.cpp): deflection at or past this counts as full, since VR sticks
// often fall short of 1.0 in some directions (Touch right stick: 0.92 right vs 0.99 left). Ini-only
// (VrStickFullDeflection, default 0.9; 1.0 = off).
float GetVrStickFullDeflection();

// Two-handed aim (openvr_direct/vr_input.cpp): pressing the off-hand grip engages it when the left hand is within
// TwoHandGrabRadius of the line running forward from the right hand, 5 cm to TwoHandReach along it. Meters, ini-only.
float GetTwoHandGrabRadius();
float GetTwoHandReach();

// HUD placement (hooks/draw_trace_diag.cpp, true-frustum mode only): the game's full-screen HUD is drawn into HudScale
// of each eye's real field of view (so it keeps to the corners), as one head-locked rectangle whose two eye copies
// are separated by HudIpdScale times the rendering IPD (see GetHudIpdScale). HudPlacementEnabled / HudScale.
bool GetHudPlacementEnabled();
float GetHudScale();
// Size of the head-locked screen a whole-2D frame (pre-rendered movie, menu, loading screen) is drawn into, as a
// fraction of the eye's view; same depth/IPD/offset rules as the HUD. MenuScreenScale, default 0.75.
float GetMenuScreenScale();
// Markers test (hooks/draw_trace_diag.cpp): from this draw number of the final 2D pass on, the full frame is restored
// instead of the HUD rectangle, to see whether the in-world objective markers are already placed per eye by the game.
// 0 = off. FullFrameFromDraw.
int GetFullFrameFromDraw();
// Hides the game's screen-centre reticle (draws 2..7 of the frame's final backbuffer pass, gameplay only). Menus draw
// into the same pass, so while it's on an in-game (pause) menu can lose the elements at those draw positions.
// HideReticle, default on.
bool GetHideReticle();

// First-person body batch parts (instance index 0..) hidden while the gun follows the controller, ini HideBodyPart1..
// The part count isn't known ahead (only 3 seen so far, and levels differ): the menu shows one row per part of the body
// batch now drawn (SetBodyPartCountSeen, from hooks/camera_matrix_test_hook.cpp), plus any hidden part beyond that.
// kMaxBodyParts is storage only; a batch with more parts is logged.
constexpr int kMaxBodyParts = 32;
bool GetHideBodyPart(int index);
void SetBodyPartCountSeen(int count);
// Moves the HUD rectangle off head-forward: degrees, + = right / up (HudOffsetX / HudOffsetY).
void GetHudOffsetDeg(float* x, float* y);
// The HUD's eye separation as a multiple of the rendering IPD (the eye offset the world is drawn with, IpdScale
// included): 1 = the world's own IPD, 0 = at infinity. HudIpdScale, default 1.
float GetHudIpdScale();
// Distance the HUD is fused at, meters (HudDepth, default and minimum 0.1). Separation = eye offset * HudIpdScale /
// HudDepth; HudIpdScale 0 puts the HUD at infinity.
float GetHudDepth();

// Whether the controller shot drops the game's random weapon spread (sdk/motion_controller_aim.h's ShotSpreadRemover).
// Ini RemoveShotSpread, default on.
bool GetRemoveShotSpread();

// Fire / ADS go out as mouse clicks (left / right, hooks/controller_trigger_hook.cpp) instead of the virtual pad's RT /
// LT (openvr_direct/vr_input.cpp). FireViaMouse, default off: the pad triggers also drive vehicles.
bool GetFireViaMouse();

// Hands (openvr_direct/vr_hands.cpp): LeftHanded = the default weapon hand; HolsterHoldToKeep = a weapon grabbed from
// a holster only stays in that hand while its grip is held (default off: it stays until the next grab).
bool GetLeftHanded();
bool GetHolsterHoldToKeep();

// The gun drawn on the weapon-hand controller (hooks/camera_matrix_test_hook.cpp's weapon drive; arms and hands are
// hidden while it's on). WeaponDriveEnabled, default on, so it applies as soon as a character loads; Numpad . toggles
// it and the setter saves.
bool GetWeaponDriveEnabled();
void SetWeaponDriveEnabled(bool value);
// Holster zones: radius (meters) and a vertical offset added to every zone (meters, + = lower).
float GetHolsterRadius();
float GetHolsterDrop();
// Holster zone -> game action: a GstKeyBinding.infantry concept name from Documents\MOHW\settings\PROF_SAVE_profile
// (e.g. ConceptSelectInventoryItem2), empty = unassigned. Ini keys HolsterStomach / HolsterHip / HolsterChest /
// HolsterLeftShoulder / HolsterRightShoulder.
constexpr int kHolsterStomach = 0;
constexpr int kHolsterHip = 1;
constexpr int kHolsterChest = 2;
constexpr int kHolsterLeftShoulder = 3;
constexpr int kHolsterRightShoulder = 4;
constexpr int kHolsterZoneCount = 5;
std::string GetHolsterAction(int zone);
// Zone centre as an offset from the head in the body frame, meters: out = {right, up, forward}. Stomach and hip are
// mirrored pairs: out[0] is the distance to each side. Ini HolsterStomachPos=x,y,z etc.
void GetHolsterOffset(int zone, float out[3]);

// Sight dot (openvr_direct/vr_overlay.cpp): a dot on the weapon hand's aim ray at SightZeroDistance meters, so targets
// at that distance line up with where the shot lands. SightDotSize in degrees.
bool GetSightDotEnabled();
// Both sight dots only while the weapon grip is on: the weapon hand's grip held (HolsterHoldToKeep on), or toggled on
// by a weapon-hand grip press (off). See openvr_direct/vr_hands.h HandsResult::weaponGripped.
bool GetDotsOnlyWithGrip();
// A grip double-tap holds that hand's bumper (left LB, right RB) for the second press (openvr_direct/vr_input.cpp).
// GripDoubleTapBumpers, default on.
bool GetGripDoubleTapBumpers();
float GetSightZeroDistance();
float GetSightDotSizeDeg();

// Optic dot (openvr_direct/vr_overlay.cpp): fixed to the weapon hand like a sight on the gun -- at the hand's position,
// raised by the zero (height, + up), moved by windage (+ right) and forward by OpticDistance, meters, all in the hand's
// own frame. Zeroed by eye against where shots land. Ini OpticHeight / OpticWindageOffset / OpticDistance.
bool GetOpticDotEnabled();
void GetOpticOffset(float* height, float* windage);
float GetOpticDistance();
// Dot colours: green when on, red when off (OpticDotGreen default off, RayDotGreen default on).
bool GetOpticDotGreen();
bool GetRayDotGreen();
// Aim ray dot correction, degrees from the controller's forward (+ up / + right): the shot-spread remover
// (sdk/motion_controller_aim.h) isn't always exact. RayDotElevation / RayDotWindage.
void GetRayDotOffsetDeg(float* elevation, float* windage);

// Menu table for the SteamVR dashboard tab (openvr_direct/vr_overlay.cpp): every setting a player can adjust, in
// display order, grouped. AdjustMenuSetting flips a toggle (any nonzero steps) or moves a number by steps * step
// (clamped to its range) and saves the ini. Safe from any thread.
struct MenuSettingInfo
{
    const char* group;
    const char* label;
    bool isToggle;
    bool isChoice; // a list setting (holster actions): text is the current choice, steps +/-1 cycle it
    float value;   // toggles: 0 or 1
    float step;
    int decimals;
    const char* unit;
    char text[64];
    char labelBuffer[64]; // backs label for rows built at runtime (body parts)
};
int GetMenuSettingCount();
bool GetMenuSettingInfo(int index, MenuSettingInfo* out);
void AdjustMenuSetting(int index, int steps);
// The game actions a holster can be cycled through (keyboard-bound actions from the game's profile).
void SetHolsterActionChoices(const std::vector<std::string>& actions);

// Where the right controller sits on the gun for the weapon drive (hooks/camera_matrix_test_hook.cpp), in the gun's
// rest view: out = {right, up, back} in meters (forward is negative back). Ini-only (WeaponGripRight/Up/Back).
void GetWeaponGripOffset(float out[3]);
// The gun's rotation relative to the controller: out = {pitch, yaw, roll} in degrees. Ini-only (WeaponGripPitch/Yaw/Roll).
void GetWeaponGripRotationDeg(float out[3]);

} // namespace mohw
