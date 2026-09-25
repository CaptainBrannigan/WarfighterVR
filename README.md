# MOHWVR

An unofficial VR mod for **Medal of Honor: Warfighter** (2012, Frostbite 2 /
DirectX 11). The game has no built-in VR support — this mod adds head-tracked
stereo rendering and motion-controller aiming via a proxy `dxgi.dll` that
hooks the game's own D3D11 rendering and connects directly to SteamVR.

**Status: early/experimental.** This is a hobby reverse-engineering project,
not a polished release. Expect to need some patience getting it running, and
expect the known issue below.

## Current state

- **Rendering (SteamVR / OpenVR, 32-bit direct connection):** this is the
  active, default rendering path. Full color quality, no intermediate copy
  step. **Known issue: a persistent ghosting/double-image artifact in the
  right eye only**, under active investigation — this is the main thing that
  needs solving before the mod is really usable end-to-end. See
  [DEVELOPMENT.md](DEVELOPMENT.md) for the technical writeup of what's been
  ruled out so far, if you want to help dig into it.
- **Rendering (OpenXR, historical):** an earlier version of the mod used a
  separate 64-bit companion process talking to OpenXR instead of connecting
  to OpenVR directly. It rendered reliably with no ghosting, at the cost of
  a color-quality loss from the extra cross-process copy step. That
  architecture has since been retired from the main codebase in favor of the
  OpenVR path, but it's fully preserved in git history and could be revived
  as a separate branch if the OpenVR ghosting turns out to be a dead end.
- **Motion-controller aiming:** the hook that redirects the player's aim
  direction to the right VR controller's live pose is implemented and has
  been confirmed working. It currently has no live controller-pose feed,
  though — that data path was supplied by the retired OpenXR companion
  process and hasn't been reconnected to the current OpenVR pipeline yet.
  Reconnecting it and refining tracking accuracy is open work.
- **Head tracking, FOV matching, head-roll/lean:** working.

## Installing

Copy into the game's install directory (next to `MOHW.exe`):

- `dxgi.dll` (Found in Releases or build it yourself — see [DEVELOPMENT.md](DEVELOPMENT.md))
- `openvr_api.dll`, a current copy from your SteamVR install (e.g.
  `<Steam>\steamapps\common\SteamVR\bin\win32\openvr_api.dll`)

Launch the game normally with SteamVR running.

## Contributing

The right-eye ghosting bug is the most valuable thing to help with right
now. [DEVELOPMENT.md](DEVELOPMENT.md) has build instructions and points to
the deeper technical history in `docs/`.
