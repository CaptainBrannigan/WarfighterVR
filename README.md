# MOHWVR

An unofficial VR mod for **Medal of Honor: Warfighter** (2012, Frostbite 2 / DirectX 11). The game has no VR support
of its own. This mod is a proxy `dxgi.dll` that hooks the game's rendering and input and talks to SteamVR directly
from inside the game: stereo rendering at the headset's real field of view, head-tracked look and movement, motion
controllers for aiming, a gun held in your hand, body holsters, and an in-headset settings menu.

**Status: 1.0, a hobby reverse-engineering project.** It's playable, but not every part of the game has been
covered yet; see [Known issues](#known-issues).

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

Start SteamVR, then launch the game normally. The mod creates `mohwvr_settings.ini` and its SteamVR action and
binding files next to the DLL on first launch. Settings are changed from inside the headset (see
[Settings menu](#settings-menu)).

**Controller bindings:** in SteamVR's controller bindings for the game, pick **"MOHW VR defaults"**. If you
previously saved your own binding for the game, SteamVR keeps using that copy and won't pick up new actions from a
newer version of the mod until you switch back to the defaults (or add the new actions to your copy).

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
- Pre-rendered movies, menus and loading screens are shown as a floating screen (see [Known issues](#known-issues)).

### Head look
- Head yaw is **added** to the game's own turning (right stick or mouse), and head pitch is absolute; the game's own
  pitch input is removed so the view never fights your head.
- Recenter from the settings menu (or F3); the headset's own recenter is also picked up.

### Motion controllers
- **Aim with the controller:** shots leave from your hand along the controller's direction, independent of where you
  look. The weapon's random spread is removed.
- **The gun follows your hand** (on by default, Numpad . toggles), with adjustable grip offset and rotation.
- **Two-handed aiming:** grip with your other hand on the barrel to aim along the line between your hands.
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
- **Sight dots:** an optic dot fixed to the gun (zero it with its height and windage settings) and an aim-ray dot
  along the controller's line, each red or green.

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
| Grips | - | Holsters, two-handed grip |

Everything is rebindable in SteamVR. Besides the gamepad buttons (including LB, RB and Back, unbound by default),
every keyboard action the game has (read from your game profile, so in-game rebinds carry over) is available as its
own SteamVR action, listed as "Keyboard: ...", alongside "Gamepad: ..." for the pad buttons.

### Settings menu
Open the SteamVR dashboard in the headset and select the **MOHW VR** tab. Every setting is there and applies
immediately: head look, view (IPD, world scale, smoothing), controls (turn speed, stick deadzone, two-handed grab,
shot spread, fire as mouse or triggers), hands (left-handed, holsters, sight dots), weapon grip, HUD, and holster
positions and actions. There is also a Recenter button.

## Known issues

- Pre-rendered movies, menus and loading screens: the switch to a floating screen is new and may not apply to every
  movie yet.
- Mission markers are shifted along with the HUD instead of staying on their targets.
- The sky and clouds have a small per-eye offset.
- Scripted "breach" scenes lock the camera; head look can briefly fight them.
- In-engine cutscenes are shown in 3D with head tracking, the same as gameplay.

## Contributing

[DEVELOPMENT.md](DEVELOPMENT.md) has build instructions and points to the deeper technical history in `docs/`.
