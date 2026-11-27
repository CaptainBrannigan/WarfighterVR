# MOHWVR

An unofficial VR mod for **Medal of Honor: Warfighter** (2012, Frostbite 2 / DirectX 11). The game has no VR support
of its own. This mod is a proxy `dxgi.dll` that hooks the game's rendering and input and talks to SteamVR directly
from inside the game: stereo rendering at the headset's real field of view, head-tracked look and movement, motion
controllers for aiming, a gun held in your hand, arms that follow your hands, manual magazine reloads, body holsters,
and an in-headset settings menu.

**Status: v1.5, a hobby reverse-engineering project.** It's playable, but there are rough edges and the campaign
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
weapon profiles made so far, see [Weapon profiles](#motion-controllers)), with no grip roll on any gun; an existing
`mohwvr_settings.ini` is kept, and only gains the built-in weapon profiles for weapons it has none for (delete it to
start from the current defaults). Two things in an older one change by themselves: the -10 degree grip roll earlier
versions started with becomes 0, and "Hud Element Scale" starts again at 1, which is automatic. Settings are changed
from inside the headset (see [Settings menu](#settings-menu)).

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
- Stereo rendering at each eye's **real, asymmetric field of view**, taken from the headset, with correct IPD. By
  default both eyes are rendered in one frame through the game's own two-view path (or one eye per frame, see the
  render modes below), and submitted to SteamVR as a matched pair with the pose they were rendered for, so SteamVR can
  reproject cleanly.
- **2D or 3D is told by what's drawn** (2026-11-22): a frame that draws the 3D world into the game's G-buffer is 3D,
  one that doesn't (a movie, a menu, a loading screen, a 2D cutscene) gets the floating screen. Unlike the old test
  (how busy the game's cameras were per second) it doesn't depend on the frame rate, so a heavy level running slowly
  no longer drops in and out of stereo.
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
  - **Engine two-view** (the default): in play, the game renders both eyes in one frame through its own two-view path. Movies,
    menus, loading screens and cutscenes before you have control stay on the floating screen. The first-person weapon
    and the HUD are drawn per eye, the in-world markers in the right direction at the HUD's depth (5 m: HUD IPD is 0.02 here by default), and the desktop
    window shows both eyes side by side. The ocean water's per-view jobs are kept in order, which fixed the crashes
    in levels with open water.

  A change applies fully at the next level load: while the level in play was loaded in the other mode, a note under
  the setting says so ("Changed: reload the level to apply"). The menu shows only the selected mode's settings. The
  HUD's numbers (menu/movie screen size and distance, Hud Element Scale, HUD scale, depth, IPD and offsets) are kept
  per mode, so each mode keeps its own tuning; the driving missions use the selected mode's too. Details:
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
- **Experimental, on by default: backbuffer at SteamVR's eye size** (View > "Backbuffer at SteamVR eye size", engine
  two-view only). In two-view each eye is only half the game's resolution; this asks the game for SteamVR's recommended
  eye size, two side by side (e.g. 5824x3112), through its own resize path, so the world and the HUD reach the headset
  at SteamVR's resolution. Needs "Keep game out of exclusive fullscreen". Switched on mid-level, it shows fully after
  a level reload. Details: [docs/native_stereo_render_path.md](docs/native_stereo_render_path.md).
- **HUD element scale** (HUD > "Hud Element Scale", 1 = automatic): the game lays its menus out in pixels, so they
  would shrink as the resolution rises. The scale follows the resolution's width, so they look the same size at any
  resolution and in both render modes: 1.5 at 5824 wide, 1 at about 3883, about 0.5 at 1920, always drawn at the full
  resolution. The setting multiplies that: above 1 bigger, below 1 smaller. Applies live, with HUD placement on (not in
  the driving missions). The HUD section sits under View, in the first column of the settings.
- The floating screen's distance is its own setting (HUD > "Menu / movie screen distance", 0.7 m by default).

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
  - when look comes back, wherever your head faces becomes straight ahead, so the view carries on from the scene's
    last camera instead of swinging back to the old forward (2026-11-21).

  Every change is logged in `mohwvr_control.log` ("INPUT LOCKS"), with the inputs taken. Details:
  [docs/camera_state_and_resets.md](docs/camera_state_and_resets.md).

### Head look
- Head yaw is **added** to the game's own turning (right stick or mouse), and head pitch is absolute; the game's own
  pitch input is removed so the view never fights your head.
- Recenter from the settings menu; the headset's own recenter is also picked up.

### Motion controllers
- **Aim with the controller:** shots leave from your hand along the controller's direction, independent of where you
  look. The weapon's random spread is removed.
- **Motion Controls** (on by default; Weapon > "Motion Controls" in the settings): the gun follows your hand and the
  shots follow the controller. Turning it off hands the gun and the shots back to the game's own aim (see
  [Known issues](#known-issues)).
- **Where the hands and gun sit: three layers**, each its own group in the settings' right-hand column, applied in
  this order:
  1. **Controller calibration (moves the aim too):** "Point right / up / forward" and "Turn pitch / yaw / roll" move
     and turn the tracked controller point itself, before anything uses it: the aim and shots, the laser, the gun,
     the hands, holsters and two-handed grab. Mirrored for the left hand. Use it when the aim itself sits wrong in your
     hand.
  2. **Hands on the controllers (looks only):** "Gun hand ..." and "Off hand ..." place the model's hands on the
     (calibrated) controllers. The gun rides the gun hand, so this moves the hand and the gun you see, never the aim.
  3. **Gun in the hand (looks only):** "Gun right / up / forward" moves the gun inside the gun hand, for all guns.

  Each gun is held at its own hand socket, the point the game's own animation holds the right hand on (its
  IK_Joint_RightHand), so no per-gun grip position is needed; a weapon profile's grip rotation turns the gun about
  that socket. Details: [docs/weapons_sockets_magazines.md](docs/weapons_sockets_magazines.md).
- **No idle animations under your hands** ("Hold hands and gun still (no idle animations)", on by default,
  2026-11-21): about a second after a weapon comes in, once you're not aiming down sights and the pose is still, the
  mod takes that pose (the hands, fingers, the gun in the hand, and the support hand on the gun) and holds it while
  you have control. Idle fidgets, breathing and walking bob no longer move the hands against the controllers or the
  gun in the hand. If it's ever taken at a bad moment, switch weapons (or the setting off and on) to take it again.
- **Weapon socket view** (Gun in the hand > "Show weapon sockets (debug lines)", off by default): every bone of the gun's
  skeleton drawn in the headset as a line from its parent to itself, with its name, plus each hand's weapon bone. For
  checking where the gun's grip points are against the hands.
- **Two-handed aiming:** grip with your other hand on the barrel to aim along the line between your hands. Only the
  other hand's position counts (its palm, the SteamVR grip pose), so turning that hand doesn't move the aim. Each
  weapon profile's "Secondary grip offset right / up / forward" moves the point aimed at from your hand to where it
  really holds the gun (below the bore for a vertical foregrip; forward along the barrel sets how far right / up tilt
  the aim). **Pistols** (2026-11-21) aim with the weapon hand alone: gripping with the other hand anywhere on the
  barrel line or within "Two-hand grab radius" of the weapon hand puts that hand on the pistol, and the aim doesn't
  change. Pistols are told by their name in the game data, so the MP7 aims two-handed even when it's your sidearm.
- **Left-handed mode.** With the gun in the left hand the game's arms swap roles, each hand mirroring the other's pose.
- **Arms on your hands:** the first-person arms follow the controllers, with two-bone IK and an elbow model that
  follows your wrist. Each hand follows its own controller, the weapon hand included (it used to ride the gun); the
  other hand stays on the gun when two-handed; the gun follows the weapon hand. The hands' places on the controllers
  are in **Hands on the controllers**; each shoulder's position and the elbow limits are in **Arm IK**.
- **Body:** the body and legs can be moved under your view, and kept upright when you look up or down, in the **Body**
  settings. The hands stay on the controllers. Which other parts are drawn is set by the "Hide part" rows under Body.
- **Scripted moments** (a breach's door kick, cutscenes, a locked camera): while the game has taken your aim away, the
  gun and arms go back to the game's own animation, the shots and laser go back to the game's aim, and the reticle
  returns. They glide back onto your hands a quarter second after you have control again.
- **Manual reload:** there's no reload animation; you change the magazine yourself.
  1. Grip the magazine in the gun with your other hand. Your hand stays where it grabbed, on the magazine.
  2. Press the **magazine release** (the gripping hand's X / A by default, the SteamVR action "Magazine release") and
     pull it straight down out of the magwell. It only starts moving once you've pulled past the platform's pull force,
     and follows that far behind; at the end of its travel it comes free, staying where your hand had it, and your hand
     returns to the controller. The gun can't fire once it's moving.
  3. Let go away from the gun, and the magazine moves to a pouch on your chest ("Mag in pouch pitch / yaw / roll" in
     the Holsters settings turn it there).
  4. Grip at the pouch to take it. How it sits in your hand is "Mag taken from the pouch" (Holsters settings):
     - **Snapped, vertical from the grip** (default): it snaps to the platform's hold ("Mag in hand" X / Z / pitch /
       yaw / roll), but stays as far up or down the magazine as you gripped it: grip it low and it sticks up out of
       your hand, grip it near the top and it hangs below. The hold's Y moves it up or down from there.
     - **Snapped to the hand:** the whole hold, Y included.
     - **Kept where grabbed:** it stays where it sat relative to your hand.

     The hold is for the left hand, mirrored in the right. Bring its top to the magwell: in range ("Magazine starts turning at") it turns onto the way in, inside "Magazine locks horizontal at"
     it's held on the magwell's line, and brought up to the seat ("Magazine seats within") it seats. No push needed.

  Pulling a magazine and pushing the same one back in costs nothing. The Reload button drops the magazine, as does the
  game's auto-reload when empty. The ammo count stays the game's own. If your hand gets further than "Magazine grip lets
  go beyond" from the magazine, the grip lets go. The grab box, aim point and ranges are per weapon (see **Weapon
  profiles**); the rest is per platform (see **Magazine platforms**).
- **Magazine platforms:** every gun's magazine belongs to a platform (AK, AR-15 / 5.56, AR-10 / 7.62, SMG, bolt-action
  rifle, LMG box, shotgun box, other), picked from its name, or set with "Magazine platform" in its weapon profile.
  Everything in a platform is shared by every gun on it: how a magazine from the pouch sits in the hand, the travel to
  clear the magwell, the pull force to pull it out, and whether it needs the magazine release (by default all but
  AR-15 / 5.56 do). The **Magazine platform:** *name* group in the settings edits the platform of the gun in your hand.
- **Magazine release action:** its own SteamVR action set, "Magazine", active only while a hand grips a magazine and
  only for that hand, so the button can do something else the rest of the time. A SteamVR binding saved before it
  existed has nothing on it: then the gripping hand's X / A (left / right) works as the release and stays away from
  the game while you hold the magazine. Re-select "MOHW VR defaults" in SteamVR's bindings to get it bound properly.
- **Scopes (prototype, engine two-view), set up per gun:** a gun has a scope when its weapon profile gives it a "Scope
  magnification" (0 = no scope). It works without aiming down sights in the game. The scope's lens is a disc centred
  on the gun's **zero** (the shot line, below): the profile sets only how far along it the lens sits ("Scope lens
  forward") and its size ("Scope lens radius"). For a red dot with a flip-to-side magnifier, set "Scope shows" to "Only with the game's
  magnifier on": the scope then appears only while the game's magnifier is switched on (ADS and click the right stick
  to flip it; it stays flipped after you let go of ADS). The **Scope** group holds what every scope shares (the scoped
  eye's eye box, eye relief, eyepiece size, drawn crosshair, and the lens picture on / off).
  - **Scoped eye** (off by default; the lens picture is on): bring one eye behind the zero (within "Eye box" of it, up to "Eye
    relief" behind your hand) and that eye sees through the scope, magnified, inside an eyepiece that narrows as your
    eye leaves the line, black around it. The other eye keeps its own view, the gun included. It costs nothing extra:
    the scoped view replaces that eye's normal one.
  - **Lens picture (screen space):** with neither eye behind the scope, both eyes see a magnified picture inside the
    lens, taken from the other eye's view (it sees past the scope rather than through it). Drawn over the frame, so it
    ignores anything in front of the lens.
  - **Holographic crosshair:** both show a crosshair on the impact point, placed where that eye sees the laser's point
    (the same spot as the laser dot), and the magnified picture is anchored there. Wherever you see the crosshair is
    where the shots land, whatever your eye's offset from the sight line: it moves a little inside the eyepiece or lens
    instead of drifting off the target. Without the laser running it marks the sight line. The game's own zoom-optic
    reticle isn't used ("Crosshair on the impact point" in the Scope group turns ours off).
- **Holographic red dot sight (new in v1.5, engine two-view):** set a gun's profile "Sight" to "Red dot
  (no magnification)". Nothing is drawn but a glowing dot on the impact point: each eye sees it where that eye sees the
  laser's point, so it's zeroed at whatever range you aim at, with no parallax. The dot only shows while that eye looks
  through the sight's window, a disc centred on the gun's zero with its own "Red dot window forward" (how far along the
  zero) and "Red dot window radius" in the profile, separate from the scope lens's. So it comes and goes as you bring
  the gun up to your eye, and the window itself is never drawn. To aim with the dot alone, set the gun's "Aim ray" to
  Hidden (below). "Red dot size" sets the dot's size in view. With "Scope shows: only with the game's magnifier on" and a
  magnification, flipping the game's magnifier turns the red dot into the scope.
- **Body holsters:** press grip with a hand at a body position to trigger an action. That hand also takes the weapon
  if the action is a weapon.

  | Zone | Default action |
  |---|---|
  | Stomach (either side) | Primary weapon |
  | Either hip | Secondary weapon |
  | Chest | Throw grenade |
  | Either shoulder | Melee |

  Zone positions, size and actions are all adjustable; actions can be any of the game's keyboard actions. A held grip
  that eases off and comes back within 0.3 s is the same hold, not a new press, so it can't fire the zone your hand
  is in (2026-11-21: a held left grip easing off during a thumbrest double tap threw grenades at the chest). On Touch a
  grip only counts as let go once it has been released for 0.1 s.
- **Laser:** a dot exactly on the line the shots take, with no offset of its own, sitting on the first surface that
  line hits (found with the game's own collision, so it lands on walls, cover and enemies at the right depth), with a
  semi-transparent beam in the same colour from your hand to it. Shots and tracers are aimed at the dot, so they land
  where it sits. A per-weapon "Laser start" moves the laser's beginning out to the muzzle so the gun can't block it,
  and the gun's **zero** ("Zero right / up" in its profile, where the shot line starts beside your hand) moves the whole
  line: beam, dot, shots, and both sight windows (the scope lens and the red dot's window are centred on it). Set it at
  the gun's sight and the shots leave from there, so a gun held low (behind a boat's side, low cover) still shoots over
  it. Once a gun's zero is where you want it (at its sight), its profile's **"Aim ray: Hidden"** hides that gun's beam
  and dot and you aim with the sight alone: the laser still runs underneath, so the shots, a scope's crosshair and a red
  dot still sit on the same point (the OBR 7.62's default profile comes set up this way). The aim laser on/off, its
  opacity and width, the max range, and how often the raycast runs (every N frames) are in the settings menu; turning
  the aim laser and dot off there only hides them too.
- **Peek and lean:** hold LB (double-tap the left thumbrest, or "Gamepad: LB" in the SteamVR bindings) and push the
  move stick: analog lean left / right and peek up / down, as on a gamepad. LB stays on while your thumb moves to the
  stick, and for as long as the stick is off centre. Turn off the game's
  own **Auto ADS** option to lean without aiming down sights (with it on, a full lean aims down sights by itself).
- **Weapon profiles:** the grip rotation, the zero, the controller aim offset, the sight and its window, the secondary
  (two-handed) grip offset, the laser start and the magazine's setup (grab box, aim point, turn, lock and seat
  distances) are kept per weapon
  and switch automatically with the weapon in your hand (picked-up weapons included). The settings menu's "Weapon
  profile: <weapon>" section edits the weapon you're holding; a weapon you haven't set up yet starts from the default
  values, which are also used when no weapon is held. The mod ships with profiles for the weapons used so far (AK-103,
  AKM, OBR 5.56, OBR 7.62, AKS-74U, HK416, Mk18, PKP, HK45C, MP7, TAC-300). The campaign hasn't been played all the way through yet, so later
  weapons have no profile and use the defaults until you set them up.

### Driving missions
The game runs its driving missions in a separate executable (`game\MOHWD.exe`, see [Installing](#installing)); the
mod covers it too.
- Stereo at each eye's real field of view, with head pitch and roll. Head yaw turns the game's own vehicle camera
  (as right-stick look, up to the game's limit), so the view is never cut off where you look.
- **Both render modes** (2026-11-18, engine two-view new and untested): View > "Render mode" applies here as well. In
  engine two-view the HUD is drawn into each eye's half at the HUD settings' place. Not there yet in two-view: depth
  of field isn't turned off (cutscenes may show a faint double image), and the render scale and the eye-sized
  backbuffer don't apply.
- **Cutscenes show as the floating screen** (2026-11-21), as in the rest of the game: the game presents them with
  vsync and play without, which is how they're told apart (with the game's own vsync on there's no difference, and
  cutscenes stay in 3D).
- **Exclusive fullscreen stays on here**: "Keep game out of exclusive fullscreen" doesn't apply to the driving
  missions (a run with it went black at a few frames a second, sound and input still working).
- **Logs are always written here for now**, as `mohwvr_driving_*.log` next to `MOHW.exe`, whatever "Write log files"
  says.
- The in-game menu shows in the HUD panel over the 3D world, as in the rest of the game, and head yaw and the wheel
  stay off the sticks while it's up.

### Specialized sequences (new 2026-11-22, not yet tested in the headset)
**Use 6DoF Motion For Specialized Sequences** in the dashboard switches all of them on or off (on by default). Each
sequence also has its own profile below it. Off, a sequence plays as any other scene, still in stereo.

#### Zodiac Sequence (the boat in Basilan)
The RHIB sequence in Basilan is recognised by the level, the boat and your seat, so nothing here acts anywhere else.
Its settings are in the dashboard's **Sequence profile: Zodiac Sequence** group.
- **6DoF in both seats:** your head moves and turns in the boat, with a level horizon, and the view turns with the boat.
  Your hands stay in the game's animation, on the wheel or the gun. Recenter sets straight ahead.
- **Grenade launcher:** hold grip with one or both hands and move them as on the gun's handles: down and left sends the
  barrel up and right. The turret is driven until its handles match your hands. Let go, and the thumbstick turns it,
  with the view coming round. The mod doesn't move or re-pivot the gun, so aiming stays the game's. By the game's code
  and data (not yet seen in play) grenades fly parallel to the barrel from 0.32 m above it, and drop with distance.
- **Driving:** a steering wheel like the driving missions' (below): hold grip to steer. The triggers are throttle and
  brake. The wheel's lock, placement and look are the profile's own, starting from your driving wheel's.

#### Steering wheel
Steer with your hands on a virtual wheel instead of the thumbstick.

- **Grab:** hold a grip, with your hand on or near the rim. Your hand's position around the wheel's centre turns the
  wheel (top = straight ahead, clockwise = right), so move your hand round the rim as you would on a real wheel.
- **Two hands:** grip with both and the wheel turns by the average of the two hands' movement. Grabbing or letting go
  with either hand never makes the wheel jump, so you can steer hand over hand.
- **Let go** with both hands and the wheel snaps back to centre; the thumbstick steers again.
- **Full lock** is a hard stop at the "Wheel turn for full lock" angle (190 degrees by default): turning past it does
  nothing more, and turning back takes effect straight away.
- **Where it is:** fixed in your room, not to your head. It's set from where your head is at the start of the
  mission and at every recenter, so sit as you want to drive and recenter. Its distance forward, height below your
  eyes, sideways offset and tilt are adjustable and move it live.
- **Seeing it:** drawn as a grey rim with an orange marker at its top that turns with it, so you can see how far it's
  turned. Radius, thickness and opacity are adjustable (opacity 0 hides it; the radius is only the drawing, steering
  works anywhere around the centre). It's hidden in the menu and on loading screens.
- The turn goes to the game as the left stick, so the game's own steering response applies. If small turns don't
  register, raise "Stick deadzone compensation".
- In the driving missions the grips only steer: holsters and two-handed aiming are off there. Grip double-tap, when
  turned on, still gives LB / RB.

All of it is in the settings menu's **Driving** section; "Steering wheel (hold grip)" switches it off.

### Controls
The game reads a gamepad only while its window has keyboard focus. If the sticks and buttons are dead until you click
the game's window, turn on Controls > "Controllers work without window focus" (off by default, 2026-11-22, untested in
game; driving missions too). It doesn't cover holsters, whose key presses still need the window focused.

**Other controllers plugged in** (a steering wheel, a joystick, pedals) could be taken by the game as its gamepad
instead of the motion controllers, which then did nothing. With Controls > "Hide other game controllers (wheels,
joysticks)" on (the default), the game can't see them while SteamVR is running, so the motion controllers are the only
gamepad it finds. The keyboard and mouse are untouched, and a real Xbox pad still works alongside the motion
controllers. Turn it off to use a wheel in the game; the game may only look for controllers when it starts, so restart
it after changing this. `mohwvr_dinput.log` lists the controllers the game saw and which were hidden.

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
tap of the trackpad. Controllers with neither can use a double-tap of the grip: turn on Controls > "Grip double-tap =
LB / RB" (off by default since 2026-11-23).

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
and a double-tap also counts as two quick grabs. A grip counts as pressed at 30 % pull and let go under 10 % (Index:
squeeze force, unchanged). Everything is rebindable in SteamVR. Besides the gamepad buttons (including LB, RB and Back, unbound by default),
every keyboard action the game has (read from your game profile, so in-game rebinds carry over) is available as its
own SteamVR action, listed as "Keyboard: ...", alongside "Gamepad: ..." for the pad buttons.

### Settings menu
Open the SteamVR dashboard in the headset and select the **MOHW VR** tab. It's an overlay inside SteamVR, so it works
over the running game. Point with SteamVR's laser, and scroll with the stick or the title bar's buttons. Every setting
is there, applies immediately and saves by itself. There is also a Recenter button. Three columns (2026-11-23):
rendering on the left, hands and weapons in the middle, body and arm details on the right.

**No keyboard hotkeys** (since v1.5): the mod reads no keys at all, so nothing changes a setting by accident. Earlier
versions had keys such as F7 / F8 for the IPD scale and Numpad . for the motion controls; the settings menu replaces
them. If an IPD, FOV or head setting changed by itself on an older version, check it in the menu.

| Column | Group | What's in it |
|---|---|---|
| Left | View | Render mode (alternating eye or engine two-view), IPD, world scale, rotation smoothing window, keeping the game out of exclusive fullscreen, exact sky rays, scripted scenes from the game's input locks; two-view only: screen effects in both eyes, depth of field, backbuffer at SteamVR eye size |
| Left | Head | Head look, its sensitivity and direction, head roll, head position |
| Left | HUD | The HUD panel on or off, its size, depth, IPD and offset, Hud Element Scale, the menu and movie screen size and distance, hiding the reticle (the numbers are kept per render mode) |
| Left | Driving | Head look, stereo and the steering wheel in the driving missions |
| Left | Specialized sequences | Use 6DoF Motion For Specialized Sequences: the switch over every sequence profile |
| Left | Sequence profile: Zodiac Sequence | 6DoF in the RHIB's seats, the grenade launcher's grip aim and how hard it follows, the wheel with its own lock, placement and look, stick deadzone compensation |
| Left | Diagnostics | Write log files: off by default, since logging costs frames; turn it on to report a problem (see [Troubleshooting](#troubleshooting)) |
| Middle | Weapon | Motion Controls, manual reload, the magazine grip's break distance |
| Middle | Controls | Left-handed, turn speed, stick range, two-handed grab, shot spread, fire as mouse or triggers, double-tap bumpers, controllers without window focus |
| Middle | Aim ray (laser) | The aim laser and its dot, their look, whether they show only while gripping, the max range, how often it casts |
| Middle | Sights (engine two-view) | What every sight shares: the scoped eye on / off, eye box, eye relief, eyepiece size, the drawn crosshair; the lens picture on / off; the red dot's size |
| Middle | Weapon profile: *weapon* | Everything kept per weapon, for the weapon in your hand: grip rotation, aim offset, its zero (where the shot line starts) and whether its aim ray shows, its sight (scope or red dot) and how far along the zero its window sits and how big, the secondary grip, the magazine |
| Middle | Magazine platform: *platform* | Travel to clear the magwell, pull force out, the release |
| Right | Controller calibration (moves the aim too) | The controller point moved and turned: moves everything, the aim included |
| Right | Hands on the controllers (looks only) | Where the model's gun hand and off hand sit on their controllers; holding the hands and gun still |
| Right | Gun in the hand (looks only) | The gun moved inside the gun hand (all guns); the weapon socket view |
| Right | Arm IK | Arms on or off, each shoulder's position, elbow behaviour |
| Right | Holsters | The grab zone view, holding grip to keep a weapon, holster size and height, each zone's position, the magazine pouch |
| Right | Holster actions | What each zone does |
| Right | Body | Body position, keeping it upright, steadying it on head turns, which body parts are hidden |

**Grab zone view:** turn on **Show grab zones (setup)** in Holsters to see everywhere a grip does something, drawn in
the headset at its real position, so you can adjust it with the view on. Anything your hand is inside turns green.

| Shape | Shows |
|---|---|
| Blue labelled rings | The holster zones (they're spheres, and a ring is a sphere's outline from any side) |
| Orange ring | The magazine pouch |
| Yellow box | The magazine's grab box |
| Purple / pink rings | The magazine's turn range and lock distance, around its seat |
| White / red dots | The magazine's seat and its aim point |
| Green band | The two-handed grab along the barrel |

The magazine's shapes only show on a weapon manual reload works with. All of them draw over everything, including the
gun.

## Troubleshooting

**The mod writes no log files by default**, because logging costs frames. To get them, turn on Diagnostics > **Write
log files** in the settings menu, or, if the mod isn't running far enough to show the menu, put `WriteLogFiles=1` in
`mohwvr_settings.ini` next to `MOHW.exe` (create the file if it isn't there). Then start the game again.

If the game runs flat, or the mod doesn't seem to do anything, turn the logs on and open `mohwvr_startup.log` next to
`MOHW.exe`. It reports the game build, SteamVR install, headset and controllers it found, and ends with why the mod is
on or off. (`mohwvr_startup_driving.log` is the same for the driving missions.) If the game folder isn't writable, as
under Program Files, the logs and settings are in `%LOCALAPPDATA%\MOHWVR` instead.

The mod talks to SteamVR only: **SteamVR must already be running** when you need VR, and an OpenXR runtime on its own
doesn't count. With Virtual Desktop, start SteamVR from Virtual Desktop first.

If, with the logs on, `mohwvr_startup.log` doesn't exist in either place, the DLL never loaded: check that `dxgi.dll` is
next to `MOHW.exe`. When reporting a problem, include that log along with `mohwvr_proxy.log`, `mohwvr_master.log` and
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
- **Shooting from the boat at the start of Hat Trick:** the boat's side is too high to
  aim over from where a recenter puts you, and the controllers can't lift the gun above it. Before you shoot, crouch
  or lower your head and recenter there, then straighten up: your standing height is then above the recentered one,
  and you can aim over the side. Raising the gun's "Zero up" (its weapon profile) to the sight also helps:
  the shots then leave from there.
- **Not every weapon has a profile yet** (the campaign hasn't been played all the way through); those use the default
  grip until you set them up.
- **Default bindings other than Touch are untested on real hardware**, including Index and Touch presented as Index by
  Virtual Desktop. The hand calibration and weapon profiles were tuned on Touch, so other controllers may need
  "Controller calibration" and the hand placements adjusted. Controllers whose grip is bound as a button have no
  analog grip, and on HP, Cosmos and WMR two-handed aiming uses the controller's tip instead of the palm.
- **Manual reload works but is unrefined,** and has only been tested on a few rifles. Going in is the first version's
  again (2026-11-18); a weapon profile's "Magazine locks horizontal at" is measured from the seat, as it was before
  2026-11-14, so a value tuned since then may want widening.
- **The scopes are a prototype** (2026-11-14): engine two-view only; the HUD shows inside
  the eyepiece; the eyepiece's edge is hard. The switch between the eye's view and the scope can be a frame off. The
  lens picture is drawn over everything (a hand in front of the lens doesn't hide it). The magnifier switch was found
  on one gun (the OBR's) with one toggle each way.
- **Manual reload needs a magazine that's a separate part of the gun,** so it doesn't apply to pistols yet. They reload
  as the game does. Taking the magazine out of a gun and switching weapons refills it, and the HUD's count can change
  before you've seated a new magazine.
- **With the gun in the left hand** the forearms don't twist with the wrists.
- **Steam Link can drop the stream at launch.** The game starts in exclusive fullscreen, and leaving it stops the GPU
  for about half a second. In VR the mod keeps the game out of it from then on, which took care of the same error at
  exit.
- **The robot-control section** shows the robot's camera head-locked (no head look on it yet); the right stick
  looks around.
- **Some trucks in Connect the Dots** won't register being shot with motion controller aim: the game's aim check only
  counts its own aim. Turn off **Motion Controls** (Weapon group) in the settings while you take them out,
  then turn it back on (the next point has the details).
- **Objectives where you shoot something that isn't a soldier** (a target, a device) only count the game's own aim,
  not the controller's. Until the objective is done, turn off **Motion Controls** (Weapon group) in the
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
