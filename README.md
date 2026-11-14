# MOHWVR

An unofficial VR mod for **Medal of Honor: Warfighter** (2012, Frostbite 2 / DirectX 11). The game has no VR support
of its own. This mod is a proxy `dxgi.dll` that hooks the game's rendering and input and talks to SteamVR directly
from inside the game: stereo rendering at the headset's real field of view, head-tracked look and movement, motion
controllers for aiming, a gun held in your hand, arms that follow your hands, manual magazine reloads, body holsters,
and an in-headset settings menu.

**Status: v1.3, a hobby reverse-engineering project.** It's playable, but there are rough edges and the campaign
hasn't been played all the way through yet; see [Known issues](#known-issues).

## Requirements

- Medal of Honor: Warfighter (the Steam/Origin PC release, 32-bit `MOHW.exe`).
- SteamVR **2.17 or newer** (the release that added a 32-bit runtime), with a headset and motion controllers.
  Developed and tested on a Meta Quest 3 over Steam Link with Touch controllers; other headsets should work. Default
  bindings are included for Touch, Valve Index (including Touch that Virtual Desktop presents as Index), Pico, Vive
  Focus 3, HP Reverb G2, Vive Cosmos, Windows Mixed Reality, Vive wands and Steam Frame. Any other controller type has
  no input until you bind it yourself in SteamVR.
- `openvr_api.dll` (32-bit) from your SteamVR install.

## Installing

Copy into the game's install folder, next to `MOHW.exe`:

- `dxgi.dll` from [Releases](../../releases) (or build it yourself, see [DEVELOPMENT.md](DEVELOPMENT.md)).
- `openvr_api.dll`, copied from `<Steam>\steamapps\common\SteamVR\bin\win32\openvr_api.dll`.

Also copy `dxgi.dll` into the `game` subfolder (next to `game\MOHWD.exe`). The game runs its driving missions in that
separate executable; with the copy there, those missions get stereo and head look too (see
[Driving missions](#driving-missions)). Settings and logs stay in the main folder.

Start SteamVR, then launch the game normally. The mod creates `mohwvr_settings.ini` and its SteamVR action and
binding files next to the DLL on first launch. The settings start from the author's own tuned values (including the
weapon profiles made so far, see [Weapon profiles](#motion-controllers)); an existing `mohwvr_settings.ini` is kept, and
only gains the built-in weapon profiles for weapons it has none for. Settings are changed from inside the headset
(see [Settings menu](#settings-menu)).

**No headset, no mod:** the mod only switches on once it has connected to a headset through SteamVR. Without one the
game runs completely unmodified (nothing is hooked), so you can leave the DLL installed and play flat. The mod never
starts SteamVR itself. It keeps checking every few seconds: start SteamVR and connect a headset while the game is
running and the mod switches on then, and stays on until you quit.

**Controller bindings:** in SteamVR's controller bindings for the game, pick **"MOHW VR defaults"**. If you
previously saved your own binding for the game, SteamVR keeps using that copy and won't pick up new actions from a
newer version of the mod until you switch back to the defaults (or add the new actions to your copy). The ones a saved
binding may be missing: "Left / Right hand position for two-handed grip", bound to the Grip pose (without it two-handed
aim uses the controller's tip instead of your palm), and "Thumb tap - left / right hand" for LB / RB.

## Features

### Rendering
- Stereo rendering at each eye's **real, asymmetric field of view**, taken from the headset, with correct IPD. Eyes
  are rendered on alternating frames and submitted to SteamVR as matched pairs, with the pose each frame was rendered
  for, so SteamVR can reproject cleanly.
- Rotation smoothing for the game's 30 Hz simulation tick, head roll, and positional head tracking.
- First-person weapon and body are rendered with the same per-eye projection and parallax as the world, and the body
  keeps level when you tilt your head.
- **HUD:** moved into a head-locked panel that fuses in both eyes, sized to the corners of your view, with adjustable
  scale, depth, IPD and offset. The screen-centre reticle is hidden by default (you aim with the controller).
- **In-world markers** (nametags, objective, grenade and vehicle icons) stay on their targets in both eyes while the
  HUD is in its panel (in the driving missions too); ones outside the panel are pinned to its edge, like the game's
  own off-screen arrows.
- Pre-rendered movies, menus and loading screens are shown as a floating screen (see [Known issues](#known-issues)).
- **Two render modes in one DLL** (View > "Render mode", the first row):
  - **Alternating eye** (the original): one eye per frame.
  - **Engine two-view:** in play, the game renders both eyes in one frame through its own two-view path. Movies,
    menus, loading screens and cutscenes before you have control stay on the floating screen. The first-person weapon
    and the HUD are drawn per eye, the in-world markers in the right direction at the HUD's depth, and the desktop
    window shows both eyes side by side. The ocean water's per-view jobs are kept in order, which fixed the crashes
    in levels with open water.

  A change applies fully at the next level load: while the level in play was loaded in the other mode, a note under
  the setting says so ("Changed: reload the level to apply"). The menu shows only the selected mode's settings. The
  HUD's numbers (menu/movie screen size and distance, Hud Element Scale, HUD scale, depth, IPD and offsets) are kept
  per mode, so each mode keeps its own tuning; the driving missions always use the alternating-eye values. Details:
  [docs/native_stereo_render_path.md](docs/native_stereo_render_path.md).
- **The sky is drawn with exact view rays** (View > "Exact sky rays", on by default). The game's own rays warp the
  clouds differently in each eye with a headset's wide, off-centre view.
- **Screen effects in both eyes** (View > "Screen effects (dust) in both eyes", on by default, engine two-view
  only): the left eye's dust and other full-screen effects are drawn after the right eye's, since the right eye's
  render was wiping them.
- **No depth of field in two-view mode** (View > "Depth of field (doubles the image)", off by default, engine
  two-view only): the game's depth of field blurs the whole side-by-side frame, so each eye got a faint copy of both
  eyes' images (seen in cutscenes once the camera is on the player). Turned off while two views render, including the
  ADS blur.
- **Experimental, off by default: backbuffer at SteamVR's eye size** (View > "Backbuffer at SteamVR eye size", engine
  two-view only). In two-view each eye is only half the game's resolution; this asks the game for SteamVR's recommended
  eye size, two side by side (e.g. 5824x3112), through its own resize path, so the world and the HUD reach the headset
  at SteamVR's resolution. Needs "Keep game out of exclusive fullscreen". Switched on mid-level, it shows fully after
  a level reload. Details: [docs/native_stereo_render_path.md](docs/native_stereo_render_path.md).
- **HUD element scale** (HUD > "Hud Element Scale", 1 = the game's own): the game lays its menus out in pixels, so at
  a high resolution they come out small. Above 1 lays the UI out as on a screen that much smaller, so the menus are
  bigger again and still drawn at the full resolution. Applies live. The HUD section sits under View, in the first
  column of the settings.
- The floating screen's distance is its own setting (HUD > "Menu / movie screen distance", 2.5 m by default).

### Scripted scenes
- **The game's own input locks mark scripted scenes** (View > "Scripted scenes follow the game's input locks", on by
  default). Cutscenes, breaches and other scripted moments take inputs away through the game's input restrictions;
  the mod reads them and steps aside, leaving only the stereo rendering:
  - look taken: the head stops driving anything (no head look, head position or head roll), the mod no longer undoes
    the game's own turns of the player, and the game's camera is shown as it is, in 3D, fixed to your view (like a
    screen strapped to the headset). The same as when the aim stops altogether (the robot section, menus);
  - look or fire taken: the controllers stop driving anything too. The gun and arms go back to the game's animation,
    the shots, laser and sight dots go back to the game's aim, and the reticle returns;
  - in either case the body is where the game puts it: no Body offset or "Keep body upright" turn and no "Keep body
    and gun steady" easing.

  Every change is logged in `mohwvr_control.log` ("INPUT LOCKS"), with the inputs taken. Details:
  [docs/camera_state_and_resets.md](docs/camera_state_and_resets.md).

### Head look
- Head yaw is **added** to the game's own turning (right stick or mouse), and head pitch is absolute; the game's own
  pitch input is removed so the view never fights your head.
- Recenter from the settings menu (or F3); the headset's own recenter is also picked up.

### Motion controllers
- **Aim with the controller:** shots leave from your hand along the controller's direction, independent of where you
  look. The weapon's random spread is removed.
- **The gun follows your hand** (on by default, Numpad . or "Gun + shots follow controller" toggles), with adjustable
  grip offset and rotation. Turning it off also hands the shots back to the game's own aim (see
  [Known issues](#known-issues)).
- **Hand calibration** (all weapons): "Hand offset right / up / forward" and "Hand rotation pitch / yaw / roll" in
  the Hands settings move and turn the hand itself before anything uses it -- the gun, the laser and shots, the dots,
  holsters and two-handed aim -- mirrored for the left hand. Per-weapon settings come on top.
- **The gun sits at its own hand socket:** each gun is placed in the arm IK's weapon hand by the point the game's own
  animation holds the right hand on (its IK_Joint_RightHand), so no per-gun grip position is needed (the profiles'
  grip positions were removed, 2026-11-09). The hand sits on its controller ("Right hand ..." in Arm IK), and "Gun
  offset in the hand right / up / forward" fine-tunes the gun in it for all guns. The profiles' grip rotation still
  applies. Details: [docs/weapons_sockets_magazines.md](docs/weapons_sockets_magazines.md).
- **Weapon socket view** (Hands > "Show weapon sockets (debug lines)", off by default): every bone of the gun's
  skeleton drawn in the headset as a line from its parent to itself, with its name, plus each hand's weapon bone. For
  checking where the gun's grip points are against the hands.
- **Two-handed aiming:** grip with your other hand on the barrel to aim along the line between your hands. Only the
  other hand's position counts (its palm, the SteamVR grip pose), so turning that hand doesn't move the aim.
- **Left-handed mode.** With the gun in the left hand the game's arms swap roles, each hand mirroring the other's pose.
- **Arms on your hands:** the first-person arms follow the controllers, with two-bone IK and an elbow model that
  follows your wrist. Each hand follows its own controller, the weapon hand included (it used to ride the gun); the
  other hand stays on the gun when two-handed. With "Gun held at its hand socket" the gun follows the weapon hand.
  Hand offsets, each shoulder's position and the elbow limits are in the **Arm IK** settings.
- **Body:** the body and legs can be moved under your view, and kept upright when you look up or down, in the **Body**
  settings. The hands stay on the controllers. Which other parts are drawn is set by the "Hide part" rows in Weapon.
- **Scripted moments** (a breach's door kick, cutscenes, a locked camera): while the game has taken your aim away, the
  gun and arms go back to the game's own animation, the shots and laser go back to the game's aim, and the reticle
  returns. They glide back onto your hands a quarter second after you have control again.
- **Manual reload:** there's no reload animation; you change the magazine yourself.
  1. Grip the magazine in the gun with your other hand. Your hand stays where it grabbed, on the magazine.
  2. Press the **magazine release** (the gripping hand's X / A by default, the SteamVR action "Magazine release") and
     pull it out the way the gun's platform takes it: straight down an AR's magwell, rocked forward on an AK's front
     lug. It only starts moving once you've pulled past the platform's pull force, and follows that far behind; at the
     end of its travel it comes free and your hand returns to the controller. The gun can't fire once it's moving.
  3. Let go away from the gun, and the magazine moves to a pouch on your chest.
  4. Grip at the pouch to take it, and bring it to the magwell: its insertion point (the in-hand magazine's top) is
     what counts. In range it turns onto the way in; at the lock distance it engages and your hand sticks to it.
  5. Push it home (an AK: rock it back) past the pull force to seat it.

  Pulling a magazine and pushing the same one back in costs nothing. The Reload button drops the magazine, as does the
  game's auto-reload when empty. The ammo count stays the game's own. If your hand gets further than "Magazine grip lets
  go beyond" from the magazine, the grip lets go. The grab box, aim point and ranges are per weapon (see **Weapon
  profiles**); the rest is per platform (see **Magazine platforms**).
- **Magazine platforms:** every gun's magazine belongs to a platform (AK, AR-15 / 5.56, AR-10 / 7.62, SMG, bolt-action
  rifle, LMG box, shotgun box, other), picked from its name, or set with "Magazine platform" in its weapon profile.
  Everything in a platform is shared by every gun on it: where the magazine sits in your hand ("Magazine in hand"
  right / up / forward / pitch / yaw / roll), straight or hinged ("In and out of the gun"), the travel or the hinge
  point and angle, the pull force, and whether it needs the magazine release. The **Magazine platform:** *name* group
  in the settings edits the platform of the gun in your hand.
- **Magazine release action:** its own SteamVR action set, "Magazine", active only while a hand grips a magazine and
  only for that hand, so the button can do something else the rest of the time. A SteamVR binding saved before it
  existed has nothing on it: then the gripping hand's X / A (left / right) works as the release and stays away from
  the game while you hold the magazine. Re-select "MOHW VR defaults" in SteamVR's bindings to get it bound properly.
- **Scoped eye (prototype, engine two-view):** bring one eye behind a scope's sight line (the weapon profile's optic
  zero: within "Eye box" of it, up to "Eye relief" behind your hand) while aiming down sights, and that eye sees
  through the scope: the world magnified by the game's own zoom for that gun (or the profile's "Scope magnification"),
  inside an eyepiece that narrows as your eye leaves the line, black around it. The other eye keeps its own view, the
  gun included. It costs nothing extra: the scoped view replaces that eye's normal one. The game's own zoom-optic mesh
  (its reticle) stays in the scoped view; a drawn crosshair stands in when there's none. Only zooms of at least "Game
  ADS zoom that counts as a scope" (2.5x) count, so red dots and iron sights aren't magnified. Settings: the **Scope**
  group (shown in the engine two-view mode).
- **Body holsters:** press grip with a hand at a body position to trigger an action. That hand also takes the weapon
  if the action is a weapon.

  | Zone | Default action |
  |---|---|
  | Stomach (either side) | Primary weapon |
  | Either hip | Secondary weapon |
  | Chest | Throw grenade |
  | Either shoulder | Melee |

  Zone positions, size and actions are all adjustable; actions can be any of the game's keyboard actions.
- **Laser:** a dot exactly on the line the shots take, with no offset of its own, sitting on the first surface that
  line hits (found with the game's own collision, so it lands on walls, cover and enemies at the right depth), with a
  semi-transparent beam in the same colour from your hand to it. Shots and tracers are aimed at the dot, so they land
  where it sits. A per-weapon "Laser start" moves the laser's beginning out to the muzzle so the gun can't block it,
  and "Beam start right / up" move where the drawn beam starts (purely visual, e.g. a laser module beside the barrel;
  the dot stays on the shot line). Beam on/off, opacity and width, the max range, and how often the raycast runs
  (every N frames) are in the settings menu.
- **Optic dot:** fixed to the gun like a sight -- zero it against the laser with its height and windage settings. Each
  dot red or green. Caveat: It's best to use the new tracer in place of this optic dot, as the optic dot is tied to the
  right hand tracking, prone to tracking jitter.
- **Peek and lean:** hold LB (double-tap the left thumbrest, or "Gamepad: LB" in the SteamVR bindings) and push the
  move stick: analog lean left / right and peek up / down, as on a gamepad. LB stays on while your thumb moves to the
  stick, and for as long as the stick is off centre. Turn off the game's
  own **Auto ADS** option to lean without aiming down sights (with it on, a full lean aims down sights by itself).
- **Weapon profiles:** the grip rotation, the optic zero, the controller aim offset, the secondary
  (two-handed) grip offset, the laser / beam start and the magazine's setup (grab box, aim point, turn, lock and seat
  distances) are kept per weapon
  and switch automatically with the weapon in your hand (picked-up weapons included). The settings menu's "Weapon
  profile: <weapon>" section edits the weapon you're holding; a weapon you haven't set up yet starts from the default
  values, which are also used when no weapon is held. The mod ships with profiles for the weapons used so far (AK-103,
  AKM, OBR, AKS-74U, PKP, HK45C, MP7, TAC-300). The campaign hasn't been played all the way through yet, so later
  weapons have no profile and use the defaults until you set them up.

### Driving missions
The game runs its driving missions in a separate executable (`game\MOHWD.exe`, see [Installing](#installing)); the
mod covers it too.
- Stereo at each eye's real field of view, with head pitch and roll. Head yaw turns the game's own vehicle camera
  (as right-stick look, up to the game's limit), so the view is never cut off where you look.
- The in-game menu shows in the HUD panel over the 3D world, as in the rest of the game, and head yaw and the wheel
  stay off the sticks while it's up.

#### Steering wheel
Steer with your hands on a virtual wheel instead of the thumbstick.

- **Grab:** hold a grip, with your hand on or near the rim. Your hand's position around the wheel's centre turns the
  wheel (top = straight ahead, clockwise = right), so move your hand round the rim as you would on a real wheel.
- **Two hands:** grip with both and the wheel turns by the average of the two hands' movement. Grabbing or letting go
  with either hand never makes the wheel jump, so you can steer hand over hand.
- **Let go** with both hands and the wheel snaps back to centre; the thumbstick steers again.
- **Full lock** is a hard stop at the "Wheel turn for full lock" angle (90 degrees by default): turning past it does
  nothing more, and turning back takes effect straight away.
- **Where it is:** fixed in your room, not to your head. It's set from where your head is at the start of the
  mission and at every recenter, so sit as you want to drive and recenter. Its distance forward, height below your
  eyes, sideways offset and tilt are adjustable and move it live.
- **Seeing it:** drawn as a grey rim with an orange marker at its top that turns with it, so you can see how far it's
  turned. Radius, thickness and opacity are adjustable (opacity 0 hides it; the radius is only the drawing, steering
  works anywhere around the centre). It's hidden in the menu and on loading screens.
- The turn goes to the game as the left stick, so the game's own steering response applies. If small turns don't
  register, raise "Stick deadzone compensation".
- In the driving missions the grips only steer: holsters and two-handed aiming are off there. Grip double-tap still
  gives LB / RB.

All of it is in the settings menu's **Driving** section; "Steering wheel (hold grip)" switches it off.

### Controls
The controllers drive a virtual Xbox gamepad, so the game itself maps buttons to actions in every context (on foot,
vehicles, the Apache, menus). Default layout:

| Touch | Gamepad | In game |
|---|---|---|
| Left stick | Left stick | Move |
| Right stick | Right stick (turn) | Turn |
| A / B / X / Y | A / B / X / Y | Jump / crouch / reload & use / next weapon |
| Stick clicks | LS / RS | Sprint / melee |
| Weapon-hand trigger | RT | Fire (accelerate in vehicles) |
| Other-hand trigger | LT | Aim down sights (brake in vehicles) |
| Double-press X / Y / B / A | D-pad left / up / right / down | Items and gadgets |
| Long-press Y | Start | Pause menu |
| Grips | - | Holsters, two-handed grip, the magazine; the steering wheel in driving missions |
| X / A of the hand gripping a magazine | - | Magazine release (SteamVR action set "Magazine", that hand only) |
| Double-tap left / right thumbrest | LB / RB | Keep the second touch down to hold. LB stays on while your thumb moves to the left stick, for peek and lean |

On Index controllers the layout is the same, with the left hand's A / B in place of X / Y (left A = Touch X, left B =
Touch Y, double presses and the long press included). The grips use squeeze force rather than finger position, because
an Index grip reads as nearly full just from holding the controller. The Index has no thumbrest, so LB / RB are a double
tap of the trackpad, and a double-tap of the grip also works. Controllers with neither use the grip double-tap (setting:
"Grip double-tap = LB / RB").

Other controllers use the same actions, moved to what each one has:

| Controller | Differences from the Touch layout |
|---|---|
| Pico, Vive Focus 3 | None |
| HP Reverb G2, Vive Cosmos | None, except the grip is bound as a button |
| Windows Mixed Reality | Trackpad click: right south / north = A / B, left south / north = X / Y, left west / east = D-pad left / right, right west / east = D-pad down / up. Left menu = Start, right menu = Back |
| Vive wand | Left / right trackpad touch = move / turn. Trackpad click: right south / north = A / B, right east = right stick click, right west = D-pad right; left south / north = X / Y, left west = left stick click, left east = D-pad left. Left menu = Start (double-press = D-pad up), right menu = Back (double-press = D-pad down) |
| Steam Frame | Real pad buttons: A, B, X, Y and Start on the right, D-pad and Back on the left, bumpers = LB / RB |

Buttons that also have a double press (A, B, X) register a hold after 0.3 s, and a tap just after the double-press
window; that is how SteamVR tells single from double presses. The grips are bound as their analog pull and the mod
makes the press and the double-tap itself (SteamVR can't bind a double press on the Touch grip), so a grab is instant
and a double-tap also counts as two quick grabs. Everything is rebindable in SteamVR. Besides the gamepad buttons (including LB, RB and Back, unbound by default),
every keyboard action the game has (read from your game profile, so in-game rebinds carry over) is available as its
own SteamVR action, listed as "Keyboard: ...", alongside "Gamepad: ..." for the pad buttons.

### Settings menu
Open the SteamVR dashboard in the headset and select the **MOHW VR** tab. It's an overlay inside SteamVR, so it works
over the running game. Point with SteamVR's laser, and scroll with the stick or the title bar's buttons. Every setting
is there, applies immediately and saves by itself. There is also a Recenter button.

| Group | What's in it |
|---|---|
| Head | Head look, its sensitivity and direction, head roll, head position |
| View | Render mode (alternating eye or engine two-view), IPD, world scale, rotation smoothing, keeping the game out of exclusive fullscreen, exact sky rays, scripted scenes from the game's input locks; two-view only: screen effects in both eyes, depth of field, backbuffer at SteamVR eye size |
| Controls | Turn speed, stick range, two-handed grab, shot spread, fire as mouse or triggers, double-tap bumpers |
| Hands | Left-handed, holster size, hand calibration, sight dots, laser beam |
| Holsters | The grab zone view, each zone's position, the magazine pouch |
| Holster actions | What each zone does |
| Arm IK | Arms on or off, hand offsets, each shoulder's position, elbow behaviour |
| Body | Body position, keeping it upright, steadying it on head turns |
| Weapon | Gun follows the controller, manual reload, the magazine grip's break distance, which body parts are hidden |
| Scope (engine two-view) | The scoped eye: on / off, eye box, eye relief, eyepiece size, the zoom that counts as a scope, the drawn crosshair |
| Weapon profile: *weapon* | Everything kept per weapon, for the weapon in your hand, including its magazine platform and scope magnification |
| Magazine platform: *platform* | The magazine in the hand, straight or hinged, travel, hinge point and angle, pull force, the release |
| HUD | The HUD panel on or off, its size, depth, IPD and offset, Hud Element Scale, the menu and movie screen size and distance, hiding the reticle (the numbers are kept per render mode) |
| Driving | Head look, stereo and the steering wheel in the driving missions |

**Grab zone view:** turn on **Show grab zones (setup)** in Holsters to see everywhere a grip does something, drawn in
the headset at its real position, so you can adjust it with the view on. Anything your hand is inside turns green.

| Shape | Shows |
|---|---|
| Blue labelled rings | The holster zones (they're spheres, and a ring is a sphere's outline from any side) |
| Orange ring | The magazine pouch |
| Yellow box | The magazine's grab box |
| Purple / pink rings | The magazine's turn range and lock distance, around where it engages (the end of its path) |
| White / red dots | The magazine's seat and its aim point |
| Green band | The two-handed grab along the barrel |

The magazine's shapes only show on a weapon manual reload works with. All of them draw over everything, including the
gun.

## Troubleshooting

If the game runs flat, or the mod doesn't seem to do anything, open `mohwvr_startup.log` next to `MOHW.exe`. It
reports the game build, SteamVR install, headset and controllers it found, and ends with why the mod is on or off.
(`mohwvr_startup_driving.log` is the same for the driving missions.) If the game folder isn't writable, as under
Program Files, the logs and settings are in `%LOCALAPPDATA%\MOHWVR` instead.

The mod talks to SteamVR only: **SteamVR must already be running** when you need VR, and an OpenXR runtime on its own
doesn't count. With Virtual Desktop, start SteamVR from Virtual Desktop first.

If `mohwvr_startup.log` doesn't exist in either place, the DLL never loaded: check that `dxgi.dll` is next to
`MOHW.exe`. When reporting a problem, include that log along with `mohwvr_proxy.log`, `mohwvr_master.log` and
`mohwvr_openvr_direct.log`.

For Steam Link "vrlink driver" errors at launch or exit, see [Known issues](#known-issues); `C:\Steam\logs\driver_vrlink.txt`
(under your Steam folder) shows whether the stream reset.

## Known issues

The rough edges known so far:

- **Hit detection doesn't always follow the shots.** The tracer and the impact go where the laser dot is, but the
  game's own hit check sometimes disagrees -- possibly because it still treats the shot as leaving from the player
  rather than from your hand.
- **The steering wheel with both hands** is janky in its default setup; one-handed steering works fairly well.
- **The gun's bones aren't centred on its grip**, so how a gun sits in your hand is always a little off, even after
  tuning its profile.
- **Shore Leave's sniping section** is wonky: the inconsistent hit detection above, and the game force-hiding the
  weapon mesh there.
- **Not every weapon has a profile yet** (the campaign hasn't been played all the way through); those use the default
  grip until you set them up.
- **Default bindings other than Touch are untested on real hardware**, including Index and Touch presented as Index by
  Virtual Desktop. The hand calibration and weapon profiles were tuned on Touch, so other controllers may need
  "Hand offset" / "Hand rotation" and the grip offsets adjusted. Controllers whose grip is bound as a button have no
  analog grip, and on HP, Cosmos and WMR two-handed aiming uses the controller's tip instead of the palm.
- **Manual reload works but is unrefined,** and has only been tested on a single rifle in a single level. The sticky
  magazine, the platforms and the release action (2026-11-14) haven't been tested in the headset yet, and every
  platform's in-hand offset starts at zero (the magazine's top at the controller's tip, as before).
- **The scoped eye is a prototype** (2026-11-14, untested in the headset): engine two-view only; the HUD shows inside
  the eyepiece; the eyepiece's edge is hard. The switch between the eye's view and the scope can be a frame off.
- **Manual reload needs a magazine that's a separate part of the gun,** so it doesn't apply to pistols yet. They reload
  as the game does. Taking the magazine out of a gun and switching weapons refills it, and the HUD's count can change
  before you've seated a new magazine.
- **With the gun in the left hand** the forearms don't twist with the wrists.
- **Steam Link can drop the stream at launch.** The game starts in exclusive fullscreen, and leaving it stops the GPU
  for about half a second. In VR the mod keeps the game out of it from then on, which took care of the same error at
  exit.
- **The robot-control section** shows the robot's camera head-locked (no head look on it yet); the right stick
  looks around.
- **Objectives where you shoot something that isn't a soldier** (a target, a device) only count the game's own aim,
  not the controller's. Until the objective is done, turn off **Gun + shots follow controller** (Numpad .) in the
  settings and aim with your head (head look can stay on): the shots then go where the game aims, at the reticle,
  which comes back while it is off. Turn it on again afterwards. With Head aim off as well, the right stick pitches
  as well as turns.
- Pre-rendered movies, menus and loading screens: the switch to a floating screen is new and may not apply to every
  movie yet.
- The sky and clouds had a per-eye offset. The "Exact sky rays" fix (on by default) is confirmed in two-view mode, not
  yet in the one-eye-per-frame mode.
- In-engine cutscenes and other scenes where the game takes look away are shown in 3D but fixed to your view, without
  head tracking (see [Scripted scenes](#scripted-scenes)). A scripted camera that doesn't take look away still gets head
  tracking.

## Contributing

[DEVELOPMENT.md](DEVELOPMENT.md) has build instructions and points to the deeper technical history in `docs/`.
