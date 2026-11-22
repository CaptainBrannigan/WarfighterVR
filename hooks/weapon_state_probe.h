#pragma once
// WEAPON STATE PROBE (2026-11-17), logged to mohwvr_weaponstate.log whatever the log switch says (sdk/logging.h
// LogAlways), as the user asked for these probes.
//
// 1. MAGNIFIER TOGGLE -- ANSWERED, the field watch is gone: the AimingController's +0x194 is the flip magnifier's switch
//    (it holds out of ADS) and +0x160 the zoom level (hooks/weapon_identity.h GetEquippedOpticState).
//
// 2. WEAPON SLOTS (the user: "account for rifles that are picked up in level and not primary to the game"): holsters
//    need the weapons the player carries. ClientSoldierWeaponsComponent (2026-09-13 live dumps, vtable 027A9830): +0xC8
//    its soldier, +0xCC / +0xD0 the begin / end of its slot array of ClientSoldierWeapon pointers. Found here without
//    the vtable: the object reached from the soldier (hooks/player_control.h) whose +0xC8 is that soldier and whose
//    array holds the weapon in hand. Every slot is listed (with its weapon's name) on every weapon change and whenever
//    the slots change. (The first try started from ClientSoldierWeapon+0x42C, which isn't the soldier: a per-weapon
//    object, 0x100 apart for the four weapons seen.)
//
//    FOUND (2026-11-18, mohwvr_weaponstate.log): soldier +0x554 -> ClientSoldierWeaponsComponent, 11 slots: 0 primary,
//    1 sidearm, 2 a launcher picked up (the SMAW), 5 melee, 6 grenades. The ClientSoldierWeapon objects are made anew at
//    checkpoints: read the slots each time, never keep their pointers.
//
// 3. THE GAME'S RETICLE TEXTURE -- REMOVED (2026-11-18): walking the optic meshes' materials for D3D11 objects and
//    calling QueryInterface on what it found crashed the game after a cinematic (MOHW.exe.20880.dmp: the call at
//    dxgi.dll+0x5CD2F jumped to B0E06FE0 -- a stale object whose vtable still pointed into d3d11.dll). The walks it made
//    found no textures within three pointers of the materials anyway. Don't call into objects found by scanning.

namespace mohw {

// Present (game thread), once per frame.
void WeaponStateProbeOnPresent();

} // namespace mohw
