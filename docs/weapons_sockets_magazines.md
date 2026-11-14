# Every gun, its hand sockets and its magazine (2026-11-06)

From the game data (`tools/fb2dump`: `Data\Win32\Weapons.sb` and the eleven `MOHW\Levels\SP` superbundles) and the
first-person skeleton. The hand sockets are measured live by this build (below); nothing here has been seen in a
headset yet.

## The guns

**Campaign / base blueprints** (the profile keys, `MOHW/Weapons/<CLASS>/<NAME>/<NAME>`):

| Class | Guns |
|---|---|
| Assault | AK103, DD_M4, HK416_MOHW, MK18, OBR_556, SCAR_H, SCAR_PDW, SUCHKA |
| LMG | M249, MG4, PKP |
| Sniper | CS5, OBR_762, TAC300 |
| SMG | MP7 |
| Shotgun | AA12, M870 |
| Pistol | M1911 |
| Launcher | RPG7 |
| Grenades | M18_SMOKE, M67_FRAG |
| Melee | KNIFE, TOMOHAWK |

**Campaign variants** (`…/spec/…`, how the levels' weapons are actually keyed, e.g. `AK103/spec/AK103__AKM`):
AK103__AKM, AK103__SNIPER, SUCHKA__NON_DISPOSABLE, SMAW__SP, GLOCK_HEAVYASSAULT, GLOCK23__SP, HK45C__SP, P226__SP,
and the 40 mm M203 / M32 rounds. The MAARS robot has its own fire profiles (`weaponfire_m203_maars`,
`1_mode_m203_maars`).

**Multiplayer** (`Weapons.sb`, `mp_weapons/<name>_mp_soldierweaponbundle`): AA12, AK5C, CS5, DDM4, EBR, F88, G36C,
G3, HK416, HK416C, M240B, M249, MG4, MK18, MP7, OBR556, OBR762, PKP, SCARPDW, TAC300 and TAC50.

The game's own weapon-type registry (`DAT_0278C18C`, 44 entries with class bitmasks) lists the same set, plus the MP
names.

## The two hand sockets

The first-person skeleton (179 bones, logged in `mohwvr_rig.log`) has the points the game's animation holds the hands
on, next to the gun's bones:

| Bone | Index | What |
|---|---|---|
| Wep_Root | 153 | the gun's root (about mid-length, a foot below the gun) |
| IK_Joint_LeftHand | 172 | the secondary grip |
| IK_Joint_RightHand | 173 | the pistol grip |
| Wep_Aim | 176 | the aim point |
| Wep_Mag, Wep_Mag_Ammo | 159, 160 | the magazine |
| RightWeapon | 124 | the gun's attach point on the right hand |

The IK joints are animated per weapon, so they're measured live. **This build logs, once per weapon, 1.5 s after it's
drawn** (the game's own pose, before the arm IK touches it), to `mohwvr_weaponsockets.log`:

- each socket relative to Wep_Root and in the camera frame (right, up, back);
- both hand centres;
- the gun's current grip profile, for comparison.

**Compared (2026-11-08):** the user's tuned grip profiles put the controller at an almost fixed offset from each gun's
IK_Joint_RightHand at rest (profile grip minus socket, camera frame: right, up, back):

| Gun | Socket in the camera | Profile grip | Profile - socket |
|---|---|---|---|
| OBR 556 | 0.134, -0.175, -0.131 | 0.075, -0.14, -0.24 (roll 1) | -0.059, +0.035, -0.109 |
| HK45C SP | 0.137, -0.131, -0.251 | 0.095, -0.095, -0.34 (pitch -23) | -0.042, +0.036, -0.089 |
| PKP | 0.132, -0.262, -0.122 | 0.09, -0.21, -0.24 (roll 5) | -0.042, +0.052, -0.118 |

So one offset for all guns can replace the per-gun grip positions. The controller sits about 5 cm left, 4 cm up and
10.5 cm forward of the socket, within 2 cm across the three guns (the pistol's pitch accounts for some of it). The
IK joint is rigid against Wep_Root: in the camera frame the offset's length stays 0.234 m for the OBR, however the
animation turns the gun.

**The gun at its hand socket** (Hands > "Gun held at its hand socket (game data)", `GunAtSocket`, off by default):

- `hooks/camera_matrix_test_hook.cpp` `GripInCamera` takes the grip's position from the equipped gun's
  IK_Joint_RightHand, read from every body batch's bones (`ReadGunSocketInCamera`, before the arm IK). At first the
  socket went on the controller's pose plus an offset of its own; since 2026-11-09 it goes where the IK hand holds it
  (below). The profile's grip rotation still applies, now about the socket.
- Math: J is the controller's pose against the camera with the gun as it is now, so a gun point p shows at
  (p - J.t) * J.R^T in the controller's frame. The socket s at offset o takes J.t = s - o * J.R. With the OBR's
  -0.059 / +0.035 / -0.109 as o, this reproduces its profile exactly.
- Read every frame, so the grip stays in the hand while the animation moves the gun (sway, the draw).
- `mohwvr_cameramatrix.log` "GUN AT SOCKET" gives, once per gun, the socket, the grip it gives, and the profile's.
- `mohwvr_weaponsockets.log` now also gives IK_Joint_RightHand against Wep_Root in the gun's own frame (Wep_Root's
  skinning rotation taken out), and the right hand's and the socket's turn in the camera per gun (skinning rows).

**Hand-relative grip (open):** placing the gun from the hand would make the arm IK the authority: the gun on the
hand, the hand on the controller with one calibration. The position side is what the socket mode does: the socket is
where the game puts the hand. The orientation side needs the hand's turn against the controller.

- The skeleton read gives bind positions only, not bind rotations. So a bone's turn is only known against its bind
  pose (its skinning matrix), and the controller-to-hand turn has to be calibrated once from a tuned gun.
- Then, per gun, the grip turn would be the reference gun's times the hand's turn change between the guns:
  J.R(g) = J.R(ref) * H(ref)^-1 * H(g), with H = the hand bone's skinning rotation in the camera.
- The new log's per-gun hand rotations show whether the tuned pitches (the HK45C's -23, the MP7's +10) are the
  game's own hand angles.
- `RightWeapon` (124) is the gun's attach point on the hand, another candidate for the hand frame.

The two-handed grip can target IK_Joint_LeftHand the same way.

**The weapon hand on its controller (2026-11-09, the user: "since we have a setting for the gun to follow the hand,
remove the hand IK follows the gun"):**

- `ApplyArmIk` used to move the weapon hand by the gun's move, plus the "Right hand ... (on the gun)" offsets. It
  now puts the weapon hand on its own controller like the off hand (`HandOnController`): the hand's centre on the
  controller plus RightHandIk right / up / forward, turned by RightHandIk pitch / yaw / roll. The dashboard rows say
  "(on its controller)". A two-handed off hand, or one without a controller pose, rides the gun's own move.
- In the game's own pose the right hand's centre sits about 2.5 cm left and 6.5 cm forward of IK_Joint_RightHand on
  every gun logged (OBR, SUCHKA, AKM, HK45C).
- The user's tuned profiles put that hand centre about 2.5 cm right, 4 cm down and 4 cm behind the controller's pose.
- So the matching starting value is RightHandIk right 0.025 / up -0.04 / forward -0.04.

**The gun follows the IK hand (2026-11-09, the user: "Arm IK hand follows controller, gun follow hand of arm IK,
correct?" It wasn't: the gun's socket went on the controller plus an offset of its own).**

- `GripInCamera` now puts the socket where the arm IK's weapon hand holds it. The hand's centre h is at `off`
  (RightHandIk position) in the controller's frame, turned by R_h (RightHandIk rotation), carrying the hand's pose.
  The socket s, where the game holds the hand on the gun, rides it to o = off + (s - h) * R_h^T.
- On top of that is the gun's own fine offset in the hand: Hands > "Gun offset in the hand right / up / forward"
  (`GunInHandOffset*`, default 0). It replaces "Gun socket offset", whose keys were renamed so the old values, which
  meant something else, don't carry over.
- The hand centre comes from the same body batch as the socket (`ReadGunSocketInCamera`, before the arm IK).
- With the gun in the left hand, the offsets and the socket's side of the hand are mirrored.
- So there's one calibration, the hand on its controller, and every gun sits in the hand as the game holds it.

**Fixed the same day: the gun moved with the head** (the user: "The weapon is now affected by the head, while the
hand IK is not. The gun grip is not in the hand grip").

- A body batch is drawn by several passes, and the arm IK solves its array once, on the first. The socket and the hand
  centre were read on every pass, so the later passes read the hand already on its controller. Its "centre" was then
  the controller's place in the camera, which moves as the head turns, and o = off + (s - h) * R_h^T moved with it.
- The log showed it: the OBR's first grip came out at the game's hand centre (0.103, -0.179, -0.192), then the
  grenade's at (0.093, -0.326, -0.016), the controller's place.
- Fix: `ReadGunSocketInCamera` reads only an array the IK hasn't solved yet (`IkAlreadyWritten`). The weapon batches
  take the latest reading from any thread; they used to prefer their own thread's, which could be a later pass's.
- The "GUN AT SOCKET" log line is now taken 1.5 s after a weapon change, as the socket log is, and also gives the
  game's hand centre it was measured from.

**Still open: the gun's angle in the hand.** The socket's position goes where the hand holds it, but the gun's turn
is still the profile's grip rotation, and the hand's is RightHandIk's. They only match where both are the same: most
profiles have no rotation, the pistol has pitch -23 and the default profile roll -10. Two ways to close it:

- The gun takes the hand's turn, so it sits exactly as the game holds it. The barrel then points where the game's own
  animation points it against the hand, which isn't the controller's aim line for every gun.
- Or that, and the shots and laser follow the barrel (Wep_Aim) instead of the controller's forward.

## The bone hierarchy and the socket view (2026-11-09)

The user: "Can you create a debug listing the weapon sockets and showing a line from their start to finish".

**Where each bone hangs** (from the game data, `tools/fb2dump`: `Animations/Skeletons/Venice1pSke01` with the
weapon sub-skeleton `Animations/Skeletons/Weapon/WeaponSke01` mapped onto bones 153-176 by
`Venice1pMasterSkeleton`):

- Wep_Root (153) hangs off SpineX (42), not the hand. The gun is animated in the upper body's frame and the hands are
  solved onto its IK joints.
- Every socket hangs off Wep_Root, except Wep_Mag_Ammo (Wep_Mag), Wep_Grenade2 (Wep_Grenade1), the belt chain
  (Wep_Belt2 to 5, each off the one before) and Wep_Bipod2 / 3 (Wep_Bipod1).
- RightWeapon (124) hangs off RightHand (123), LeftWeapon (51) off LeftHand (50). The arm IK doesn't move them: they
  aren't hand bones by name.
- The runtime skeleton asset is a `SkeletonAsset` { BoneNames, Hierarchy, LocalPose, ModelPose } after the asset's
  name: the bind poses the rig reads at +0x18 are ModelPose, and the Hierarchy is at +0x10 (an int per bone, its
  parent, with the count before the data). `ReadSkeleton` reads it, checks it (the root's parent is -1, every other
  bone's an earlier bone), and falls back to these name rules if that fails. `mohwvr_rig.log`: "bone hierarchy ...".

**The view** (Hands > "Show weapon sockets (debug lines)", `ShowWeaponSockets`, off by default):

- Every weapon bone is drawn as a line from its parent's joint (start) to its own (end), with its name beside the
  end, facing the head. Wep_Root gets its name only. Bones on their parent's joint (parts the gun doesn't have) are
  left out.
- They're drawn where the game draws the bone: a gun bone with the gun (on the weapon controller through the grip J
  while the drive has it), a hand bone (RightHand, RightWeapon, LeftHand, LeftWeapon, marked "(hand)") with its hand
  (on its controller through the hand's calibration while the arm IK has the arms). It's the way the controllers are
  taken into the view, run backwards.
- Colours: IK_Joint_RightHand green, IK_Joint_LeftHand cyan, Wep_Aim red, Wep_Mag yellow, Wep_Trigger orange,
  Wep_Slide magenta, Wep_Root and the hand bones white, the rest light blue.
- SteamVR overlays, two per bone, made the first time a bone shows (none until the view is used). They're drawn over
  the game's frame, so they show through the gun. Hidden half a second after the last update (no body batch).
- `mohwvr_weaponsockets.log` lists every weapon bone once per gun, from its parent in the gun's own frame, with its
  length.

## Magazines

- **Separate magazine parts** (placed on Wep_Mag / Wep_Mag_Ammo by `FUN_00A37160`, so manual reload already finds
  them):
  - AK family (`attachments/magazines/ak*`, `suchka_mag`);
  - the AR family (MagPul PMAGs: OBR_556, DD_M4, MK18, HK416, plus HK416's own);
  - SCAR_H, SCAR_PDW, MP7, AA12, CS5, OBR_762, TAC300 and TAC50;
  - M249 and PKP (ammo boxes and pouches), M240, and the MP rifles.
- **No separate part: the pistols** (M1911, GLOCK17 / 23 / heavy assault, HK45C, P226). Each is a single first-person
  mesh, so the magazine is inside it. This build logs, for every first-person mesh the gun is drawn with, whether
  it's **skinned to Wep_Mag** (`mohwvr_rig.log`: "weapon mesh … SKINNED TO Wep_Mag" or "body part mesh … SKINNED
  TO Wep_Mag"). If it is, a pistol's manual reload is a bone rewrite of Wep_Mag (and Wep_Mag_Ammo) in that batch's
  bone palette, with the same seat and hand math as `DriveMagazine`. The arm IK already rewrites bones in the body
  batch the same way.
- **No magazine:**
  - M870 (tube-fed: the game reloads it a round at a time, reload type 0);
  - RPG7 and SMAW (the rocket);
  - MG4 (a belt: check its parts for a box);
  - grenades and melee.

## Sticky magazines and platforms (2026-11-14, untested in the headset)

Code: `openvr_direct/vr_reload.cpp` (the state machine, submit thread, tracking space), `openvr_direct/vr_magazine_path.h`
(the path math, shared), `hooks/camera_matrix_test_hook.cpp` (`DriveMagazine`, `StickOffHandToMagazine`), the settings
in `sdk/settings.cpp` (`MagazinePlatform`, the `MagazinePlatform0..7` ini lines).

- **Frames.** Everything is in *gun-local*: the gun's rest-view axes at the seated Wep_Mag point. The magazine's own
  frame (*mag-local*) is gun-local while it's seated, so a magazine at path position s is `PathDisplacement(s)` in
  gun-local, and its batch is drawn with `W = (batch -> mag-local) * frame`, where frame is
  `PathDisplacement(s) * gunInView` on the path, `MagazineHold(platform) * offController` in the hand, and the pouch
  otherwise. s = 0 is exactly the gun's own move.
- **Paths.** Straight: along the seat -> weapon profile aim point line, `travel` metres. Hinge: about the gun's right
  axis through a point `{forward, up}` from the seat, the bottom swinging forward (+angle about +x turns up toward
  back), `hingeDeg` to clear.
- **Stickiness.** The controller's position is projected onto the path (`ProjectOnPath`: along the axis, or the angle
  about the hinge), and s follows it with a dead band of `pullForce / PathLength` on either side: the magazine moves
  only once the pull passes the pull force, and lags that far behind. Seating takes the same push past the seat (less
  the weapon profile's "seats within", capped at a quarter of the path).
- **The hand.** `handInMag` (the off controller's frame in mag-local) is recorded at the grab (going in, at the
  engagement), and the arm IK reaches for `handInMag * PathDisplacement(s) * gunInView` instead of the controller.
  Leaving the path, seating and the grip letting go (`MagazineGripBreak`) start a 160 ms glide of the hand (and, when it
  comes off the path, the magazine) back to the controller.
- **The release.** SteamVR action set `/actions/magazine` with `/actions/magazine/in/MagRelease`, activated each update
  over the main set (priority 1) with `restrictedToDevice` = the gripping hand's input source, so only that hand's button
  is taken. Default: the lower face button (Touch X / A, Index A, WMR / Vive trackpad south, Steam Frame left D-pad down
  / right A). Unbound (a binding saved before it existed): the hand's pad X / A is read instead and masked from the
  virtual pad. If SteamVR ever refuses the restricted pair, the set is dropped and the pad fallback stays.
- **Insertion point.** The held magazine's Wep_Mag point is `MagazineHold(platform).t` in the hand's frame, so the
  platform's in-hand offset moves the point that engages at the path's far end (`entry = PathDisplacement(1)`).
- **For later:** a charging handle or a bolt release is the same pattern: a grab box on the gun, a path, a pull force,
  an optional release action, and a completion callback.
