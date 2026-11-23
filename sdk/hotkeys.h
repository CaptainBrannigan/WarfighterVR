#pragma once
// KEYBOARD HOTKEYS: OFF (2026-11-23, the user: "can we disable our old hotkeys, that's why the french user's ipd was
// off due to f8, but we have no use for any hotkeys now with the settings"). Every setting is in the SteamVR dashboard
// tab, and the old keys -- F1-F12 (IPD, FOV, head aim and roll), Numpad . (Motion Controls), Pause (head position),
// PageUp / PageDown, Insert / Caps Lock, and the numpad debug keys in the SteamVR submit loop (Numpad 3 switched the
// pair publish off, bringing the right-eye ghost back) -- were easy to hit by accident while playing. Every key poll
// in the mod goes through ModKeyDown, so this one switch turns them all off; the code stays for development.

#include <windows.h>

namespace mohw {

constexpr bool kModHotkeysEnabled = false;

// Whether the key is held now (GetAsyncKeyState's high bit). Always false while the hotkeys are off.
inline bool ModKeyDown(int virtualKey)
{
    return kModHotkeysEnabled && (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

} // namespace mohw
