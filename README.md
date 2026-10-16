# MOHWVR

An unofficial VR mod for **Medal of Honor: Warfighter** (2012, Frostbite 2 / DirectX 11). The game has no VR support
of its own. This mod is a proxy `dxgi.dll` that hooks the game's rendering and input and talks to SteamVR directly
from inside the game: stereo rendering at the headset's real field of view, head-tracked look and movement, motion
controllers for aiming, a gun held in your hand, body holsters, and an in-headset settings menu.

**Status: Beta, a hobby reverse-engineering project.** It's playable, but there are rough edges and the campaign
hasn't been played all the way through yet; see [Known issues](#known-issues).

## Requirements

- Medal of Honor: Warfighter (the Steam/Origin PC release, 32-bit `MOHW.exe`).
- SteamVR **2.17 or newer** (the release that added a 32-bit runtime), with a headset and motion controllers.
  Developed and tested on a Meta Quest 3 over Steam Link with Touch controllers; other headsets should work, but only
  the Touch controllers get default bindings.
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
newer version of the mod until you switch back to the defaults (or add the new actions to your copy). New in the
Beta: "Left / Right hand position for two-handed grip", bound to the Grip pose; without it two-handed aim uses the
controller's tip instead of your palm.

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
- **Two-handed aiming:** grip with your other hand on the barrel to aim along the line between your hands. Only the
  other hand's position counts (its palm, the SteamVR grip pose), so turning that hand doesn't move the aim.
- **Left-handed mode.**
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
- **Peek and lean:** hold LB ("Gamepad: LB" in the SteamVR bindings, or a left-grip double-tap held on the second
  press) and push the move stick: analog lean left / right and peek up / down, as on a gamepad. Turn off the game's
  own **Auto ADS** option to lean without aiming down sights (with it on, a full lean aims down sights by itself).
- **Weapon profiles:** the grip offset and rotation, the optic zero, the controller aim offset, the secondary
  (two-handed) grip offset and the laser / beam start are kept per weapon
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
vehicles, the Apache, menus). Default Touch layout:

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
| Grips | - | Holsters, two-handed grip; the steering wheel in driving missions |
| Double-tap left / right grip | LB / RB | (setting: "Grip double-tap = LB / RB") |

Buttons that also have a double press (A, B, X) register a hold after 0.3 s, and a tap just after the double-press
window; that is how SteamVR tells single from double presses. The grips are bound as their analog pull and the mod
makes the press and the double-tap itself (SteamVR can't bind a double press on the Touch grip), so a grab is instant
and a double-tap also counts as two quick grabs. Everything is rebindable in SteamVR. Besides the gamepad buttons (including LB, RB and Back, unbound by default),
every keyboard action the game has (read from your game profile, so in-game rebinds carry over) is available as its
own SteamVR action, listed as "Keyboard: ...", alongside "Gamepad: ..." for the pad buttons.

### Settings menu
Open the SteamVR dashboard in the headset and select the **MOHW VR** tab. Every setting is there and applies
immediately: head look, view (IPD, world scale, smoothing), controls (turn speed, stick deadzone, two-handed grab,
shot spread, fire as mouse or triggers), hands (left-handed, holsters, sight dots), weapon grip, HUD, holster
positions and actions, and the driving missions (head look, stereo, steering wheel). There is also a Recenter button.

## Known issues

This is a Beta. The rough edges known so far:

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
- **The robot-control section** shows the robot's camera head-locked (no head look on it yet); the right stick
  looks around.
- **Objectives where you shoot something that isn't a soldier** (a target, a device) only count the game's own aim,
  not the controller's. Until the objective is done, turn off **Gun + shots follow controller** (Numpad .) in the
  settings and aim with your head (head look can stay on): the shots then go where the game aims, at the reticle,
  which comes back while it is off. Turn it on again afterwards. With Head aim off as well, the right stick pitches
  as well as turns.
- Pre-rendered movies, menus and loading screens: the switch to a floating screen is new and may not apply to every
  movie yet.
- The sky and clouds have a small per-eye offset.
- Scripted "breach" scenes lock the camera; head look can briefly fight them.
- In-engine cutscenes are shown in 3D with head tracking, the same as gameplay.

## Contributing

[DEVELOPMENT.md](DEVELOPMENT.md) has build instructions and points to the deeper technical history in `docs/`.
