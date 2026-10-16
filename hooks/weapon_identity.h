#pragma once
// Which weapon is equipped (2026-10-15, for per-weapon profiles). Every weapon has its own AimingController, and the
// hooked AimingController::Update runs only for the player's equipped one (hooks/aiming_controller_hook.cpp). Found by
// four live probes (mohwvr_weaponid.log, 2026-10-15) -- the chain from the controller to the weapon's name:
//   AimingController+0x1C0 -> its owning ClientSoldierWeapon + 0x7C   (so owner = [AC+0x1C0] - 0x7C)
//   ClientSoldierWeapon+0x430 = the controller (the cross-check), +0x440 = its ClientWeapon
//   a SoldierWeaponUnlockAsset field a little before +0x430 (seen at +0xAC / +0xBC / +0xDC by weapon), whose
//   +0x24 Weapon is the SoldierWeaponBlueprint -- an Asset, its name at +0x8, e.g.
//   "MOHW/Weapons/ASSAULT/OBR_556/OBR_556" (the player's starting rifle), ".../ASSAULT/SUCHKA/SUCHKA" (AKS-74U),
//   ".../GRENADES/M67_FRAG/M67_FRAG".
// Objects are recognised by their reflected type: a typed object's vtable slot 0 is GetType, compiled as
// "mov eax, <TypeInfo>; ret", and the TypeInfo leads to the type's name -- read from the code bytes, never called.
// Resolved on the game thread whenever the controller changes: a few hundred memory reads. Logged to
// mohwvr_weaponid.log.
//
// Dead ends on the way (don't repeat): walking pointers OUT from the controller reaches the whole squad's weapons
// (teammates' M249 / M79 got reported as the player's); the controller's aiming data (AC+0x0) is shared by several
// weapons; a 0x7FFF0000 pointer bound rejected everything -- the game is large-address-aware, its heap is above 2 GB.

#include <cstddef>
#include <cstdint>

namespace mohw {

// AimingController::Update (hooks/aiming_controller_hook.cpp), every tick: the equipped weapon's aiming controller.
void NoteActiveAimingController(void* aimingController);

// Any thread: the equipped weapon's blueprint name (the profile key, e.g. "MOHW/Weapons/LMG/PKP/PKP") and a short
// display name ("PKP"). False until a weapon has been identified (or when the last one couldn't be).
bool GetEquippedWeapon(char* key, size_t keySize, char* display, size_t displaySize);

// Any thread: bumped every time the equipped weapon changes (for whoever needs to react to a switch).
unsigned GetEquippedWeaponGeneration();

// Any thread: the equipped weapon's ClientSoldierWeapon (+0x42C its soldier, +0x440 its ClientWeapon), or 0 -- checked
// on every call to still hold the active aiming controller.
uintptr_t GetEquippedSoldierWeapon();

// Any thread: a typed object's reflected type name (from its GetType's code bytes, see above); false if the object
// isn't one. For diagnostics (hooks/laser_raycast.cpp names what the laser hit).
bool GetReflectedTypeName(uintptr_t object, char* out, size_t size);

} // namespace mohw
