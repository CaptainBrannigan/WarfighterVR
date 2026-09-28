#pragma once
// Which hand holds the weapon, body holsters and two-handed grip (2026-09-28). Runs on the submit thread from
// vr_input.cpp, on raw tracking-space hand poses; the weapon hand's pose is what gets published as the aim pose.
//
// Weapon hand: LeftHanded picks the default. Pressing a grip with that hand inside a weapon holster zone switches to
// that weapon (sends its key) and makes that hand the weapon hand; with HolsterHoldToKeep it only stays so while the
// grip is held, otherwise (default) until another grab. Grip with the other hand near the weapon's barrel = two-handed
// aim along weapon hand -> off hand. Non-weapon zones (grenade, melee) only send their key.

#include "../sdk/vr_math.h"

namespace mohw::openvr_direct {

constexpr int kLeftHand = 0;
constexpr int kRightHand = 1;

struct HandInput
{
    bool tracked = false;
    float pose[3][4] = {}; // tracking space: columns 0-2 right/up/back, column 3 position
    bool grip = false;
    bool trigger = false;
};

struct HandsResult
{
    int weaponHand = kRightHand;
    bool twoHanded = false;
    Quat weaponOrientation{}; // valid when twoHanded: the weapon hand's orientation aimed at the off hand
    bool fire = false;         // the weapon hand's trigger
    bool ads = false;          // the off hand's trigger
};

// head: tracking-space head position and orientation (haveHead false if no head pose yet).
void UpdateHands(const HandInput hands[2], bool haveHead, const float headPos[3], const Quat& headOrientation,
                 HandsResult* out);

// Any thread: whether the weapon is currently in the left hand (the weapon drive mirrors its grip offset).
bool IsWeaponHandLeft();

// Reads the game's own key bindings (Documents\MOHW\settings\PROF_SAVE_profile) now rather than at the first holster
// grab, so that grab never waits on disk. Called once from InitVrInput.
void PreloadGameBindings();

// Submit thread: holds (down) or releases a game action's key, looked up from the game's own bindings by its
// GstKeyBinding.infantry concept name. Returns the key used, or -1 if the action has no keyboard key.
int SetGameActionKey(const char* concept, bool down);
// Same for a fixed DirectInput scancode (keys the profile doesn't hold, e.g. the menus' Enter / Escape). Returns dik.
int SendGameScancode(int dik, bool down);

} // namespace mohw::openvr_direct
