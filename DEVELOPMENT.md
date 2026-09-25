# Development

Build/setup/technical-history reference for contributors. If you're just
looking to install and run the mod, see [README.md](README.md) instead.

An unofficial VR mod for **Medal of Honor: Warfighter** (Frostbite 2, DirectX
11). There's no built-in stereo-rendering pipeline to lean on — this mod
builds the whole head-tracked stereo render path from scratch via a proxy
`dxgi.dll` that hooks D3D11 directly and connects to SteamVR's 32-bit OpenVR
runtime in-process (SteamVR 2.17+ supports 32-bit clients directly; no
separate companion process is needed).

For the deep technical history (how each piece was found/built), see
`docs/`. This file is just setup.

## Prerequisites

- **Medal of Honor: Warfighter** installed.
- **Visual Studio 2022** (or another MSBuild toolchain) with the C++ desktop
  workload — needed to build `MOHWVR.sln`.
- **SteamVR** (2.17+) for your headset.

## Building

Build via the **solution file**, not the project's `.vcxproj` directly —
building the project on its own breaks `$(SolutionDir)` resolution and drops
the output in the wrong folder.

```
MSBuild MOHWVR.sln /p:Configuration=Release /p:Platform=Win32 /t:proxy_dll
```

Output (`dxgi.dll`) lands in `bin\<Configuration>\`. After any change, verify
it stays DRM-safe with `dumpbin /imports bin\Release\dxgi.dll` — it must show
exactly `d3d11.dll`, `USER32.dll`, `KERNEL32.dll`, nothing else (a
DRM-protected game will refuse to launch if the proxy's own import table
looks unusual).

## Installing

Copy into the game's own install directory (next to `MOHW.exe`):

- `dxgi.dll` (from `bin\<Configuration>\`)
- `openvr_api.dll` (a current copy from your SteamVR install, e.g.
  `<Steam>\steamapps\common\SteamVR\bin\win32\openvr_api.dll`) — loaded
  dynamically at runtime, never statically linked.

That's the entire install — the game loads `dxgi.dll` as a normal system DLL
(it's a proxy that forwards to the real one), which connects to SteamVR on
first `Present`. No other injection step is needed.

## First launch / configuration

On first launch, the mod creates `mohwvr_settings.ini` next to `dxgi.dll` in
the game folder, along with a set of `mohwvr_*.log` files (useful for
diagnosing anything that doesn't look right — check `mohwvr_proxy.log`
first). Settings can be edited directly in that file (game must be closed)
or changed live in-game via hotkeys, which persist back to the file
automatically.

### Hotkeys (current build)

| Key | Effect |
|---|---|
| F1 | Head-aim sensitivity down **and** head-aim pitch-invert toggle (both fire — see collision note below) |
| F2 | Head-roll toggle **and** head-aim sensitivity up (both fire — see collision note below) |
| F3 | Head-aim recenter |
| F4 | Head-aim yaw invert toggle |
| F5 / F6 | FOV scale down / up |
| F7 / F8 | IPD scale down / up |
| F9 | Dump live (decrypted) game module to disk |
| F10 | Dump module registry (diagnostic) |
| F12 | Head-roll invert toggle **and** head-aim on/off toggle (both fire — see collision note below) |
| Home | Rotation smoothing on/off |
| Page Up / Page Down | Rotation smoothing window +/- |
| Pause | Head-position (positional) tracking on/off |
| Scroll Lock | Arm one frame-trace diagnostic capture |
| Caps Lock | **Diagnostic**, right-eye-ghost investigation: forces the right eye's texture to update every frame instead of holding it stale (see `docs/`) |
| Delete | Bone-hide proof-of-concept on/off |
| Numpad − / + | Bone-hide range start down / up |
| Numpad / / * | Bone-hide range end down / up |
| End | Capture player bone distance |
| Numpad 8 / 9 | Player bone distance threshold down / up |
| Numpad 0 | Cycle entity candidate (bone-hide diagnostic) |
| Numpad 1 / 2 | Single-bone step down / up (bone-hide diagnostic) |
| Numpad 4 | **Diagnostic**: render-pose-stamping (`Submit_TextureWithPose`) on/off, for isolating the right-eye ghost |

**Known hotkey collisions** (pre-existing, not yet cleaned up): F1, F2, and
F12 are each bound twice, in two different hook modules that both poll
every frame, so pressing any of them fires *both* bound actions at once —
see `hooks/aiming_controller_hook.cpp`'s and `hooks/fov_scale_hook.cpp`'s
own hotkey constants.

Two settings have **no live hotkey** and require editing
`mohwvr_settings.ini` directly + relaunching: `HeadAimEnabled` and
`HeadAimSensitivity` (their toggle/step hotkeys are intentionally disabled
in code right now — see `hooks/aiming_controller_hook.cpp`).

### Stereo rendering

The mod renders one eye per frame, alternating between frames
(`hooks/alternating_eye.cpp`) — always on, not a toggle. Each eye is one
frame "stale" relative to the other; `hooks/render_pose_stamp.cpp` corrects
for this by stamping each submitted frame with the head orientation it was
actually rendered for, so SteamVR's own reprojection compensates for the lag.

### Known-good starting settings

A confirmed-good baseline — start here rather than tuning from scratch:

```
FovScale=2.4000
IpdScale=1.0000
RotationSmoothingEnabled=1
RotationSmoothingWindowMs=43.3333
HeadRollEnabled=1
HeadAimEnabled=1
```

### Known limitations

- **Head-aim and mouse-look fight each other.** With `HeadAimEnabled=1`,
  the mod overwrites the game's look direction from HMD orientation every
  gameplay tick, discarding whatever the mouse just did that same tick —
  don't use both at once.
- **Character/weapon mesh can visibly lag ("ghost") during fast head
  rotation.** Root-caused to the game's own animation system running on a
  fixed ~30Hz simulation tick, independent of render rate — not something
  a render-side hook can reach. Camera rotation itself is smoothed
  (`RotationSmoothingEnabled`) to compensate for the same underlying
  mismatch; mesh posing isn't.
- **In-game video/display settings** (resolution, fullscreen, refresh
  rate, etc.) live in a separate Frostbite profile file, not
  `mohwvr_settings.ini` — on Windows, typically
  `%USERPROFILE%\Documents\MOHW\settings\PROF_SAVE_profile`. Change these
  through the game's own in-game menu, not by hand-editing the file — it's
  a binary profile that likely carries a checksum.

## Reverse-engineering workflow: thread-safe instruction tracing (x32dbg)

The game's engine dispatches a lot of per-shot/per-entity work across a worker-thread pool (`EA::Jobs`-style), so any conditional breakpoint on a shared code path — not just one tied to a specific object — will legitimately fire on multiple different threads. A plain x32dbg instruction trace (`Trace Into`) has no thread awareness, so it happily interleaves whichever thread the OS scheduler happens to run next, producing a log that jumps between unrelated call chains and is effectively unreadable.

**Don't fix this by suspending the other threads.** On this engine that risks a genuine deadlock — some traced code paths have a real cross-thread dependency on a worker thread completing queued work, and suspending it stalls the trace forever (or crashes the game).

The fix that works: let every thread run normally, and filter *after the fact* — record the starting thread ID once, single-step continuously, and simply skip logging any step where the current thread has changed. The OS is still free to interleave other threads; you just don't record their instructions.

```
; Allocate variables
var target_tid
var current_cip
var step_count

; Store our current starting thread ID
set target_tid, tid()

trace_loop:
; Step Into exactly one instruction
sti

; Get our new position
set current_cip, cip

; Stop once we reach the target address (replace with whatever you're tracing toward)
cmp current_cip, 008252E0
je trace_complete

; Increment step counter
inc step_count

; Step ceiling (0x7530 = 30000) -- raise/lower depending on how long the traced chain is
cmp step_count, 7530
je trace_complete

; If the OS switches threads mid-step, skip logging it -- but keep stepping
cmp tid(), target_tid
jne trace_loop

; Log address + full disassembly text + the registers you care about
log "{cip} | {dis.text(cip)} | eax={eax} ecx={ecx} edx={edx} ebp={ebp} esp={esp} esi={esi} edi={edi}"

; Loop back
jmp trace_loop

trace_complete:
msg "Trace successfully completed!"
ret
```

**Usage:**
1. Arm a normal conditional breakpoint at whatever entry point you want to start tracing from (e.g. `breakif(<your gate condition>)`).
2. Once it hits, paste this script into x32dbg's script window and run it.
3. It logs to the Console/Log window one line per instruction on your original thread only, until it either reaches the target address or hits the step ceiling.

**Tuning notes:**
- Swap `008252E0` for whichever address you're tracing toward.
- Trim the logged register list to whatever's actually relevant to the function you're in — logging all 8 GPRs every step gets noisy fast on a long trace.
- Pick your starting breakpoint as low/close to the target as you reasonably can. Starting too far upstream (e.g. at a generic, frequently-reused utility function) means the single-step trace wanders through a huge amount of unrelated code before ever reaching what you care about — in one case here, starting a few call levels too high produced a 6000+ line trace that never got anywhere near the intended target.
- `dis.text(cip)` gives the full disassembled instruction (mnemonic + operands); `dis.mnemonic(cip)` gives just the mnemonic if you want a terser log.

### Open setup questions (not yet confirmed — help wanted)

- **Whether running the game in Fullscreen vs. Windowed/Borderless
  meaningfully affects performance hasn't been isolated as its own test.**
  One session switched away from Fullscreen and set an explicit 120Hz
  refresh rate at the same time, and screen tearing went away — but those
  two changes were never separated, so it's not confirmed which one (or
  both) actually mattered. Worth a controlled A/B before recommending one
  mode over the other as a hard rule.
- **Other `PROF_SAVE_profile` render settings may also be worth tuning**
  for performance (resolution scale, effects/shadow/texture quality
  tiers, etc.) — not yet reviewed systematically.
