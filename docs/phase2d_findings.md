# Phase 2D findings: OpenVR integration blocked by a `VR_Init()` hang

## What was built

`hooks/openvr_session.cpp/h`: initializes OpenVR (`vr::VR_IsHmdPresent()` ->
`vr::VR_Init()` -> `vr::VRCompositor()`) on its own background thread
(consistent with the Phase 2A lesson about not doing IPC/heavy init
synchronously inside a hooked call), and submits the game's real backbuffer
(left eye) plus Phase 2C's offscreen duplicate (right eye) to the
compositor every `Present`. Vendored the 32-bit (`win32`) OpenVR headers/
import lib/DLL (the originally-vendored ones were x64 -- MOHW.exe is
32-bit, caught immediately by a linker machine-type-mismatch warning).

## The hang

Live test (SteamVR + Quest running, confirmed via `vrserver`/`vrcompositor`/
`vrmonitor` processes and `vr::VR_IsHmdPresent()` returning true quickly):
`vr::VR_Init()` never returned, across an ~89 second session, though the
game itself remained fully responsive and shut down cleanly (the hang is
isolated to our own background thread, does not affect the game).

## Isolating the cause: standalone (non-injected) test

To rule out anything about being inside an injected/hooked process,
built `investigation/test_openvr_standalone.cpp` -- an ordinary, un-injected
32-bit console app that does nothing but call `VR_IsHmdPresent()`/
`VR_Init()`/`VRCompositor()` directly. Result: **the same hang**.
`VR_IsHmdPresent()` returns immediately; `VR_Init()` never returns (killed
after a 30s timeout).

**This conclusively rules out our injection/hooking as the cause.** It's a
fundamental incompatibility between this machine's current SteamVR install
and 32-bit client processes -- not a bug in the mod. Since MOHW.exe is
inherently 32-bit (confirmed via its PE header), this is a real blocker for
Phase 2D on this specific SteamVR + headset configuration, not something
more code on our end can work around.

## Root cause (user-identified)

Current SteamVR has moved to being **OpenXR-exclusive** and no longer
supports legacy 32-bit OpenVR client applications. That fully explains the
hang: `VR_Init()`'s legacy 32-bit OpenVR IPC path has nothing functioning
on the other end to complete the handshake with on this SteamVR version,
so it blocks forever rather than failing fast with a clean error.

Since MOHW.exe is inherently 32-bit, **OpenVR is no longer a viable runtime
choice for this mod** on current SteamVR, regardless of anything in our own
code -- this isn't a bug to fix, it's a dead API surface for this target.

## Path forward: switch to OpenXR

The plan originally picked OpenVR over OpenXR specifically for simpler
session/swapchain negotiation (`docs`/plan rationale: "injecting OpenXR into
a 2012 DX11 game that never used it is more involved"). That trade-off no
longer holds now that OpenVR's 32-bit path is dead weight -- OpenXR is not
just the more-modern option now, it's the only remaining option for a
32-bit game against current-generation runtimes (SteamVR's own OpenXR
runtime, Meta's OpenXR runtime for Quest/Link, etc. all still support
32-bit loaders since the Khronos OpenXR loader itself ships 32-bit builds).

Not yet started: this requires re-scoping Phase 2D's implementation
approach (OpenXR session creation, swapchain image negotiation instead of
directly wrapping our own D3D11 textures, `xrWaitFrame`/`xrBeginFrame`/
`xrEndFrame` frame loop, per-eye view/projection queries via
`xrLocateViews`) -- a genuinely different, more involved integration than
the OpenVR `Submit()` call this file originally described, not a small
patch on top of it.

## OpenXR built: instance/system/session milestone

Built `hooks/openxr_session.h/cpp` (instance -> system -> graphics
requirements -> session creation, on a background thread, using the game's
real `ID3D11Device` obtained via `IDXGISwapChain::GetDevice` on the first
`Present` call) and vendored the 32-bit OpenXR loader (`third_party/openxr`,
via the official `OpenXR.Loader` NuGet package -- a plain zip, no NuGet
client needed). Wired into `present_hook.cpp`; `openvr_session.h/cpp`
marked abandoned but left in the tree, same treatment as
`framerender_hook`/`commandlist_hook`.

**Pre-flight standalone check (learned from the OpenVR surprise -- always
verify outside the game first):** `investigation/test_openxr_standalone.cpp`
calls just `xrCreateInstance`/`xrGetSystem`, no device/session needed.
Result: no hang this time, but a fast, specific error --
`XR_ERROR_RUNTIME_UNAVAILABLE` (-51).

**Root cause, found via the Windows OpenXR runtime registry keys:**
- 64-bit active runtime (`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`):
  `steamxr_win64.json` (SteamVR).
- 32-bit active runtime (`HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime`):
  `...\Meta Horizon\Support\oculus-runtime\oculus_openxr_32.json` -- **not
  SteamVR**. SteamVR does not ship a 32-bit manifest at all
  (`steamxr_win32.json` doesn't exist in the SteamVR install dir).
- The Meta runtime's manifest and referenced library
  (`LibOVRRTImpl32_1.dll`) both exist on disk, but no Oculus/Meta runtime
  service process was running at test time -- the headset is currently
  connected through SteamVR, which doesn't activate the Meta runtime.

So a working 32-bit OpenXR runtime *is* available on this machine in
principle (Meta's), it's just not the active session type when connecting
via SteamVR. Needs the user's input on which connection mode to test
through, since that's an environment/workflow choice, not something fixable
in the mod's own code.

## Tried: SteamVR's own bundled 32-bit openvr_api.dll -- same hang

Confirmed the client-DLL-version hypothesis was worth ruling out precisely:
`C:\Steam\steamapps\common\SteamVR\bin\win32\openvr_api.dll` is a genuinely
different file (different size, different MD5) from the one vendored from
the official `ValveSoftware/openvr` GitHub repo. Tested it directly via the
same standalone harness. Result: identical hang in `vr::VR_Init()` (killed
by a 30s timeout), even though `VR_IsHmdPresent()` still returns true
quickly. This confirms the hang is not a client-DLL-version/build
mismatch -- it's `vrserver.exe` (SteamVR's own runtime process) not
completing the legacy 32-bit handshake, regardless of which exactly-matched
client library talks to it. Rules out "try a different/older client DLL"
as a fix entirely.

## Tried: OpenComposite (OpenVR-to-OpenXR shim) -- same root cause, no bypass

User found [OpenComposite](https://gitlab.com/znixian/OpenOVR) (referenced
from a [Proton feature request](https://github.com/ValveSoftware/Proton/issues/6808)):
a drop-in `openvr_api.dll` replacement that reimplements the OpenVR API by
forwarding calls to OpenXR. Idea: swap it in for the real SteamVR
`openvr_api.dll` and reuse the OpenVR integration code already built
(`hooks/openvr_session.cpp`), letting OpenComposite handle the OpenXR
session/swapchain complexity instead of us writing it directly.

Downloaded their official 32-bit Windows build (from the project's own CI
at `opencomposite.znix.xyz`, confirmed a valid 32-bit PE DLL) and tested it
through `investigation/test_openvr_standalone.cpp` (same safe, non-injected
harness used throughout this investigation). Result: fast, clean failure --
`vr::VR_IsHmdPresent()` returns false quickly (no hang this time), because
OpenComposite's internal `xrCreateInstance` call fails with the *same*
underlying cause already diagnosed: no active 32-bit OpenXR runtime while
connected via SteamVR. OpenComposite is a legitimate, well-built shim, but
it still goes through the standard OpenXR loader/runtime chain -- it
doesn't do any 32-to-64-bit IPC bridging that would route around the
missing runtime, so it hits the identical wall our own direct OpenXR
attempt did. Confirms the root cause more precisely without touching the
game, but doesn't change it: the fix is still a connection-mode question
(native Meta Link vs. SteamVR), not something fixable in the mod's code
regardless of which OpenVR/OpenXR wrapper is used.

## What's unaffected

Everything built in Phases 0-2C remains fully functional and unrelated to
this blocker: injection, all hooks, offset resolution, the constant-buffer
identification, and the draw-duplication mechanism. `IsOpenVRActive()`
correctly stays false when `InitOpenVR()` doesn't complete, and
`SubmitStereoFrame` safely no-ops -- the proxy DLL does not crash or hang
the game itself in this state, confirmed via multiple clean-shutdown live
tests.
