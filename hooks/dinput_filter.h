#pragma once
// OTHER GAME CONTROLLERS HIDDEN (2026-11-27, the user: "can we dictate controller input priority while our mod is
// active, and make the motion controllers the top priority?"). A tester with a steering wheel, a joystick and pedals
// plugged in had dead Quest controllers: SteamVR Input read every stick and button, but the game never polled the mod's
// virtual gamepad (XInputGetState slot 0, hooks/xinput_hook.h) at all. The game reads gamepads through both XInput
// (xinput1_3.dll) and DirectInput (dinput8.dll DirectInput8Create), and with a DirectInput controller present it can take
// that one as its gamepad instead.
//
// With HideOtherGameControllers on (default), while SteamVR is running or the mod is on, the game's DirectInput sees no
// game controllers -- joysticks, gamepads, wheels, flight sticks, pedals (DirectInput's "supplemental") -- only the
// keyboard, mouse and other device types, so the mod's XInput pad is the only gamepad it can find. Keyboard and mouse
// are untouched (the game reads keys through WM_INPUT anyway). A real Xbox pad still works through XInput: it's merged
// into the virtual one.
//   - IDirectInput8 A and W, EnumDevices: game controllers are left out of the enumeration.
//   - CreateDevice: a game controller asked for by its GUID (one saved from an earlier enumeration) isn't created.
// The functions are hooked inside dinput8.dll, so a DirectInput object the game made before the hooks is covered too,
// for every enumeration from then on.
// Installed early, at the first DXGI call (proxy_dll/dllmain.cpp EnsureInitialized), in both executables and whether
// the mod is on yet or not: the game may only look for controllers at startup. The connected devices are logged once
// (mohwvr_dinput.log), with every enumeration the game makes (the first few in full).

namespace mohw {

bool InstallDirectInputFilter();

} // namespace mohw
