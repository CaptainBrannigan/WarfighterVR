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

// Only the log files switch (WriteLogFiles, sdk/logging.h), read before anything is logged so a session with the logs on
// has its first lines too (the startup report, the build check). LoadSettings reads it again with everything else.
void LoadLogSwitch();

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
// Global hand calibration (2026-10-16): each hand's pose is moved by position {right, up, forward} (m) and turned by
// rotation {pitch + up, yaw + right, roll + right} (deg), in the controller's own frame, before anything uses it --
// the gun, the laser and shots, the dots, holsters, two-handed aim (openvr_direct/vr_input.cpp). Mirrored for the left
// hand. HandOffsetRight/Up/Forward, HandOffsetPitch/Yaw/Roll, default 0.
void GetHandOffset(float position[3], float rotationDeg[3]);
// The arms on the controllers (hooks/first_person_rig.cpp), default on. The left hand's own calibration: position
// {right, up, forward} (m) and rotation {pitch, yaw, roll} (deg) on its controller. ArmIkEnabled,
// LeftHandIkRight/Up/Forward, LeftHandIkPitch/Yaw/Roll.
bool GetArmIkEnabled();
// HELD POSE (2026-11-21, the user: "cancel all weapon idle animations while player has control"): the hands and the gun
// held in the pose each weapon settles in after it comes in, instead of following the game's animation under the
// controllers (hooks/first_person_rig.cpp). HoldHandPose, default on.
bool GetHoldHandPose();
void GetLeftHandIkOffset(float position[3], float rotationDeg[3]);
// The rest of the arm IK (hooks/first_person_rig.cpp). rightHand: the weapon hand's calibration on its own controller,
// as the left hand's (position {right, up, forward} m, rotation {pitch, yaw, roll} deg; RightHandIk*; until 2026-11-09
// an offset on the gun the hand rode); shoulder: each shoulder ([0] left, [1] right) moved {outward, up,
// forward}, as the game's sit a long way from a hand on the controller, and not evenly: the right sits further back.
// naturalElbow = AvP's elbow model, its angle limits below; off, the
// plain IK with elbowSwivelDeg. Every field is a row of the menu's "Arm IK" group.
struct ArmIkSettings
{
    float rightHandPos[3], rightHandDeg[3];
    float shoulder[2][3];
    bool naturalElbow;
    float elbowSwivelDeg;
    float follow, inKneeDeg, inMaxDeg, inNearBody, outKneeDeg, outMaxDeg, smoothingMs;
    float forearmTwist;
};
ArmIkSettings GetArmIkSettings();
// The whole first-person body (hooks/camera_matrix_test_hook.cpp MoveBody): moved {right, up, forward} along the level
// body frame, metres, and BodyUpright keeps it from tilting with the view. BodyOffsetRight/Up/Forward, BodyUpright.
void GetBodyOffset(float out[3]);
bool GetBodyUpright();
// The rig corrected by the skeleton's Head bone, for bones and a camera from different game ticks
// (hooks/first_person_rig.cpp AlignRigToBones). StabilizeRig, default on.
bool GetStabilizeRig();
// Draws every grab zone in the headset, for setting them up (openvr_direct/vr_hands.cpp). ShowGrabZones, default off.
bool GetShowGrabZones();
// Manual reload (openvr_direct/vr_reload.h), default on, and the pouch: head-relative like the holsters, with its grab
// radius. ManualReloadEnabled, MagazinePouchX/Y/Z, MagazinePouchRadius.
bool GetManualReloadEnabled();
void GetMagazinePouchOffset(float out[3]);
float GetMagazinePouchRadius();
// The magazine waiting in the pouch, turned {pitch, yaw, roll} degrees from the body's right / up / back (2026-11-18).
// MagazinePouchPitch / Yaw / Roll.
void GetMagazinePouchRotation(float deg[3]);
// A magazine taken from the pouch (2026-11-21, openvr_direct/vr_magazine_path.h PouchGrabInHand): kept where it sat
// relative to the hand; snapped into the hand at its platform's hold (MagazinePlatform holdPos / holdDeg) but slid along
// its own vertical to where the hand gripped it, the hold's up an offset from there; or snapped fully.
// MagazinePouchGrab, default snapped with the grip's vertical. A magazine pulled out of the gun always keeps its grab.
constexpr int kPouchGrabAsGrabbed = 0, kPouchGrabSnapKeepVertical = 1, kPouchGrabSnap = 2;
int GetMagazinePouchGrab();
// The equipped weapon's magazine (per weapon, SetWeaponProfile), metres from its seat in the gun, in the gun's frame
// {right, up, forward}. grabOffset / grabHalfSize: the box the off hand grabs it in. aimPoint: what the magazine's top
// points at. range: how far out it starts turning, alpha 0; lockRange: alpha 1, its horizontal locks and only vertical
// movement is left; seatRange: how close to the seat, along that line, counts as in.
struct MagazineProfile
{
    float grabOffset[3], grabHalfSize[3];
    float aimPoint[3];
    float range, lockRange, seatRange;
};
MagazineProfile GetMagazineProfile();
// MAGAZINE PLATFORMS (2026-11-14, "individual offsets dependent on the platform, shared if possible (all aks, all ars,
// etc)"): how a magazine sits in the hand and how it leaves and enters the gun is shared by every gun of a platform.
// A gun's platform comes from its name (MagazinePlatformChoice -1 = auto, per weapon) or is set in its weapon profile;
// the menu's "Magazine platform" rows edit the equipped gun's platform. Stored as MagazinePlatform0..7 lines.
constexpr int kMagazinePlatformCount = 8;
struct MagazinePlatform
{
    int index;
    const char* name;
    // A magazine snapped into the hand from the pouch (GetMagazinePouchGrab): its top {right, up, forward} metres from
    // the controller and its rotation {pitch, yaw, roll} degrees, for the left hand (mirrored in the right). (Unread
    // 2026-11-18 .. 21, when every grab kept its own offset.)
    float holdPos[3], holdDeg[3];
    // Coming out, it slides along the line from its seat to the weapon profile's aim point, travel metres to clear the
    // magwell. (2026-11-18: the AK's hinged rock-out is gone -- the user: "do away with the AK hinge mag feature". Its
    // values stay in the MagazinePlatform lines, unread, so older ini files still load.)
    float travel;
    // Coming out: the hand stays on the magazine, and the controller has to get this far past it (metres along the
    // path) before the magazine moves; it then follows that far behind. Going in takes no force (2026-11-18).
    float pullForce;
    // The magazine release (SteamVR action "Magazine release", only while a magazine is gripped) has to be pressed
    // before the magazine can leave the gun.
    bool needsRelease;
};
MagazinePlatform GetMagazinePlatform();
const char* GetMagazinePlatformName(int index);
// How far the controller can get from a hand stuck on a magazine before the grip lets go, metres. MagazineGripBreak.
float GetMagazineGripBreak();
// SCOPED EYE (2026-11-14, prototype, hooks/scope_view.h): in engine two-view, an eye brought behind the gun's sight
// line (the weapon profile's optic zero: height / windage above the hand) sees through a magnified scope instead of its
// own view. enabled: ScopeEyeEnabled (default on). eyeBox: how close to the sight line the eye has to be, m; relief: how
// far behind the hand along it, m; apparentFovDeg: the eyepiece's size in view; reticle (ScopeReticle): a crosshair on
// the impact point in the scoped eye and the lens picture, holographic (where each eye sees the laser's point; the game's
// own zoom-optic reticle isn't used). lensPicture (ScopeLensPicture, default on): the screen-space prototype -- each eye not
// looking through the scope sees a magnified picture in the scope's lens, filled from the other eye's view.
// Per weapon (the profile, 2026-11-17): magnification (ScopeMagnification, 0 = the gun has no scope; it no longer falls
// back to the game's ADS zoom, and needs no ADS); the lens disc: lensRadius (ScopeLensRadius, m), its centre lensRight /
// lensUp from the optic zero (ScopeLensRight / ScopeLensUp, m, in the hand's frame) and lensForward ahead of the hand
// (ScopeLensForward, m).
struct ScopeSettings
{
    bool enabled;
    float eyeBox, relief, apparentFovDeg;
    bool reticle;
    float magnification;
    bool lensPicture;
    float lensRight, lensUp, lensForward, lensRadius;
    // Per weapon (ScopeNeedsMagnifier, 2026-11-17): the scope only while the game's flip magnifier is switched on
    // (hooks/weapon_identity.h GetEquippedOpticState), for red dots with a magnifier behind them.
    bool needsMagnifier;
};
ScopeSettings GetScopeSettings();
// Secondary grip offset (per weapon, SetWeaponProfile): where the off hand holds relative to the barrel line, metres
// in the gun's frame (+ right, + up, + forward). The two-handed aim targets the off hand's point moved by it, so a hand
// below the bore (a vertical foregrip) doesn't tilt the gun; forward (2026-11-21) moves that point along the barrel,
// which sets how far the right / up parts tilt the aim. SecondaryGripRight / Up / Forward, default 0.
void GetSecondaryGripOffset(float* right, float* up, float* forward);

// HUD placement (hooks/draw_trace_diag.cpp, true-frustum mode only): the game's full-screen HUD is drawn into HudScale
// of each eye's real field of view (so it keeps to the corners), as one head-locked rectangle whose two eye copies
// are separated by HudIpdScale times the rendering IPD (see GetHudIpdScale). HudPlacementEnabled / HudScale.
bool GetHudPlacementEnabled();
float GetHudScale();
// Size of the head-locked screen a whole-2D frame (pre-rendered movie, menu, loading screen) is drawn into, as a
// fraction of the eye's view; the HUD's offsets. MenuScreenScale, default 0.75.
float GetMenuScreenScale();
// The game's own UI laid out as on a screen this many times smaller (2026-11-12, hooks/ui_pass.cpp): its menus are laid
// out in pixels, so they shrink as the resolution grows. Automatic since 2026-11-22: worked out from the width of the
// rectangle the game lays its UI out in (screenWidth), 1.5 at 5824, so the menus look the same size at any resolution;
// below 1 on smaller screens. UiLayoutSize (the dashboard's Hud Element Scale, default 1 = automatic) multiplies it.
// Only with HudPlacementEnabled.
float GetUiLayoutScale(int screenWidth);
// Where that screen fuses, meters (MenuScreenDepth, default 2.5): its two eye copies are separated by the rendering eye
// offset over this distance, as a real screen there would be (not the HUD's depth and IPD scale).
float GetMenuScreenDepth();

// Driving missions (game\MOHWD.exe, hooks/driving_camera_hook.cpp): which camera through UpdateMatrices gets head
// look by hand -- 0 = auto (GetDrivingHeadLookAuto), 1.. = by call rate, busiest first (logged to mohwvr_drivingcamera.log). Session only, not saved.
// DrivingHeadLookInvert flips the head rotation, in case the axis mapping is backwards.
int GetDrivingCameraIndex();
// End of the manual range (views ranked GetDrivingCameraIndex()..this get head look and stereo); 0 = only the "from"
// view. Session only, not saved.
int GetDrivingCameraIndexTo();
bool GetDrivingHeadLookInvert();
// Head look at the camera source (MOHWD.exe CommitViewTransform, hooks/driving_camera_hook.cpp CAMERA SOURCE), when no
// manual range is set. DrivingHeadLookAuto, default on.
bool GetDrivingHeadLookAuto();
// Head yaw as right-stick look in the driving missions (hooks/driving_camera_hook.h GetDrivingStickLook): the head yaw
// at full deflection, degrees (0 = yaw by render only, as before), and the game's stick deadzone to skip over.
// DrivingStickLookRange (90), DrivingStickDeadzone (0.25).
float GetDrivingStickLookRangeDeg();
float GetDrivingStickDeadzone();
// Stereo in the driving missions (hooks/driving_camera_hook.cpp): the view gets the per-eye shift and true
// frustum and the frame is shown full-view; off = the whole-frame floating screen. DrivingStereo, default on.
bool GetDrivingStereo();
// SPECIALIZED SEQUENCES (2026-11-22): "Use 6DoF Motion For Specialized Sequences", over every sequence's profile.
// SpecializedSequenceMotion, default on.
bool GetSpecializedSequenceMotion();
// The Zodiac Sequence's profile (the Basilan RHIB, hooks/boat_seat.h; keys SequenceZodiac*), each toggle on by default.
// GetBoatSixDof: the master and the profile's 6DoF both on: its wheel and Mk19 seats get the head in 6DoF with the
// hands in the game's pose. Off, they are as for any other scene (only the level keeps its stereo), and the two below
// with it. GunAim: the gripping hands lever the Mk19 about its pivot. Wheel: the driving missions' wheel code steers it.
bool GetBoatSixDof();
bool GetBoatGunAim();
bool GetBoatWheel();
// GunAimStrength (default 1): how hard the turret is driven to the hands. StickDeadzone (default 0.2): what the game
// ignores of the stick, added back for both the gun and the wheel.
float GetBoatGunAimStrength();
float GetBoatStickDeadzone();
// The profile's own wheel, as the driving missions' three below; an ini without it starts from that ini's.
float GetBoatWheelLockDeg();
void GetBoatWheelPlacement(float* forward, float* down, float* right, float* tiltDeg);
void GetBoatWheelLook(float* radius, float* thickness, float* opacity);
// Driving stereo: the committed view gets the next frame's eye, frustum and shift (swaps the eyes). Live 2026-10-14 the
// commit lands in the frame it is presented in, so leave this off. Ini key DrivingGameThreadNextEye, default off.
bool GetDrivingLateNextEye();
// Steering wheel in the driving missions (openvr_direct/vr_steering.cpp): while a grip is held, the hand turns a virtual
// wheel fixed in the room; its turn, over DrivingWheelLock degrees for full lock, goes out as left-stick X.
// DrivingSteeringWheel, default on.
bool GetDrivingSteeringWheel();
float GetDrivingWheelLockDeg();
// The wheel's centre from the head as it was at the last recenter (forward / below / right, metres, along that heading),
// and its tilt: the top leaning away from the driver, degrees. DrivingWheelForward / Down / Right / Tilt.
void GetDrivingWheelPlacement(float* forward, float* down, float* right, float* tiltDeg);
// The drawn wheel (openvr_direct/vr_overlay.cpp UpdateWheelOverlay): rim radius and thickness, metres, and opacity
// (0 = not drawn). Looks only -- steering goes by the hand's angle around the centre. DrivingWheelRadius / Thickness /
// Opacity.
void GetDrivingWheelLook(float* radius, float* thickness, float* opacity);
// Live debugging: this draw number of the final 2D pass is not drawn, to see which element it is. 0 = off.
// DebugHideDraw.
int GetDebugHideDraw();
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
// In VR the game never takes the display in exclusive fullscreen (hooks/present_hook.cpp). KeepGameWindowed, default on.
bool GetKeepGameWindowed();
// EXPERIMENTAL (2026-11-06): both eyes rendered by the engine in one frame, through its own two-view split-screen path
// (hooks/native_stereo.h), instead of one eye per frame. NativeStereo, default off. Only in 3D frames: movies, menus,
// loading screens and cutscenes before the player has control keep the game's one view (2026-11-08).
bool GetNativeStereoEnabled();
// The sky drawn with exact per-pixel view rays (hooks/sky_rays_fix.h): the game's own are warped for a wide, off-centre
// eye, differently in each eye. SkyExactRays, default on.
bool GetSkyExactRaysEnabled();
// Two-view (2026-11-09, "the dust effect was still only in the right eye"): both views draw the screen effects (the
// dust), but the left view's was gone from the frame, so it's drawn right after the right view's instead
// (hooks/screen_effect_diag.cpp). ScreenEffectsBothEyes, default on.
bool GetScreenEffectsBothEyes();
// Two-view (2026-11-11): the game's depth of field downsamples the whole side-by-side frame, so each eye blended both
// eyes' images in (cutscenes with the camera on the player's body). Off in two-view unless this is on
// (hooks/native_stereo.cpp: ForceDofEnable 0, DiffusionDofEnable and ADSDoFEnable false). DepthOfFieldTwoView, default off.
bool GetDepthOfFieldTwoView();
// EXPERIMENTAL (2026-11-12): the engine's 3D render scale (GameRenderSettings.ResolutionScale) follows SteamVR's
// recommended eye size, its resolution setting included, over the eye's height in the backbuffer
// (hooks/native_stereo.cpp). RenderScaleFromSteamVr, default off.
bool GetRenderScaleFromSteamVr();
// EXPERIMENTAL (2026-11-12): in two-view, the backbuffer itself is SteamVR's recommended eye size, two side by side
// (the game's screen size, its resize path; hooks/native_stereo.cpp), so each eye gets that many pixels instead of half
// the game's resolution. Only with KeepGameWindowed and the swap chain windowed. EyeSizedBackbuffer, default off.
bool GetEyeSizedBackbuffer();
// Scripted scenes (2026-11-12): while the game has taken the player's look or fire away (its input locks,
// hooks/input_restriction.h), the gun and arms are the game's and head look goes onto the game's camera.
// ScriptedScenesFromLocks, default on.
bool GetScriptedScenesFromLocks();
// The weapon socket view (2026-11-09): every weapon bone as a line from its parent's joint to its own, named, in the
// headset (hooks/camera_matrix_test_hook.cpp, openvr_direct/vr_overlay.h). ShowWeaponSockets, default off.
bool GetShowWeaponSockets();
// The gun is held at its hand socket (hooks/camera_matrix_test_hook.cpp GripInCamera): the weapon's own
// IK_Joint_RightHand where the arm IK's weapon hand holds it (the hand on its controller, RightHandIk*), plus
// GetGunSocketOffset; the profile's grip rotation turns it about the socket. (2026-11-09: no longer a setting, and the
// profiles' grip positions are gone.)
// The gun's fine offset in the hand, m: right, up, forward, in the controller's frame (mirrored for the left hand).
// GunInHandOffsetRight / Up / Forward, default 0: the gun where the game's own hand holds it.
void GetGunSocketOffset(float out[3]);
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
// A double tap of the thumbrest = that hand's bumper (openvr_direct/vr_input.cpp UpdateThumbTap), and with it on, Touch
// grips no longer double-tap. ThumbTapBumpers, default on.
bool GetThumbTapBumpers();
// The game reads its gamepad -- so the motion controllers -- without its window having keyboard focus: the game's own
// ClientSettings.UseGlobalGamePadInput held on (hooks/xinput_hook.cpp GamepadFocusOnPresent). Both executables.
// GamepadWithoutFocus, default off (2026-11-22, the user: "default to off for now").
bool GetGamepadWithoutFocus();

// Controller aim offset (sdk/motion_controller_aim.h ApplyControllerAimOffset), degrees in the controller's own frame:
// + up, + right. Turns the shots and the aim-ray dot, not the gun (and so not the optic dot). ControllerAimUp /
// ControllerAimRight, default 0: the aim comes straight from the controller's rotation, this is for an edge case.
void GetControllerAimOffsetDeg(float* up, float* right);

// How much of the weapons' sway reaches the view and the bullet while aiming down sights (hooks/weapon_sway_patch.cpp):
// 0 = none, 1 = the game's own. AdsSwayScale, default 0.
float GetAdsSwayScale();
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
// The laser (hooks/laser_raycast.h): the aim-ray dot sits where the shot line first hits (or at SightZeroDistance, the
// max range, when nothing is hit), and a beam in the dot's colour runs from the hand to it. LaserBeam (on),
// LaserBeamOpacity (0.35), LaserBeamWidth (mm, 4), LaserRaycastEveryFrames (the game raycast every N rendered frames, 1).
bool GetLaserBeamEnabled();
float GetLaserBeamOpacity();
float GetLaserBeamWidthMeters();
int GetLaserRaycastEveryFrames();
// Where the laser begins, metres forward of the hand along the shot line (per weapon: set it at the muzzle so the
// laser can't hit the gun). The raycast starts there; the drawn beam starts that far forward too. LaserStart, default
// 0.15.
float GetLaserStartMeters();
// The drawn beam's start, beside the shot line (per weapon, purely visual -- a laser module mounted off the bore): metres
// in the gun's frame, + right / + up, at LaserStart forward. The beam runs from there to the dot, which stays on the
// shot line. LaserBeamStartRight / LaserBeamStartUp, default 0.
void GetLaserBeamStartOffset(float* right, float* up);

// Menu table for the SteamVR dashboard tab (openvr_direct/vr_overlay.cpp): every setting a player can adjust, in
// display order, grouped. AdjustMenuSetting flips a toggle (any nonzero steps) or moves a number by steps * step
// (clamped to its range) and saves the ini. Safe from any thread.
struct MenuSettingInfo
{
    const char* group;
    const char* label;
    bool isToggle;
    bool isChoice; // a list setting (holster actions, the render mode): text is the current choice, steps +/-1 cycle it
    bool isNote;   // a line of text under the setting before it, nothing to adjust (the render mode's reload note)
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
// RENDER MODE (2026-11-12): one DLL, two renderers -- alternating eye, or the engine's two-view (NativeStereo). The
// dashboard shows it as a choice, shows only the selected mode's rows, and keeps the HUD's numbers per mode (HudScale,
// HudDepth, HudIpdScale, HudOffsetX/Y, MenuScreenScale/Depth, UiLayoutSize; the engine mode's under "...TwoView" keys;
// the driving missions always use the alternating-eye ones). Any thread: the render mode the current level was built
// with (hooks/native_stereo.cpp, at the level's load), for the note under the setting while the selection differs.
void NoteLevelRenderMode(bool twoView);
// The game actions a holster can be cycled through (keyboard-bound actions from the game's profile).
void SetHolsterActionChoices(const std::vector<std::string>& actions);

// The gun's rotation relative to the controller, about its hand socket: out = {pitch, yaw, roll} in degrees. Per weapon
// (SetWeaponProfile).
void GetWeaponGripRotationDeg(float out[3]);

// WEAPON PROFILES (2026-10-15): the grip rotation, the optic dot's zero (height / windage / forward), the controller
// aim offset, the secondary grip offset, the laser's start and the magazine's grab settings are per weapon (the grip
// position was too, until 2026-11-09). Called with the equipped weapon's blueprint name (hooks/weapon_identity.h;
// display = its short name for the menu), or nullptr when none is identified: the settings switch to that weapon's
// values -- a weapon without its own yet gets the default profile (the ini's keys for these settings) -- and the
// menu's rows for them, titled "Weapon profile: <weapon>", edit that weapon's. Stored as WeaponProfile2 lines (older
// WeaponProfile lines are read and converted).
void SetWeaponProfile(const char* key, const char* display);

} // namespace mohw
