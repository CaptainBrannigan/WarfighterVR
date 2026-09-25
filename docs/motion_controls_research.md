# Motion controls research: how bioshock-vr does it

Not yet started -- captured for when v1 (head-tracked stereo 3D) is done and
motion controllers become the next scope. Researched by reading
[bioshock-vr](https://github.com/mohamad-balouza/bioshock-vr) (the same repo
that originated the 64-bit companion-process idea, see
docs/companion_process_findings.md), specifically to answer: can we reuse
their approach or DLL for MOHW's motion controls?

## Key finding: their target is 64-bit, ours isn't

bioshock-vr targets **BioShock 1 *Remastered*** (`src/game/bioshock1r/`),
which is a 64-bit executable -- unlike the original 2007 BioShock. Their
entire mod (D3D11 hooking, OpenXR session/input, XInput faking) runs as a
single in-process 64-bit mod. They never hit the "32-bit can't reach
SteamVR's OpenXR runtime" wall this project did (docs/phase2d_findings.md),
so they never needed a companion-process split for themselves -- the
"64-bit companion" idea referenced from their own `docs/RESEARCH.md` was a
contingency plan for a hypothetical 32-bit target, not what they actually
ship. **We can't literally drop in their DLL** -- their whole input design
assumes OpenXR is reachable in the same process that's faking the gamepad
state, which isn't true for MOHW (32-bit, OpenXR only reachable from our
separate 64-bit `companion` process).

## Their architecture, piece by piece

- **`src/proxy/xinput_proxy.cpp` / `.def`** -- a proxy `xinput1_3.dll`,
  the exact same pattern as this project's proxy `dxgi.dll`: implements
  all 8 real XInput exports (`XInputGetState`/`GetStateEx`, `SetState`,
  `GetCapabilities`, `Enable`, `GetDSoundAudioDeviceGuids`,
  `GetBatteryInformation`, `GetKeystroke`) and forwards each to the real
  system DLL by ordinal/name. One addition beyond plain forwarding:
  `BVR_SetPostGetStateHook()`, a registration seam letting their mod DLL
  install a callback that fires after every real `XInputGetState` call, so
  it can rewrite the returned state before the game sees it.
- **`src/core/vr/openxr_input.cpp` / `.h`** -- reads real OpenXR controller
  state once per frame, in-process: `input_get_hand_pose()` (position +
  orientation, either AIM or GRIP pose) per hand, plus button/stick state
  via the OpenXR action system (`xrCreateActionSet`,
  `xrSuggestInteractionProfileBindings`, `xrSyncActions`,
  `xrGetActionStatePose`/`...Boolean`/`...Float`, `xrLocateSpace` per
  hand). Also exposes `input_set_sim_hand()`/`input_clear_sim_hands()` for
  injecting fake poses during testing -- worth borrowing that idea
  regardless of architecture, it's a cheap way to test hand-pose-dependent
  code without a headset on.
- **`src/core/input/xinput_bridge.h`** -- composes the above into a tiny
  12-byte POD mirroring XInput's own shape:
  ```cpp
  struct Gamepad {
      uint16_t buttons = 0;
      uint8_t lt = 0, rt = 0;
      int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  };
  ```
  Published once per frame via `publish_xr_state()` (called from the
  render/XR thread) and `publish_vr_gameplay()` (called from the game
  thread during `CalcView`, gating transforms like pitch-kill on stick Y).
  The hooked `XInputGetState` reads the latest composed value via
  `last_xr_pad()`/`last_composed_triggers()`/`last_composed_sticks()`.
  Notably **no locks** -- reads are passive snapshots, and stale
  publishing is handled by timestamp-based expiry rather than
  synchronization. That's a reasonable simplification for continuous
  analog input (a slightly-stale stick value is harmless) that would NOT
  be reasonable for our video frames (hence this project's keyed-mutex
  handoff for eye textures, see docs/companion_process_findings.md) --
  different data has different correctness requirements.
- **`src/game/bioshock1r/hands.cpp` / `.h`** -- separate, deeply
  game-specific code rendering a visible weapon/hand model attached to the
  tracked hand pose (the actual "looks like you're holding the gun where
  your controller is" visual piece). This is a whole separate
  reverse-engineering effort on top of input reading, tied tightly to
  BioShock's own renderer -- not something this research pass dug into in
  detail, since it's Frostbite-specific work of its own whenever we get
  there, not something transferable from BioShock's code either way.

## What transfers to MOHW, and how it has to change

The *pattern* transfers even though the code can't be reused directly,
split across our existing process boundary instead of living in one
process:

1. **Companion process reads controller pose/buttons.** It already owns
   the OpenXR session (docs/companion_process_findings.md) -- add the
   action-system calls there, same shape as `openxr_input.cpp` but as a
   new module in `companion/`.
2. **Publish composed state back to the 32-bit side via shared memory** --
   a small, lock-free, timestamp-stamped struct in the same spirit as
   their `Gamepad`, sent over a plain `CreateFileMappingW` block like this
   project's existing `HandleExchangeBlock` (`shared/ipc_protocol.h`).
   This is the new direction: everything built so far
   (`HandleExchangeBlock`, the eye textures) flows proxy_dll -> companion;
   this would be the first companion -> proxy_dll channel.
3. **Build our own small proxy XInput DLL**, following bioshock-vr's exact
   "forward everything + post-state hook" shape. Needs its own
   investigation first: which XInput version MOHW actually imports (check
   via `dumpbin /imports` against `MOHW.exe`, the same technique already
   used to enumerate the real `dxgi.dll`'s exports, see
   `docs/dxgi_exports.txt`) -- almost certainly `xinput1_3.dll` for a 2012
   title, but worth confirming rather than assuming.
4. **Separately, later: find and hook MOHW's visible-weapon rendering** to
   attach it to hand pose. Deferred -- genuinely new Frostbite-specific
   research, not something this pass answered.

## Not yet started

Everything above is design-informing research only. No code has been
written for motion controls; v1 scope remains head-tracked stereo 3D only
(see the original plan). Revisit this doc when that scope is actually
picked up.
