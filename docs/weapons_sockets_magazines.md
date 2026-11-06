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

**Next step:** once a few guns are logged, compare the right hand's camera position with the tuned grip profiles
(e.g. the OBR's 0.075 / -0.14 / -0.24). If they line up after the hand calibration, the profile defaults can be
filled from the sockets for every gun, and the two-handed grip can target IK_Joint_LeftHand instead of a fixed offset.

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
