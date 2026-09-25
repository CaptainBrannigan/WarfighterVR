# Companion-process findings: the 64-bit sidestep architecture

Follows on from `docs/phase2d_findings.md` (OpenVR ruled out, then 32-bit
OpenXR ruled out). Per the user's direction ("let's attempt the sidestep so
that we can target steamvr"), this is the "64-bit companion process"
architecture from bioshock-vr's own documented fallback plan: a separate,
non-injected 64-bit process owns all OpenXR/SteamVR interaction, and the
32-bit injected mod only has to hand it rendered eye textures.

## Milestone A: standalone 64-bit OpenXR session -- done

`companion/main.cpp` (a plain, non-injected 64-bit console app) proved a
full OpenXR session against this machine's SteamVR install: instance ->
system -> D3D11 graphics requirements (adapter-LUID-matched device) ->
session -> per-eye swapchains -> a real `xrWaitFrame`/`xrBeginFrame`/
`xrLocateViews`/`xrEndFrame` loop submitting solid red (left) / blue
(right) colors. Confirmed live in-headset.

Getting here surfaced two environment-specific, non-code blockers, both
fully root-caused:

1. **Process/session mismatch.** Running the companion via the coding
   agent's own shell tool put it in a different Windows session than the
   interactive desktop where `vrserver` runs, breaking a `CSharedResource
   NamespaceClient`-side `OpenProcess()` call against `vrserver`'s PID with
   `ERROR_ACCESS_DENIED`. Confirmed the same way in reverse: `taskkill`/
   `Stop-Process` from the same tool against `vrserver.exe` etc. also
   failed with Access Denied. Fix: run the companion in the real
   interactive session (the user running it directly, elevated) -- a
   non-issue for the real mod, since the companion will eventually be
   spawned as a child of the game process itself, inheriting the user's
   own session automatically.
2. **The ~90-second post-launch discovery window.** `vrserver`'s
   `CSharedResourceNamespaceServer` (the pipe a new client uses to
   discover the running `vrserver`'s PID) shuts itself down roughly 90
   seconds after `vrserver` starts, once the "well-known" boot-time
   clients (vrmonitor, vrwebhelper, ...) have already connected. A client
   launched well after that window has nothing to connect to and hangs
   indefinitely retrying `WaitNamedPipe`. Confirmed empirically,
   reproducibly, by the user: fully quitting and relaunching SteamVR, then
   running the companion within the fresh window, connects instantly and
   reliably every time.

## Milestone B: cross-process shared D3D11 texture handoff -- done

Built `investigation/test_shared_texture_producer.cpp` (32-bit, standalone,
non-injected) as a synthetic stand-in for the eventual real producer (the
32-bit proxy DLL). It creates two keyed-mutex D3D11 textures (left/right
eye, 1920x1080, deliberately a different size and aspect ratio than the
companion's per-eye swapchain, to exercise the same size-mismatch handling
the real game backbuffer will need) and writes an animated pulsing color
into each ~60 times/sec.

`companion/main.cpp` was extended to open these two textures and blit their
content into its OpenXR swapchain images every frame (falling back to the
original solid-color fill if the producer isn't running, so it still works
standalone). `shared/ipc_protocol.h` defines the cross-process contract.

### The real new risk here, and what actually broke

Cross-process D3D11 resource sharing is the one genuinely new mechanism
this architecture needs (everything else in Milestone A was already
independently proven). Getting it working took three iterations, each
isolating a different, unrelated failure:

1. **DXGI's named-resource API doesn't work on this machine.**
   `IDXGIResource1::CreateSharedHandle(..., name, ...)` /
   `ID3D11Device1::OpenSharedResourceByName(name, ...)` -- the D3D11.1
   convenience API for opening a shared resource by string name instead of
   a raw handle -- consistently failed with `E_INVALIDARG` on the open
   side. A same-process self-test (create a texture, name it, immediately
   try to reopen it by that same name in the *same* process) failed
   identically, which conclusively ruled out cross-process, cross-bitness,
   session, and privilege-level causes -- something about this driver/OS
   combination just doesn't implement the named-lookup path correctly (a
   known historically spotty area outside pure first-party combos). An
   earlier theory that a `"Local\"`-style backslash prefix in the name was
   the problem was also tested and disproven (a name with no backslash at
   all failed exactly the same way).
2. **Fix: switch to explicit `DuplicateHandle`, not name lookup.** Instead
   of naming the shared handle, the producer now creates it unnamed, then
   publishes `{its own PID, the raw HANDLE value}` through a plain Win32
   file-mapping (`CreateFileMappingW`/`MapViewOfFile` -- an entirely
   different, much older OS subsystem than DXGI's own naming, and not
   observed to share the same problem). The consumer reads that block,
   calls `OpenProcess(PROCESS_DUP_HANDLE, ...)` on the published PID, then
   `DuplicateHandle()`s the raw value into its own process before opening
   it. `DuplicateHandle` succeeded immediately -- confirming the
   underlying cross-process/cross-bitness handle-passing mechanism itself
   was never the problem, only DXGI's name-resolution layer specifically.
3. **Still failed once more: wrong open API for the handle type.** After
   switching to `DuplicateHandle`, the open call still failed with
   `E_INVALIDARG` -- because it used the legacy, non-"1"
   `ID3D11Device::OpenSharedResource`, which only understands old-style
   `D3D11_RESOURCE_MISC_SHARED` handles. This project's textures use
   `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` (required alongside
   `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` for keyed-mutex sync), which
   needs the newer `ID3D11Device1::OpenSharedResource1` instead. Switching
   to that fixed it immediately -- confirmed live, in-headset: the
   producer's animated red/green (left) and blue/green (right) pulse
   showed up correctly, centered, replacing the static fallback colors.

### Current state of the copy itself

The blit from producer texture to OpenXR swapchain image is a plain
`CopySubresourceRegion` (clipped to the smaller of source/destination
per-axis, centered in the destination) -- no scaling. Since the synthetic
producer's texture (1920x1080) doesn't match the companion's per-eye
swapchain size (2528x2704 on this system), this correctly shows as a
centered, unscaled letterboxed image, confirmed visually. A shader-based
scaling blit is deferred to Milestone C, where it's actually needed (the
game's real backbuffer resolution won't match either).

## What's proven, end to end

- 64-bit OpenXR against this SteamVR install: solid.
- Cross-process, cross-bitness (32-bit producer -> 64-bit consumer) D3D11
  shared-texture handoff with keyed-mutex frame-to-frame synchronization:
  solid, once using the correct handle-duplication + `OpenSharedResource1`
  combination.
- The full pipeline end-to-end, confirmed live in-headset by the user.

## Not yet done (Milestone C)

- Replace the synthetic producer with the real proxy DLL, feeding the
  game's actual backbuffer (left eye) and Phase 2C's stereo duplicate
  (right eye) instead of a solid-color test pattern.
- Proper scaling (not just clip-and-center) from the game's real
  resolution to the OpenXR-recommended per-eye size -- likely a small
  full-screen-triangle shader blit rather than `CopySubresourceRegion`.
- Have the proxy DLL launch `mohwvr_companion.exe` itself via
  `CreateProcess` (as a background-thread-deferred step, consistent with
  every other heavy/IPC init in this project) instead of a human running
  it -- this also naturally resolves the session-matching requirement from
  Milestone A, and gives the proxy DLL a process handle it can
  `DuplicateHandle` into directly, without needing the PID-lookup dance
  the standalone test harnesses used.
- Head-pose data flowing back from the companion to the mod (shared
  memory), to actually drive the game's camera -- Phase 2D's original
  scope, separate from this connectivity work.
