# Status

Read this first, every session, before touching code — then check the
current milestone in `ROADMAP.md`. Update both before signing off; a session
that ends without a `STATUS.md` update is an incomplete handoff.

**Before doing anything else this session: read "Player facing/yaw + input
pipeline" below and `project_mohw_yaw_facing_investigation.md` memory.**
**The live-debugging investigation is CONCLUDED. Position AND rotation are
both fully solved** (`CharacterPhysicsEntity+0xE0` / `AimingController+0xC`
and `+0x10`, both absolute, both freeze-confirmed). `hooks/gameplay_input_hook.cpp`
is disabled (crashed on its one live test, see Known issues) and superseded
by this finding — do not resume it. **Active work has shifted from
discovery to implementation: build the actual hook.** Plan: hook
`FUN_009DDA90`'s entry (hands us `AimingController` as `this` for free, no
chain-walking needed), call the original function, then overwrite
`AimingController+0xC`/`+0x10` with HMD-derived yaw/pitch afterward —
same architecture as the proven `CommitViewTransform` render-side hook.
A downstream-consumer check found `AimingController`'s heading gets
broadcast to multiple other objects each tick (a good sign — one correct
write updates every reader), and a one-shot-force test artifact (releasing
a forced value lerps back to a pre-test baseline) is judged unlikely to
affect a continuous-overwrite hook and isn't being chased further unless
the real hook shows fighting/jitter. See "Player facing/yaw + input
pipeline" below for the full chain and re-derivation technique.

## Player facing/yaw + input pipeline (active investigation, current state)

Multi-session x64dbg live-debugging effort (no code changes yet) to find a
genuine, non-drifting write target for gameplay-side character facing —
the rotation equivalent of the already-solved position hook below. Full
session-by-session detail lives in
`project_mohw_yaw_facing_investigation.md` memory; this section is the
current-state summary only.

**Position — fully solved, proven pattern:**
```
ClientSoldierEntity+0x1C0/+0x1C4 (raw movement input, NOT authoritative itself)
  -> FUN_00972840 -> FUN_0049C880 (CharacterPhysicsEntity's per-tick update)
    -> ... -> FUN_019C1070
      -> CharacterPhysicsEntity+0xE0 = AUTHORITATIVE ABSOLUTE POSITION
         (freeze-confirmed; this is the target shape for rotation too)
```
(`FUN_0049C880`'s object — previously called "the tracker" in older notes —
is confirmed to be `CharacterPhysicsEntity` itself, cross-verified by
matching instance address across independent traces. Use
`CharacterPhysicsEntity` going forward, not "tracker".)

**Look/rotation — SOLVED. Full pipeline, raw input down to the absolute
heading, now traced end-to-end:**
```
bound physical device objects (per-device virtual calls — literal
OS/DirectInput-level, never traced further, not needed)
  -> action-value table / master axis resolver (FUN_00A8E260) -> SetAxis
    (FUN_008055A0 @ 0x00805619) -> CharacterPhysicsEntity's per-entity axis
    array (movement=idx0, yaw=idx4, pitch=idx5 -- rate-shaped, -1..1,
    freeze-confirmed causal but NOT the absolute heading itself)
      -> cached into AimingController+0x4C/+0x50 (yaw/pitch rate input) inside
         FUN_009DDA90, the weapon-aim/aim-assist per-tick update function
        -> aim-assist-weighted blend/integration at that function's tail
           (`[AimingController+0xC] += delta`, gated behind an aim-assist-
           eligibility check)
          -> AimingController+0xC / +0x10 = AUTHORITATIVE ABSOLUTE YAW/PITCH
             (radians, 0..~2pi) -- FREEZE-CONFIRMED: forcing this snapped
             facing and held it; real input pulled back TOWARD the forced
             value rather than away -- same signature that proved +0xE0.
```

**How to (re-)find `AimingController` each launch** (heap addresses are never
stable across launches — same rule as every other object in this
project):
```
ClientSoldierEntity (vtable pattern search B8 09 7A 02)
  -> +0xB0 (embedded sub-object, 37-slot vtable)
    -> +0x4A4 -> a companion/attachment-management object
      -> vtable-resolved (`mov eax,[ecx+0x430]`) -> AimingController
```
**Fast alternative for a fresh launch — the confirmed write instruction is
a stable CODE address:** breakpoint at `0x009DE8F1` (`movss [ebx+C],xmm0`,
the UNCONDITIONAL write, inside `FUN_009DDA90` — `ebx` = `this` =
`AimingController` throughout that whole function), log `ebx` on every hit.
(Two nearby conditional wraparound-correction rewrites also exist,
`0x009DE904`/`0x009DE921` — subtract/add a full turn if the value over/
underflows `0..2pi` — but those rarely fire; don't use them as the
breakpoint address, they were tried first and confirmed near-silent live.)
Fires for AI-held weapons too, so either cross-check against the entity
chain above once, or use the established "fires consistently while
actively turning" signature to spot the player's own weapon among the
hits.

**Closed, not chased further — the "lerp-back on release" caveat**:
releasing a one-shot sustained force on `AimingController+0xC` doesn't hold
or jump to "current real input" — it smoothly lerps back to whatever
direction was current *before* the freeze test started, meaning something
else may also read/correct this field from an even more upstream source
(same pattern as the already-closed `entity+0x140`/`entity+0x1C0`
rubber-banding fields). A downstream-consumer trace (read-breakpoint on the
field) found genuine new consumers this time — `AimingController`'s
heading gets broadcast-copied into multiple other objects per tick via a
loop at `0x00A1F903`-`0x00A1F929` — but deliberately stopped there: with
confirmation this field is already load-bearing for several downstream
systems, enumerating every consumer has diminishing returns, and the real
mod hook will continuously overwrite every frame (not force-once-release
like the test), which should sidestep the lerp-back mechanism entirely.
**Resume this only if the real hook shows visible fighting/jitter against
some other writer** — otherwise proceed straight to building it. `AimingController`'s
own class identity is confirmed too, via a vtable+trailing-string dump:
`"MOHW/Weapons/TuningProfile/AimingController/spec/AC_Default_s3SP"`. Full
technique/methodology notes (parameter-offset pitfalls, freeze-test
methodology, vtable identification, the many closed-out detours along the
way) are in the memory file.

## Current status (as of 2026-08-04)

**Stereo rendering pipeline is complete and stable — MRT G-buffer
duplication + SRV redirection are live, and a real recurring crash in that
path is found and fixed** (not just quieted). Companion Milestone C is
fully done. Depth-sense investigation found **two real, independent
contributors** to the earlier "no sense of depth" problem — **both now
fixed.**

**Fixed: FOV/IPD minification (2026-08-04).** The game's internal FOV was
scaled 2.4x for comfort, but the companion submits to the compositor at
the REAL headset FOV — that rendered-vs-submitted FOV mismatch minifies
the displayed image and compresses every depth cue, including stereo
disparity, which is why amplifying IPD alone (up to 8x, tested the
previous session) never helped. Made both FOV scale (F5/F6) and IPD scale
(F7/F8, now uncapped, ±1.0x steps) live-adjustable; lowering FOV first and
re-testing IPD produced a real, perceptible depth difference for the first
time.

**Baseline values, confirmed and set as the new compile-time defaults
(2026-08-04): FOV scale 2.2x (110° vertical, native is 50°) + IPD scale
3.5x.** Recorded here specifically so there's a known-good pair to reset
to later — `commit_view_transform_hook.cpp`'s `g_fovYScale` and
`draw_duplication_hook.cpp`'s `g_ipdScale`'s initial values were both
updated to match, so a fresh launch now starts at this baseline rather
than requiring hotkey retuning every session. Not claimed to be the final/
perfect tuning — just the confirmed-working reference point.

**Fixed: head position/translation tracking (2026-08-04).** Compared
against bioshock-vr's reference head-tracking code
(`https://github.com/mohamad-balouza/bioshock-vr`): their IPD handling is
the same simple lateral-offset technique already used here — not a gap —
but their camera additionally applies real head **position** every frame,
reprojected through a recenter-yaw → game-yaw rotation. This project's
code published and even logged `HeadPoseBlock.positionX/Y/Z` but never
consumed it. Implemented in `commit_view_transform_hook.cpp`'s
`ApplyHeadPoseTransform` (renamed from `ApplyHeadPoseRotation`), reusing
the same recenter + local-frame-reprojection technique already proven for
rotation. **Correct on the first live test** — no sign/frame-of-reference
fixes needed, unlike rotation's three-bug history. User: "Perfect first
try head tracking actually."

Other candidates already ruled out during this investigation: toggling
full G-buffer stereo (MRT + SRV redirection) on/off made no perceptible
difference either way (F11 hotkey); eye routing itself was independently
confirmed correct via a temporary red(left)/blue(right) debug marker
(since removed, not shipped) — ruling out "both eyes see the same
texture."

## What's working right now

- Injection: proxy `dxgi.dll`, deferred init, MinHook installed across
  multiple hook modules (`Present`, `ConstantBuffer`, `DrawDuplication`,
  `EngineFunction`/view-constants, `CommitViewTransform`) —
  `UpdateMatrices` hook intentionally **not installed**, see Known issues
- `Present`/`ResizeBuffers` hooked via the dummy-device vtable-read
  technique
- Companion process (`mohwvr_companion.exe`) launches itself from the
  `Present` hook and receives the real backbuffer + duplicated-eye texture
  every frame via `DuplicateHandle` + `OpenSharedResource1`
- Companion holds a live 64-bit OpenXR session against SteamVR, confirmed
  in-headset
- Head pose (**orientation and, as of 2026-08-04, position**) flows
  companion → proxy_dll over a lock-free shared-memory channel
  (`HeadPoseBlock`)
- **One rotation+position source now:** the original per-draw
  `crViewProjMatrix`/`cameraPos` constant-buffer patch
  (`draw_duplication_hook.cpp`) no longer touches rotation at all —
  `ApplyHeadPoseToViewMatrix` was renamed to `ApplyIpdOffsetToViewMatrix`
  and stripped of its recenter/delta-quaternion rotation logic (dead code
  now that the hook below owns rotation exclusively); it only computes the
  per-eye IPD lateral offset (baseline 3.5x, runtime-adjustable via F7/F8,
  see below), and no longer reads `HeadPoseBlock` at all
  - The engine-level hook at `OFFSET_COMMITVIEWTRANSFORM`
    (`hooks/commit_view_transform_hook.cpp`, `ApplyHeadPoseTransform` —
    renamed 2026-08-04 from `ApplyHeadPoseRotation` to reflect the wider
    scope) — patches the camera's world-space `LinearTransform` rotation
    AND, as of 2026-08-04, position once per tick, before the engine's own
    view/projection math runs. Rotation confirmed live with correct axes
    and proportional roll/tilt. **Position confirmed correct on first live
    test** (2026-08-04) — no sign/frame-of-reference fixes needed
- Real per-eye camera patch: `crViewProjMatrix` (translation-free) +
  `cameraPos`, on the correct buffer (352-byte `viewConstants`, VS slot 2)
- Both eyes patched, GPU-instanced static geometry included
- **Aspect-correct "cover" scaling blit** (`companion/main.cpp`,
  `ComputeCoverUvScaleOffset`) — uniformly scales to fill the destination,
  cropping the excess axis, no shear/letterboxing
- **Real per-eye FOV** pulled from `xrLocateViews` (symmetric average, +1.12x
  margin) instead of a hardcoded 90° — black borders now nearly unnoticeable
- **Game's own internal FOV scaled** (`commit_view_transform_hook.cpp`'s
  `ApplyFovScale`, baseline 2.2x, runtime-adjustable via F5/F6) for depth
  perception and comfort — does not affect the view-model (arms/weapon),
  which reads FOV from an unidentified separate source
- **MRT-aware right-eye G-buffer target pool** (`hooks/stereo_render.h/.cpp`)
  — format-keyed pool handling the deferred renderer's multiple simultaneous
  RTV shapes (confirmed live: up to 4 simultaneous targets), instead of one
  shared target overwriting itself
- **Shader-resource-view redirection** (`Hooked_PSSetShaderResources`) — the
  later lighting/shading pass now reads back the *right*-eye G-buffer
  textures it wrote, not the game's original left-eye ones
- **A real, recurring live crash in the SRV-redirect path is found and
  fixed** (not the same as an earlier, incomplete fix) — see Known issues
  for what was actually wrong; `PurgeStaleSrvRedirectsIfGenerationChanged()`
  is now called twice per draw, closing a one-draw race window
- **`sdk/logging.h` rewritten** — was open+append+close (real
  CreateFile-class syscalls) on every single log call, on the render
  thread; now keeps one file handle open per log file for the process
  lifetime (mutex-guarded, `fflush()` after every write so crash forensics
  still work). Independently worth keeping regardless of its role in the
  crash investigation
- **Persistent settings system** (`sdk/settings.h/.cpp`, new 2026-08-04) —
  plain-text `mohwvr_settings.ini` next to the DLL, loaded at startup,
  written immediately on every hotkey change so values survive a relaunch.
  Modeled loosely on bioshock-vr's `vrpreset.ini`. Current fields:
  `FovScale`, `IpdScale` (both default to the confirmed baseline),
  `HeadAimEnabled`, `HeadAimSensitivity` (default OFF/1.0 — see below)
- **Runtime debug hotkeys**, all persisted via the settings system above,
  polled from `Hooked_Present`: **F11** toggles full G-buffer stereo
  (`draw_duplication_hook.h`) live; **F7/F8** step IPD scale (±1.0x, no
  practical cap); **F5/F6** step FOV scale (`commit_view_transform_hook.h`,
  0.2x steps); **F12** toggles head-driven aim/movement
  (`gameplay_input_hook.h`, see below); **F1/F2** step its sensitivity
- **Head-driven aim/movement (new 2026-08-04, built, live-test pending) —
  see Known issues for the real crash-risk caveat before testing.**
  `hooks/gameplay_input_hook.cpp` hooks a new address
  (`OFFSET_INPUTACCUMULATOR`) that accumulates each frame's gamepad/mouse
  look/move deltas, and additively injects the HMD's frame-to-frame
  rotation on top (a genuinely different computation from the render
  hooks' delta-since-recenter scheme — this needs an incremental, per-
  frame delta). Default **disabled** until explicitly enabled via F12

## Known issues / deferred

- **Head-driven aim/movement (`hooks/gameplay_input_hook.cpp`) was
  live-tested (2026-08-04) and crashed** — confirmed the exact
  signature-mismatch risk flagged when it was built (`OFFSET_INPUTACCUMULATOR`
  takes at least one stack parameter beyond the assumed zero; MSVC `/RTC1`'s
  "ESP not properly saved" error). Fixed the immediate crash by commenting
  out `InstallGameplayInputHook()` in `proxy_dll/dllmain.cpp` (hook is
  DISABLED, not deleted). **Do not just fix the signature and re-enable —**
  a separate design review the same day concluded the whole approach
  (additively injecting a per-frame delta into a shared, per-frame-reset
  accumulator) drifts over time and needs replacing with an absolute-write
  strategy, mirroring `CommitViewTransform`'s own architecture. That
  requires finding a persistent, non-accumulating gameplay-side object to
  write into — an extensive live-investigation effort (GameContext's
  14-field subsystem registry, the XInput accumulator/action-table
  consumer graph for both look and move axes, and a position-value memory
  search from the known camera transform) has been tried and fully
  exhausted without finding it; see `project_mohw_frostbite.md` memory for
  the complete trace. Next planned technique: a differential/delta memory
  scan (snapshot before/after a known movement, filter by matching delta
  rather than absolute value) to work around a likely constant offset
  between the camera and the real player-pawn anchor point.
- **The SRV-redirect crash's real root cause (2026-08-03, this session):**
  the earlier "fix" (a generation-counter purge at the top of
  `PrepareRightEyeTarget`) was real but incomplete — it checks the
  generation as of the *end of the previous draw*, so it can't see a bump
  caused by *that same draw's own* `EnsureRightEyeTargets`/
  `EnsureRightEyeMrtTargets` call, which runs later in that same function.
  During the observed format-thrashing burst (10 single-target
  recreations within ~20ms, happening during the launch→main-menu
  intro-movie transition, not a level load as first assumed), that left a
  one-draw window where some *other* already-tracked resource's stale SRV
  pointer would survive into the redirect lookup and get dereferenced — a
  genuine, 100%-reproducible use-after-free, confirmed via bisection
  (disabling SRV redirection entirely made the crash stop). **Fixed** by
  calling the purge function a second time, right before the redirect
  lookup in `DuplicateDraw`, after that draw's own recreation has already
  happened. Not yet stress-tested beyond the sessions since the fix landed
  — no recurrence so far.
- **Shader compatibility issue found under full G-buffer SRV
  redirection:** rain (and possibly other shaders not yet identified)
  render incorrectly. Not yet investigated — plausibly a shader doing
  something format/precision-sensitive (soft particles, refraction/depth
  sampling) that doesn't tolerate being handed the right eye's redirected
  G-buffer textures. Worth deciding whether to debug it or scope
  redirection to only known-safe channels, once the depth-cue question
  above is resolved (no point tuning this further if the underlying
  approach is dropped or changed).
- **Performance not benchmarked.** User reported "not great" performance
  with the full pipeline active; no profiling done yet to attribute it
  between MRT pool overhead, SRV redirect rebind churn, or something else.
- **`InstallUpdateMatricesHook()` is intentionally left uninstalled**
  (`proxy_dll/dllmain.cpp:112`, call commented out) — **not related to the
  rotation-patch decision above**, despite both being about camera
  hooking. `updatematrices_hook.cpp` is a read-only diagnostic hook (logs
  which `RenderView` is being updated, thread IDs, does empirical struct-
  layout scanning) — it never patched or injected anything. It was the
  investigative tool used to originally help locate
  `OFFSET_COMMITVIEWTRANSFORM` and to confirm `GameRenderer::Singleton()+0x50`
  is the real camera (a fact other code now relies on as already
  established). Its job is done; nothing currently working depends on it
  being installed. Decided (2026-08-03) to leave it disabled rather than
  re-enable it for no active purpose — the file is kept in the tree since
  its layout-scanning technique may be reusable for the planned AOB/
  offset-hardening work later.
- **View-model (arms/weapon) FOV doesn't follow the 2.4x world FOV scale.**
  Confirmed it reads from a different source/camera than the main world
  view; that source is still unidentified.
- **`mohw_offsets.h`'s static RVAs are only guarded by a whole-image-size
  check**, not re-verified per-offset. A same-size hotfix patch to the game
  is a real, unaddressed risk (see ROADMAP "Next — harden before
  extending").
- `OFFSET_DBGRENDRAWLINE` / `OFFSET_DBGRENDRAWSPHERE` unconfirmed (two
  disagreeing sources) — not on the critical path, don't rely on them yet.
- `OFFSET_TOSCREENFUNCTION_UNVERIFIED` / `OFFSET_SNOWROLLER_UNVERIFIED` —
  carried over from a mismatched-build header, do not dereference.
- **Whether Fullscreen vs. Windowed/Borderless (`GstRender.FullscreenEnabled`
  in the game's own `PROF_SAVE_profile`, NOT `mohwvr_settings.ini`) actually
  affects performance is unconfirmed (2026-08-23).** Switched away from
  Fullscreen and set an explicit 120Hz refresh rate in the same session,
  and screen tearing went away — but those two changes were never isolated
  from each other, so it's not known which one (or both) mattered. See
  README.md's "Open setup questions" for the user-facing note.
- **`HeadAimEnabled` fights native mouse-look (2026-08-23, confirmed
  live).** `hooks/aiming_controller_hook.cpp`'s `ApplyHeadAim` unconditionally
  overwrites yaw/pitch from HMD orientation every gameplay tick, discarding
  whatever the mouse just wrote that same tick — not a bug exactly, just no
  blending mechanism exists between the two input sources yet. See
  `project_mohw_alternating_eye_investigation.md` memory for the full
  diagnosis.
- **This whole file predates 2026-08-23's session (alternating-eye
  rendering, the 30Hz-sim-tick jitter root-cause + fix, rotation smoothing)
  — treat everything above this point as historical until a proper refresh
  happens.** See `project_mohw_alternating_eye_investigation.md` and
  `project_mohw_alternating_eye_baseline.md` memory for that session's
  actual current-state summary in the meantime.

## Next priority items

1. **Build the actual rotation hook.** Investigation concluded — target is
   `AimingController+0xC`/`+0x10` (freeze-confirmed absolute yaw/pitch,
   radians). Plan: MinHook this-call trampoline on `FUN_009DDA90`'s entry
   (gets `AimingController` as `this` for free), call original, then
   overwrite `+0xC`/`+0x10` from HMD yaw/pitch — same architecture as
   `CommitViewTransform`. See "Player facing/yaw + input pipeline" above
   for the exact writer instruction and full chain. Full detail in
   `project_mohw_yaw_facing_investigation.md` memory.
2. Investigate the rain-shader (and possibly other shader) compatibility
   issue under full G-buffer SRV redirection.
3. Profile performance of the MRT + SRV redirection pipeline.
4. AOB/string-xref address-scan pass for the CPU-side static offsets
   (harden against future game patches).
5. D3D11 entry-point audit (`DrawAuto`/`ExecuteIndirect`/compute
   `Dispatch`) — confirm no other silent blind spot like the missed
   `DrawInstanced` bug.
6. Identify the view-model's real FOV source, if it becomes a bigger
   comfort issue than it already is.
7. **Test whether Fullscreen vs. Windowed/Borderless mode actually affects
   performance**, isolated from the refresh-rate change made alongside it
   (2026-08-23) — currently confounded, not confirmed either way.
8. **Review additional `PROF_SAVE_profile` `GstRender.*` settings** (user
   has specific ones in mind to go through together) for further
   performance gains — resolution scale, quality tiers, etc.

## Session log

- **2026-08-04 through 2026-08-07** — Early stage of the player facing/yaw
  investigation (`gameplay_input_hook.cpp` live-tested and crashed as
  flagged, now disabled; searched `GameContext`'s subsystem registry, XInput
  move-axis consumers, and an exact-value memory search seeded from camera
  position — all closed without finding the target; found and vtable-
  confirmed `ClientSoldierEntity` via backward-tracing from
  `CommitViewTransform`; mapped its 8 embedded sub-components; several
  structurally-promising rotation candidates on the entity — a `+0x180`
  sin/cos pair, a `+0x330`/`+0x4B0` `LinearTransform` — all freeze-tested
  and closed out as cosmetic/animation, the `+0x4B0` one traced all the way
  to its source and confirmed to be an IK-driven camera-tracking bone, not
  gameplay state). Superseded by later sessions below — this era's detail
  is preserved in `project_mohw_frostbite.md` and
  `project_mohw_yaw_facing_investigation.md` memory if ever needed again.

Reconstructed retroactively from `docs/*_findings.md` and file history when
this file was created (2026-07-29) — exact session boundaries before that
date are approximate. Going forward, append one dated entry here at the end
of every session instead of relying on file timestamps.

- **2026-07-25** — Phase 0 (injection) and Phase 1 (native dual-view path)
  done; Phase 1 concluded a dead end (`secondaryStreamingView` is an
  asset-streaming look-ahead camera, not a second eye). Phase 2B's first
  buffer candidate identified (80-byte, VS slot 0) but flagged provisional.
  Phase 2C's high-level "call the render function twice" attempt abandoned
  after two identical crashes (a `__thiscall` stack-parameter mismatch) —
  the real per-frame call site was traced anyway, kept as a lead for if a
  disassembler ever becomes available.
- **2026-07-26** — Phase 2C's low-level draw-call duplication built and
  proven safe (render-target-size filtering, 261 duplicated draws/session,
  no crashes across multiple sessions). OpenVR and in-process OpenXR
  integration work begun.
- **2026-07-28** — Phase 2D concluded both OpenVR and 32-bit OpenXR are
  dead ends on this SteamVR install (confirmed via standalone,
  non-injected harnesses, ruling out our own hooks as the cause). Decision
  made to pivot to a 64-bit companion process. Companion Milestones A
  (standalone OpenXR session) and B (cross-process shared-texture handoff)
  both built and confirmed live in-headset the same day.
- **2026-07-29** — Motion-controls research pass completed (design only,
  no code — see `docs/motion_controls_research.md`). Companion Milestone C
  wired: real game frames + on-demand companion launch, replacing the
  synthetic test producer. Major breakthrough: root-caused why the Phase 2B
  buffer patch had zero visible effect (wrong buffer entirely — compiler-
  marked `[unused]` fields) via 3Dmigoto shader disassembly, found and
  patched the real `viewConstants` buffer, then fixed four follow-on bugs
  (camera-position source, one-eye-only patching, inverted rotation
  direction, missed instanced draws). Head-tracked, IPD-correct stereo now
  working end-to-end, live in-headset, for the real game. Full
  investigation write-up produced: `docs/stereo_view_matrix_investigation.md`,
  with a matching visual/diagram Claude Artifact. Paused deliberately after
  this with frustum culling lag chosen as the explicit next task.
- **2026-08-03 (long session, two parts)** — **Part 1:** set out to fix
  frustum culling lag by finding the engine's genuine per-frame camera-
  input path. Traced the game's XInput/mouse input pipeline in full detail
  and confirmed it's unrelated to rendering (dead end for this task, kept
  for future input work). Pivoted to tracing backward from the already-
  hooked view-constants upload, found `OFFSET_COMMITVIEWTRANSFORM` — an
  engine function that commits the camera's world-space transform once
  per tick, upstream of render math. Built `commit_view_transform_hook.cpp`,
  wired real HMD rotation, fixed three live bugs (double-rotation risk,
  full-axis inversion, local-vs-world frame composition) — confirmed
  correct on all axes. Closed Companion Milestone C's remaining gaps:
  aspect-correct scaling blit + real per-eye FOV matching, and multi-
  channel G-buffer stereo via an MRT pool + SRV redirection (fixed a
  use-after-free crash found on first live test; believed fixed at the
  time). Scaled the game's own FOV to 2.4x for comfort.
  **Part 2 (continued the same day):** the SRV-redirect crash recurred,
  100% reproducible during the launch→main-menu intro-movie transition,
  identical fault every time. Logging-volume reduction didn't fix it;
  rewrote `sdk/logging.h` to stop paying open/close cost per call (kept
  regardless of outcome) and re-enabled full diagnostics, which showed a
  dense resource-format-thrashing burst right before each crash. A
  bisection test (disabling SRV redirection entirely) proved the bug lived
  there specifically, and re-auditing the purge logic found the real gap:
  the generation-check only ran at the *top* of `PrepareRightEyeTarget`,
  missing a bump from that same draw's own recreation later in the same
  function — fixed by checking again right before the redirect lookup.
  Added F11 (toggle full G-buffer stereo)/F7/F8 (IPD scale) runtime debug
  hotkeys to make future A/B comparisons possible without rebuilding.
  With the crash genuinely fixed, live-tested whether the stereo/depth
  work actually produces a sense of 3D: neither toggling full G-buffer
  stereo nor amplifying IPD up to 8x made a perceptible difference. A red/
  blue debug marker test confirmed eye routing itself is correct, ruling
  out a plumbing bug. Pulled bioshock-vr's reference head-tracking code
  for comparison and found the likely real answer: their camera applies
  head **position** (translation), reprojected through recenter-yaw/game-
  yaw; MOHW's hooks only ever apply orientation — `HeadPoseBlock`'s
  position fields are published and logged but never consumed by any
  render patch. Session ended with IPD reset to 1.0x (realistic) and an
  explicit two-step plan for next session: implement head-position
  tracking first as an immersion test, then tackle making gameplay aim/
  forward-direction actually follow head tracking (currently render-only).
  **Part 3 (housekeeping, same day):** decided the two-patch rotation
  question — retired the CB-patch's rotation entirely rather than keeping
  it as a disabled-but-present code path, since `CommitViewTransform` is
  the sole confirmed-correct rotation source now. Removed the recenter/
  delta-quaternion logic from `draw_duplication_hook.cpp`'s per-eye
  function, renamed it `ApplyIpdOffsetToViewMatrix` to match its narrowed
  scope (IPD offset only, no head-pose dependency at all anymore), and
  dropped the now-unused `companion_bridge.h` include. Also clarified a
  mistaken link the user drew between that decision and
  `InstallUpdateMatricesHook()`: the two are unrelated — that hook is a
  read-only diagnostic tool (never patched anything) used to originally
  help find `OFFSET_COMMITVIEWTRANSFORM`, and its job is already done.
  Decided to leave it uninstalled permanently rather than re-enable it,
  documented as an intentional decision rather than debt.
- **2026-08-04 — FOV/IPD minification confirmed, depth-of-field
  researched-and-deferred.** Before starting head-position tracking, the
  user raised two points. (1) Whether the 2.4x game-internal FOV scale
  could itself be suppressing depth, independent of IPD amount — reasoned
  through the projection math (companion submits at the real headset FOV,
  not the scaled render FOV, so the mismatch minifies the image and
  compresses every depth cue including disparity) and confirmed correct: a
  live test lowering FOV scale (new F5/F6 hotkeys) then re-adjusting IPD
  (F7/F8, widened to uncapped/±1.0x steps per request) produced a real,
  perceptible depth difference for the first time. Added real degree-level
  FOV telemetry (`LogFovIfDue`, 2s-throttled) so the live FOV value is
  directly readable in the log instead of requiring a hand computation
  from the scale multiplier. (2) Shared a focus-adaptive depth-of-field
  technique (depth-aware bilateral blur + forced in-focus range) as a
  possible future immersion addition — assessed and logged in
  `docs/ROADMAP.md` as researched but deliberately deferred until the
  fundamentals (this FOV/IPD work, head-position tracking) are settled.
  Session moves on to head-position tracking next, as planned.
- **2026-08-04 (continued) — head position/translation tracking
  implemented and confirmed correct on the first live test.** Extended
  `commit_view_transform_hook.cpp`'s rotation hook (renamed
  `ApplyHeadPoseRotation` → `ApplyHeadPoseTransform`) to also apply real
  head position, reusing the exact recenter-on-first-pose + local-frame-
  reprojection technique already proven for rotation rather than
  re-deriving bioshock-vr's raw yaw-trig approach — added a `VecSub`
  helper to `sdk/vr_math.h`, reused the existing 1-unit-=-1-meter world-
  scale assumption from the IPD code, and added throttled telemetry
  (`LogPositionIfDue`) logging raw/local/world deltas from the start.
  Unlike rotation's three-bug history, this needed **zero** sign/frame
  fixes — user: "Perfect first try head tracking actually." Both real
  contributors to the earlier "no sense of depth" problem (FOV/IPD
  minification and missing head translation) are now fixed.
- **2026-08-04 (continued) — FOV/IPD baseline recorded, forward-tracking
  design settled, settings system + head-driven aim/movement built.**
  Recorded the session's final settled FOV/IPD values (2.2x/3.5x) as the
  new compile-time defaults, specifically so there's a known-good reset
  point. Discussed the forward-tracking/aim task with the user's three
  concerns (predictable movement direction, controller-independent weapon
  aim eventually, player-adjustable settings for future hardware) —
  initially recommended deferring aim/movement work entirely until motion
  controllers exist, but the user clarified: only weapon aim specifically
  needs to wait for controllers; head-driven aim/movement is a practical
  interim step worth building now. Also discussed (and deferred, not
  started) an in-game settings-menu UI — Dear ImGui in the companion
  process was the recommended approach, but the user chose to resolve
  head-driven aim first. Built `sdk/settings.h/.cpp`, a persistent
  `mohwvr_settings.ini` (FOV/IPD scale, head-aim enable/sensitivity),
  replacing the FOV/IPD hotkeys' local atomics so values survive a
  relaunch. Built `hooks/gameplay_input_hook.cpp`, hooking a new address
  (`OFFSET_INPUTACCUMULATOR`, added to `sdk/mohw_offsets.h`) to additively
  inject the HMD's frame-to-frame rotation into the gameplay look
  accumulator, gated behind a new F12 toggle (default OFF) + F1/F2
  sensitivity hotkeys. **Explicitly flagged as this project's
  highest-signature-risk hook yet** (the target function's exact
  parameter count was never fully confirmed, unlike every other hooked
  function) — built, deployed, but **not yet live-tested** as of session
  end. Next session should start there.

- **2026-08-12 through 2026-08-16 — full trace of the input-binding-
  resolution and axis-write pipeline, converging on the pipeline
  documented above.** Consolidated summary; full session-by-session
  blow-by-blow (Run Trace captures, the generic action-value table
  discovery, the clone-vs-real-writer distinction, the master resolver,
  and the device-binding-list trace) lives in
  `project_mohw_yaw_facing_investigation.md` memory. Key milestones, in
  order: (1) live Run Trace ruled out an SSE matrix-transform lead and
  established Ghidra static disassembly is unreliable in this whole
  gameplay code region (likely anti-tamper) — treat x64dbg live
  reads/traces as ground truth here; (2) found the generic
  `GetActionValue`/`SetActionValue` table (`FUN_008cfe40`/`FUN_008cfe80`/
  `FUN_008cfed0`) and traced the real input-binding-resolution layer
  (`0xA89C23`) that writes into it; (3) traced a full downstream chain to
  a write on `CharacterPhysicsEntity+0x250`/`+0x258` (found via the
  vtable+trailing-string technique) — freeze-tested and **ruled out as
  non-authoritative**, along with several sibling candidates
  (`entity+0x178`/`+0x17C`, `FUN_0097CC50`'s branch, a stack-relative
  local) all closed as cosmetic/animation/debug-overlay dead ends; (4)
  **breakthrough**: a 4-way opposite-direction binary-dump diff (yaw-left
  vs. yaw-right, movement held constant) found a clean sign flip at
  `CharacterPhysicsEntity+0x10`/`+0x14`, freeze-confirmed as the real
  yaw/pitch input axes — but confirmed these are `-1..1` INPUT-shaped
  axes, not a persistent absolute heading, and confirmed via 5+ launches
  that their absolute offset from the entity's base is **not fixed**
  (seen at `+0x0`, `+0x570`, `+0x580`, `+0x590` depending on launch —
  never hardcode this offset, always re-derive from the stable write
  instruction); (5) a write breakpoint on the field initially only found
  bulk-clone copies (`FUN_0080CC00`/`FUN_0080EF50`, 237 hits, no direct
  writer) — corrected a source/destination mixup in that clone function
  along the way, then found the real, distinct writer, **`FUN_008055A0`**
  (`SetAxis`-style: clamps, bit-tests the axis index, writes via
  `movss [edi+esi*4],xmm0` at `0x00805619`); (6) traced `FUN_008055A0`'s
  caller to **`FUN_00A8E260`**, the master per-tick axis-resolver
  (loops all 74 axes via binding-hash lookups), then through its vtable
  `+0x10` call to per-axis lists of bound physical device objects
  (`FUN_00A8E4C0`, 16-byte binding records keyed by a global action
  code — reconciling the local per-entity axis numbering, movement=0/
  yaw=4/pitch=5, against the global table's 0/1/7/8); (7) ruled out the
  device objects themselves as value holders (clean 4-way-diff negative,
  descriptor/config data only) and found the resolver instead makes a
  separate companion-object virtual call, `FUN_00A895F0`, which resolves
  per-device values (picks largest magnitude) via `FUN_008CFE40`/
  `FUN_008CFE80`/`FUN_008CFED0` — **closing the loop back to the exact
  action-value table system documented since session 1 of this whole
  project.**
  Also this window: reconciled and superseded an earlier (2026-08-14)
  session's position-tracking work (recovered from a lost-context
  transcript search) — `ClientSoldierEntity+0x1C0`/`+0x1C4` is raw
  movement input (not authoritative), feeding `FUN_00972840` ->
  `FUN_0049C880` (`CharacterPhysicsEntity`'s own per-tick update,
  cross-confirmed the same object via matching instance address across
  independent traces) -> ... -> `FUN_019C1070`, writing authoritative
  position to `CharacterPhysicsEntity+0xE0`.
  **Result: the full pipeline is now understood end-to-end**, matching
  the "Player facing/yaw + input pipeline" section above. **Open thread
  carried forward**: find what reads `CharacterPhysicsEntity`'s per-tick
  axis array downstream, to locate a true absolute accumulator (or a
  self-correcting delta approach) as the real hook target — see that
  section for the current plan.

- **2026-08-20 — BREAKTHROUGH: rotation's absolute heading found
  (`AimingController+0xC`/`+0x10`), freeze-confirmed.** Long session, several
  detours fully closed along the way (all preserved in
  `project_mohw_yaw_facing_investigation.md` memory): an exhaustive
  entry-to-return trace of `FUN_0049C880` found zero yaw/pitch consumption
  by any method (address, offset, or value) — closed as unreliable for
  this function generally, not just negative for yaw/pitch. A parallel
  facial-animation interpolation quad (`entity+0x138`) and a weapon
  idle-sway trig block (gated behind a condition that turned out unrelated
  to aim-assist) were both traced and closed as cosmetic. Pursuing the
  user's aim-assist-authority theory (aim assist must adjust real aim
  somehow) led through a weapon-aim-state update function
  (`FUN_009DDA90`) to its aim-assist target-cache (closed as generic
  refcounted-pointer infrastructure, twice), but re-examining that same
  function's tail found a genuine accumulator: `AimingController+0xC`, updated
  via `[AimingController+0xC] += delta` fed by the aim-assist blend. Live
  observation (value consistently 0..~2pi for a given look direction) plus
  a sustained freeze test confirmed it: forcing to π snapped and held
  facing, and real input pulled back toward the forced value rather than
  away — the same signature that proved `+0xE0` for position. Also found
  `AimingController+0x4C`/`+0x50` (yaw/pitch rate input feeding the
  accumulator) are independently freeze-confirmed authoritative too, while
  `+0x44`/`+0x48` (movement/strafe) are not. Full re-derivation chain for
  `AimingController` documented (`ClientSoldierEntity+0xB0+0x4A4` ->
  vtable-resolved `+0x430`), plus a fast-path using the confirmed stable
  write instruction (`0x009DE8F1` — corrected after first trying
  `0x009DE921`, a nearby wraparound-correction branch that almost never
  fires) to skip the chain walk on future
  launches. **One open caveat, not yet resolved:** releasing a one-shot
  freeze force lerps facing back to the pre-test baseline rather than
  holding or jumping to current input — suggesting a further-upstream
  corrector may exist. Next session: find that corrector (write breakpoint
  on `AimingController+0xC` excluding the known writer), decide whether to
  hook this field directly or the upstream source, then build the actual
  mod hook.

- **2026-08-21 — Investigation concluded; class identity confirmed;
  pivoting to implementation.** Continued from the previous session's
  breakthrough. Corrected the fast-path writer address (`0x009DE8F1`, not
  `0x009DE921` — the latter is a rarely-fired wraparound-correction branch,
  confirmed live it never hits). Identified `AimingController`'s real class
  via a vtable+trailing-string dump: `"MOHW/Weapons/TuningProfile/
  AimingController/spec/AC_Default_s3SP"` — renamed from the placeholder
  "weaponObject" throughout STATUS/ROADMAP/memory. Traced one step further
  downstream from `AimingController+0xC` and found genuine new consumers
  (not cosmetic dead ends): `FUN_009D0130` copies the heading into a
  different persistent object, and — the key structural finding — it's
  called inside a loop that broadcasts the heading to multiple destination
  objects per tick. Deliberately stopped there rather than enumerating
  every consumer: a broadcast pattern is a good sign for a hook target
  (one correct write updates every reader), and the real mod hook will
  continuously overwrite every frame, which should sidestep the earlier
  session's "lerp-back on release" caveat (a one-shot-force test artifact,
  not expected to matter for continuous writes). Discussed hooking strategy
  before implementation: static-address hooking (proven, matches this
  project's existing pattern) over vtable hooking (not needed — everything
  found is ordinary statically-addressed code, not virtual dispatch), and
  deriving `AimingController` from the hook's own calling convention
  (`this`/`ecx` at `FUN_009DDA90`'s entry) rather than replicating the
  multi-hop entity chain in mod code. **Next: build the hook.**

## How to use this file

Read this and the top of `ROADMAP.md` at the start of every session. When
something lands, tick it in `ROADMAP.md` and refresh the relevant section
here. When a session ends, append one dated bullet to the session log above
— even a short one — before signing off.
