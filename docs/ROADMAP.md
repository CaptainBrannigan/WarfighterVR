# Roadmap

Phases/milestones ordered the way they actually happened, each with a "done
when" acceptance note. This is a retrofit — the project ran for several days
before this file existed — reconstructed from `docs/*_findings.md` and file
history. Going forward: tick boxes as work lands, move new findings into
their own `docs/*.md`, and keep `STATUS.md` current every session (a session
that ends without a `STATUS.md` update is an incomplete handoff).

## Phase 0 — Injection proof (done)

Goal: our own code runs inside `MOHW.exe`, safely, with logging.

- [x] Proxy `dxgi.dll`: real typed trampolines for `CreateDXGIFactory`/`1`/`2`
      (the exports the game actually calls, confirmed via `dumpbin /exports`
      against the live `SysWOW64\dxgi.dll` — `docs/dxgi_exports.txt`), lazy
      loading the real system DLL by full path
- [x] Correction vs. the original plan: MSVC's linker does not support `.def`
      `EntryName=Module.Function` forwarding (`LNK2001` on both x86/x64) —
      real trampolines are the only working option here
- [x] Deferred-init pattern established (`EnsureInitialized`, `std::call_once`,
      never from `DllMain`) — avoids loader-lock deadlocks for everything
      built afterward
- **Done when:** game loads our proxy DLL, log shows attach, real `dxgi.dll`
  still loads, game runs normally. ✅

## Phase 1 — De-risk: is there a native second-eye path? (done — dead end)

Goal: check whether `GameRenderer::secondaryStreamingView` is a drivable
second eye before committing to a full draw-call-duplication hook.

- [x] Hooked `UpdateMatrices` (`0x707CB0`) via MinHook, logged `RenderView`/
      thread/fov data per call
- [x] Corrected `RenderView`'s true size for this build (`0x490`, not the old
      header's `0x480`) via an empirical `fovY`/`aspect` scan instead of
      trusting stale class-layout assumptions
- [x] Found `secondaryStreamingView` is real and live, but is an
      asset-streaming look-ahead camera (70°, 4:3, zero stereo separation) —
      not a stereo eye
- [x] Confirmed `UpdateMatrices` fires from 6+ worker threads — any future
      `RenderView` hook must assume concurrent access, not single-threaded
- **Done when:** a clear yes/no, so Phase 2 isn't built on a wrong
  assumption. **NO — proceed to full draw-call duplication.**
  (`docs/phase1_findings.md`)

## Phase 2A — Own the swapchain (done)

Goal: hook `Present`/`ResizeBuffers` for a per-frame foothold.

- [x] Dummy-device vtable-read technique: throwaway 2×2 device+swapchain,
      read real `Present`/`ResizeBuffers` at vtable slots 8/13, discard the
      device
- [x] Moved off the calling thread entirely, on a background thread spawned
      from `InstallPresentHook` — a first synchronous attempt deadlocked the
      game inside `D3D11CreateDeviceAndSwapChain` (reentrant device creation
      contends for an internal DXGI/driver lock the game's own in-flight
      factory call already holds)
- **Done when:** Present hook logs every Nth frame, game renders normally. ✅

## Phase 2B — Find the camera data (done, superseded — see *Stereo rework*)

Goal: identify which GPU constant buffer carries view/projection data.

- [x] Hooked `Map`/`Unmap` + `VSSetConstantBuffers`/`PSSetConstantBuffers`
- [x] Content-signature scan (`proj[1][1] == 1/tan(fovY/2)`, no layout
      assumptions) found an 80-byte, VS-slot-0 buffer — structurally a clean
      projection matrix, bit-identical across 8 samples
- [x] Flagged as provisional in a longer real-gameplay session: 5 other
      buffers (352B, 4096B, 12800B ×2, 1472B) also matched the same
      signature by coincidence. Accepted the 80-byte candidate on structural
      grounds rather than build a stricter matcher immediately, to unblock
      Phase 2C
- **Done when:** one buffer identified with enough confidence to attempt a
  patch. **Confidence was later shown to be misplaced — see *Stereo
  rework*.** (`docs/phase2b_findings.md`)

## Phase 2C — Draw-call duplication (done)

Goal: render the scene twice per frame, once per eye.

- [x] High-level attempt (call the engine's own per-frame render entry point
      twice) — **abandoned.** Crashed twice at an identical fault address;
      root-caused to an assumed `__thiscall` stack-parameter count being
      wrong. A safe naked/ECX-only verification build (touches only `ECX`,
      tail-`JMP`s, asserts nothing about parameter count) ran clean and
      proved neither `createUpdateJob` nor `joinUpdateJob` is even the
      per-frame trigger (`joinUpdateJob` *waits* for scheduled work, doesn't
      *start* it) — real per-frame call site traced to one stable address
      anyway, preserved as a lead for if a disassembler ever becomes
      available (`docs/phase2c_highlevel_findings.md`)
- [x] Low-level path (the one that shipped): hooked `Draw`/`DrawIndexed`/
      `OMSetRenderTargets`, render-target-**size** filtering (track the
      largest RT seen this session) to isolate main-scene draws from
      post-process/shadow/mip-chain passes — 261 duplicated draws/session,
      correctly filtered
- [x] Unconditional-duplication smoke test (no filtering at all) proved the
      swap/restore mechanism itself is sound: no crash, no visual glitch,
      only the expected perf cost
- [ ] **Known gap, not yet closed:** this is a deferred renderer — multiple
      simultaneous G-buffer channels (format changes repeatedly at the same
      resolution). One shared second-eye target means each channel's
      duplicate overwrites the last rather than accumulating; a hypothetical
      multi-channel lighting/resolve pass wouldn't see a coherent second
      G-buffer yet
- **Done when:** duplication mechanism proven safe and selective. **Achieved
  — the real blocker turned out to be per-eye camera data, not the
  duplication mechanism.** (`docs/phase2c_findings.md`)

## Phase 2D — Get into a headset (done — after two dead ends)

Goal: a real OpenXR/OpenVR session submitting frames to the Quest.

- [x] OpenVR attempt: `VR_Init()` hung indefinitely. Isolated with a
      standalone, non-injected harness (`investigation/test_openvr_standalone.cpp`)
      — same hang with zero hooks involved, confirming an environment-level
      cause (current SteamVR is OpenXR-exclusive, no working legacy 32-bit
      OpenVR handshake), not our code
- [x] Direct 32-bit OpenXR attempt: `XR_ERROR_RUNTIME_UNAVAILABLE`. Root
      cause via the Windows OpenXR registry keys: SteamVR ships no 32-bit
      runtime manifest at all; only Meta's Quest-Link runtime has one, and
      it isn't active while connected through SteamVR
- [x] OpenComposite (OpenVR→OpenXR shim) tested as a possible bypass — same
      root cause, no workaround exists at the API level
- [x] **Decision:** 32-bit in-process OpenXR/OpenVR is a dead end on this
      SteamVR configuration. Pivot to a 64-bit companion process — the same
      contingency architecture bioshock-vr's own docs describe for a
      hypothetical 32-bit target
- **Done when:** a definitive, unblocked path to the headset is chosen.
  (`docs/phase2d_findings.md`)

## Companion Milestone A — Standalone 64-bit OpenXR session (done)

Goal: prove a 64-bit, non-injected process can hold a real OpenXR session
against this SteamVR install.

- [x] `companion/main.cpp`: instance → system → D3D11 graphics requirements
      (adapter-LUID-matched device) → session → per-eye swapchains → real
      `xrWaitFrame`/`xrBeginFrame`/`xrLocateViews`/`xrEndFrame` loop
      submitting solid red (left) / blue (right)
- [x] Confirmed live in-headset
- [x] Root-caused two environment blockers: an interactive-session mismatch
      when launched from the coding agent's own shell (fixed by running in
      the real interactive session — a non-issue once the companion is
      spawned as the game's own child process, inheriting its session
      automatically), and `vrserver`'s ~90-second post-launch discovery
      window for `CSharedResourceNamespaceServer`
- **Done when:** solid-color stereo visible in headset from a standalone
  process. ✅ (`docs/companion_process_findings.md`)

## Companion Milestone B — Cross-process shared-texture handoff (done)

Goal: get a rendered eye texture from a 32-bit producer into the 64-bit
companion's OpenXR swapchain.

- [x] Synthetic 32-bit producer (`investigation/test_shared_texture_producer.cpp`)
      proves the mechanism before wiring the real game
- [x] Named-shared-resource API (`CreateSharedHandle` by name /
      `OpenSharedResourceByName`) ruled out — fails `E_INVALIDARG` even
      same-process/same-name; a driver/OS gap, not a code bug
- [x] Fixed via explicit `DuplicateHandle` over a plain Win32 file-mapping
      (`shared/ipc_protocol.h`'s `HandleExchangeBlock`) instead of DXGI name
      lookup
- [x] Second bug found and fixed: needed `ID3D11Device1::OpenSharedResource1`
      (not the legacy `OpenSharedResource`) for NTHANDLE + keyed-mutex
      textures
- [x] Confirmed live in-headset: animated per-eye test pattern, correctly
      letterboxed at deliberately mismatched producer/consumer resolutions
- **Done when:** a synthetic eye texture crosses the process boundary and
  shows up in the headset. ✅ (`docs/companion_process_findings.md`)

## Companion Milestone C — Real game frames + head pose (in progress)

Goal: replace the synthetic producer with the actual game, and close the
loop with head-pose data flowing back to drive the camera.

- [x] `EnsureCompanionProcess()` launches `mohwvr_companion.exe` from the
      `Present` hook (background thread, same deferred-init pattern as
      everything else heavy/IPC in this project)
- [x] `UpdateCompanionEyes()` feeds the real backbuffer (left) + Phase 2C's
      stereo duplicate (right) into the shared textures every `Present`
- [x] `HeadPoseBlock` channel (companion → proxy_dll, lock-free
      publish-and-overwrite — a torn read of a few floats is at worst one
      frame of jitter, not a correctness problem) wired and consumed
- [x] Real per-eye view patch landed — see *Stereo rework* below, which
      needed a different buffer than Phase 2B found
- [x] Proper scaling — replaced with an aspect-correct "cover" blit
      (`ComputeCoverUvScaleOffset` in `companion/main.cpp`): scales
      uniformly to fill the destination, cropping whichever axis has
      excess, no shear or letterbox bars. Paired with real per-eye FOV
      pulled from `xrLocateViews` (symmetric average + 1.12x margin)
      instead of a hardcoded 90° — black borders now nearly unnoticeable.
      Confirmed live: fixed both a roll-induced shear artifact and visible
      black borders (2026-08-03)
- [x] Multi-channel G-buffer duplication for a fully correct second eye —
      see *Stereo depth: MRT + SRV redirection* below (2026-08-03)
- **Done when:** the real game, not a test pattern, renders stereoscopically
  with head tracking, confirmed live in-headset. ✅

## Stereo rework — the real view-projection buffer (done, most recent work)

Goal: Phase 2B's 80-byte/VS-slot-0 patch produced *zero* visible effect —
find what the shader actually reads.

- [x] Ruled out stale-buffer, not-bound, and write-not-landing explanations
      one at a time: session-long buffer-instance tracking (only 1 ever
      existed), per-draw binding checks across 80,000+ draws (100% bound),
      a deliberately absurd forced +5-unit offset (writes land, shader just
      doesn't read that field)
- [x] Pivoted to reading the real shader with 3Dmigoto's hunting mode —
      found the skinned-geometry vertex shader's actual cbuffer (352 bytes,
      `viewConstants`, VS slot 2). `viewMatrix`/`projMatrix`/
      `viewProjMatrix` are all compiler-marked `[unused]`; the real math is
      camera-relative: subtract `cameraPos`, then multiply by
      `crViewProjMatrix` (translation-free)
- [x] Patched `crViewProjMatrix` + `cameraPos` instead of the old fields.
      Fixed 3 follow-on bugs: camera position reconstructed from a dead
      field (108-unit drift) → read the buffer's own live `cameraPos`
      directly; only the hidden duplicate eye was patched → both eyes now
      patched (higher-risk change, touches the always-executed primary draw
      too); rotation direction was inverted → swapped to the conjugate delta
- [x] Fixed a 4th bug: `DrawInstanced`/`DrawIndexedInstanced` weren't
      hooked, so GPU-instanced static geometry (buildings, props)
      intermittently didn't track head rotation at all
- [x] Current status: head-tracked rotation (both eyes), IPD stereo
      separation, camera-relative position patch, and instanced geometry
      all working — **"rough but functional"** per live testing
- **Known unresolved limitation:** the game's own CPU-side frustum
  culling/streaming follows the game's own (non-head-tracked) camera
  direction — geometry outside that frustum was never submitted as a draw
  call, so it isn't merely mis-rendered when you look there via head motion
  alone, it doesn't exist in the frame at all. Structural limitation of any
  post-hoc constant-buffer-patching approach, not a bug here. Possible
  mitigations, not yet attempted: widen the game's own culling FOV, or hook
  the culling logic directly.
- **Done when:** moving your head in the headset visibly and correctly
  moves the world. ✅ (`docs/stereo_view_matrix_investigation.md`)

## Engine-level camera injection — a second, deeper rotation patch (done)

Goal: the per-draw CB patch above fixes what's *rendered*, but not the
engine's own game-logic view state — specifically motivated by *Stereo
rework*'s known limitation that CPU-side frustum culling/streaming still
follows the game's own (non-head-tracked) camera. Find a genuine per-tick
camera-transform write to patch instead, upstream of both rendering and
culling.

- [x] **Dead-end investigated first, in full:** traced the game's entire
      XInput/mouse input pipeline (raw accumulator → per-binding axis
      resolver → a 200+-entry action-value table, with move/look action
      indices identified) — confirmed exhaustively, via direct hardware
      breakpoints during live gamepad *and* keyboard/mouse look input, that
      every reader of this pipeline is UI/HUD/frontend/animation
      infrastructure, never the render camera. Real dead end for this task,
      but the full mapping is preserved for the future input-system work
      (see *Motion controls*, and `project_mohw_frostbite` memory for the
      complete trace if needed again)
- [x] Pivoted to tracing **backward** from the already-hooked view-constants
      upload (`FUN_007738c0`) instead of forward from input. Hit and fixed a
      methodological trap along the way: `OFFSET_UPDATEMATRICES` was already
      hooked in the shipping build, so an initial Ghidra import was actually
      decompiling our own MinHook trampoline — required temporarily
      disabling `InstallUpdateMatricesHook()` for a clean dump (**still
      disabled** as of this note, see *Next — harden before extending*)
- [x] Found the real chain: `UpdateMatrices` (`FUN_00707cb0`) consumes
      `RenderView::m_desc.transform` directly → traced its writer to
      `OFFSET_COMMITVIEWTRANSFORM` (`FUN_008ab580`), which commits an
      already-fully-computed world-space `LinearTransform` into
      `GameRenderer::m_viewParams` once per simulation tick, reached via the
      Windows message pump rather than a bare per-frame render tick.
      Independently converges with a camera-related lead flagged (and
      dismissed as "garbage data") earlier in this project's history, before
      live memory analysis tooling existed — three separate investigative
      threads across the project's life now agree on this one address
- [x] Built `hooks/commit_view_transform_hook.cpp` (MinHook + this-call
      trampoline, same pattern as `engine_function_hook.cpp`). Proof of
      concept (a forced yaw oscillation) confirmed live: camera visibly
      swayed independent of player input
- [x] Wired real HMD rotation (`ApplyHeadPoseRotation`, reusing
      `companion_bridge.h`'s `GetHeadPose()` and `sdk/vr_math.h`'s
      quaternion helpers, same recenter-on-first-pose convention as the
      CB-patch path). Found and fixed 3 real bugs via live iteration:
      1) **double-rotation risk** — the old CB-patch already applies HMD
         rotation downstream; fixed by temporarily forcing its `delta` to
         identity (`draw_duplication_hook.cpp:509`) rather than letting both
         compound. **Still a temporary override, not a permanent decision**
         — see *Next*
      2) **full-axis inversion** — this hook rotates the WORLD transform
         before the engine inverts it into a view matrix, the mathematical
         opposite of rotating the view matrix directly (what the CB-patch
         does); fixed by swapping to the conjugate delta
      3) **local-vs-world frame composition** — applying the delta directly
         to world-space basis vectors only produces correct results at the
         exact recenter orientation, and diverges as soon as mouse-yaw or
         non-axis-aligned spawn facing rotates away from it (caused
         pitch-flip specifically when turning via mouse, not via the
         headset). Fixed by composing the delta in the LOCAL frame instead
         (`R_new = R_old * Rot(delta)`)
- [x] User confirmed live: no inversion on any axis, pitch stays consistent
      turning via headset or mouse, roll/tilt feels proportional
- [x] **Game's own FOV separately scaled for comfort**
      (`commit_view_transform_hook.cpp`'s `ApplyFovScale`, patches
      `RenderViewDesc::fovY` at the same transform pointer, offset `0x48`).
      Live-tested several values (1.2x/2.0x/3.0x/2.5x/2.25x/1.71x/2.6x),
      **settled on 2.4x**. Confirmed this does NOT affect the first-person
      view-model (arms/weapon) — it reads FOV from a separate, still-
      unidentified source; a `prevView` fovY field was tried and found
      inconclusive
- [x] **Permanent rotation architecture decided (2026-08-03):** retired the
      CB-patch's rotation entirely rather than keeping it as a disabled-
      but-present path. `draw_duplication_hook.cpp`'s per-eye function had
      its recenter/delta-quaternion logic removed and was renamed
      `ApplyIpdOffsetToViewMatrix` (from `ApplyHeadPoseToViewMatrix`) to
      match its narrowed scope — it now only computes the per-eye IPD
      lateral offset from the buffer's own already-correctly-rotated
      state, doesn't read `HeadPoseBlock` at all, and the now-unused
      `companion_bridge.h` include was dropped. `CommitViewTransform` is
      the sole rotation source going forward.
- **Deprioritized, not tracked as an active item (2026-08-04):** whether
  this actually fixes the frustum culling lag that originally motivated
  this whole investigation was never verified in-game, and the user chose
  to drop it from active tracking rather than leave it open indefinitely.
  Not confirmed fixed, not confirmed still broken — just no longer a
  tracked priority. Revisit ad hoc if culling lag is ever noticed live.
- **Done when:** a genuine per-tick, engine-level camera-rotation injection
  point found, built, and confirmed correct live in-headset. ✅ Permanent
  rotation architecture also now decided and cleaned up.

## `InstallUpdateMatricesHook()` — decided to leave uninstalled (2026-08-03)

Not a phase of its own — captured here since it came up as a housekeeping
question and is easy to conflate with the rotation-architecture decision
above (both are "camera hook" decisions made the same day, but otherwise
unrelated). `updatematrices_hook.cpp` hooks `OFFSET_UPDATEMATRICES`
(`FUN_00707cb0`, the function that *consumes* `RenderView::m_desc.transform`
to rebuild view/projection matrices) purely to **log** which `RenderView`
instance is being updated, thread IDs, and to do empirical struct-layout
scanning — it never patches or injects anything, and nothing else in the
codebase reads its internal state (no exported getters beyond
install/remove). It was the investigative tool used to originally confirm
`GameRenderer::Singleton()+0x50` is the real camera `RenderView` and to help
trace the caller chain toward `OFFSET_COMMITVIEWTRANSFORM` (see *Engine-
level camera injection* above) — both already-banked, already-relied-upon
facts. Its job is done, and no live functionality depends on it being
installed (it was disabled mid-session purely to get a clean Ghidra dump of
`OFFSET_UPDATEMATRICES`'s own bytes, an unrelated one-time need).
**Decision: leave it uninstalled** (`proxy_dll/dllmain.cpp:112`, call stays
commented out) rather than re-enable it for no active purpose. The file is
kept in the tree, not deleted, since its layout-scanning technique
(`ScanForFovYAspectPairs`) may be worth reusing for the planned AOB/offset-
hardening work later.

## Stereo depth: MRT + SRV redirection (done)

Goal: close *Companion Milestone C*'s "only one shared second-eye G-buffer
target" gap — this is a deferred renderer with multiple simultaneous format
channels per frame (G-buffer albedo/normal/material, HDR lighting-
accumulation + motion vectors, etc.), and the single shared duplicate target
was overwriting itself rather than accumulating a coherent second G-buffer.

- [x] Confirmed the real scope first via a live diagnostic before writing
      any fix: real MRT shapes seen include `n=4` (four simultaneous
      R8G8B8A8_UNORM targets) and `n=3` (HDR + motion vectors + a mask
      channel) — 2-3 of every 3-4 G-buffer channels were being lost on the
      right eye
- [x] **MRT-aware right-eye target pool** (`hooks/stereo_render.h/.cpp`):
      format-keyed pool (capped at 16 entries) rather than naive per-slot
      recreation, since the game cycles many different MRT "shapes" through
      the same low slot indices with different formats frame to frame.
      Original single-target behavior (feeding `GetRightEyeColorTexture()`
      for the compositor) left completely untouched for `numRTVs<=1` draws
      — zero regression risk. One shared depth buffer across the MRT set,
      same simplification the original design already made
- [x] **Shader-resource-view redirection** — the write side alone wasn't
      enough, since the later lighting pass reads the G-buffer back as
      shader input (`PSSetShaderResources`, confirmed live: thousands of
      hits/minute across slots 0-3 and 10-13) and was still reading the
      game's original left-eye textures. Added SRVs alongside each pooled
      RTV, a persistent `originalResource -> right-eye SRV` map populated
      whenever a right-eye duplicate is written, and real-time slot
      tracking in `Hooked_PSSetShaderResources`. `DuplicateDraw`'s right-eye
      re-issue now swaps tracked slots to their right-eye SRV immediately
      before the draw and restores the exact original pointer immediately
      after
- [x] **Crash found and fixed on first live test:** `STATUS_ACCESS_VIOLATION`
      inside `d3d11.dll` itself, right after a burst of single-target
      recreations. Root cause: the SRV redirect map held a raw, non-owning
      pointer that went dangling whenever the single-target path did its
      existing release-and-recreate-on-mismatch, with nothing invalidating
      stale map entries — a genuine use-after-free. Fixed with a generation
      counter (`stereo_render.cpp`'s `g_targetGeneration`, bumped in
      `ReleaseAll()`) that purges the whole redirect map whenever pooled
      resources get recreated, checked once per draw before any lookup.
      Also fixed a secondary resource-hazard case (a stale tracked slot's
      redirect target coinciding with the current draw's own right-eye RTV)
      with an explicit skip-and-log guard
- [x] User confirmed live: noticeably more 3D-feeling motion when turning
      the head, after this landed. Not a rigorous full quality assessment —
      that remains open (see *Next*)
- [x] Diagnostic logging quieted afterward (`kVerboseStereoLogging = false`
      in `draw_duplication_hook.cpp`) now that the MRT-shape census and
      crash-cause questions are answered — flip back to `true` if this area
      needs debugging again
- [x] **The crash recurred (2026-08-03, later same day) — the generation-
      counter fix above was real but incomplete, not a false fix.** 100%
      reproducible, identical `d3d11.dll` fault every time, always during
      the launch→main-menu intro-movie transition (not a level load, as
      first assumed — corrected mid-investigation). Quieting logs further
      didn't help, which ruled out log-volume-as-cause but motivated
      rewriting `sdk/logging.h` anyway (was open+append+close, a real
      syscall pair, on every single call, on the render thread — now keeps
      one handle open per log file for the process lifetime, `fflush()`
      per write so crash forensics still work). With that fixed, safely
      re-enabled full diagnostics and saw a dense resource-format-thrashing
      burst immediately before every crash. A bisection test (a temporary
      `kEnableSrvRedirection` kill-switch, redirect block disabled but
      tracking hook left running) proved the bug lives specifically in the
      redirect apply/restore block or the map it reads, not the MRT pool
      churn itself.
- [x] **Real root cause found and fixed:** the purge-on-generation-change
      check only ran at the very *top* of `PrepareRightEyeTarget`,
      comparing against the generation as of the *end of the previous
      draw* — it couldn't see a bump caused by *that same draw's own*
      `EnsureRightEyeTargets`/`EnsureRightEyeMrtTargets` call, which runs
      later in that same function. During the observed thrashing burst (10
      single-target recreations within ~20ms), that left exactly a
      one-draw window where some *other* already-tracked resource's stale
      entry — pointing at whatever `g_colorSrv` was before this draw's own
      recreation just freed it — would still be found and dereferenced by
      the redirect lookup: a genuine use-after-free, not a false alarm.
      Fixed by extracting the purge into
      `PurgeStaleSrvRedirectsIfGenerationChanged()` and calling it a
      *second* time, immediately before the redirect lookup in
      `DuplicateDraw`, after that draw's own recreation has already run.
      Kill switch removed, redirection back on unconditionally. No
      recurrence since.
- [x] **Added runtime debug hotkeys** (`draw_duplication_hook.h/.cpp`,
      polled from `Hooked_Present`) so future A/B comparisons don't need a
      rebuild/redeploy cycle: **F11** toggles full G-buffer stereo (MRT +
      SRV redirection) vs. the old single-target-only path live; **F7/F8**
      step IPD scale down/up (0.5x-8x). Left in the code as standing tools.
- **Done when:** the lighting pass reads back the right eye's own G-buffer
  data, not the left eye's, without crashing. ✅ (full depth-quality
  assessment done, see *Depth-sense investigation* below — result was
  negative, motivating the next phase)

## Depth-sense investigation — why doesn't it feel 3D? (done — root cause identified, fix not yet built)

Goal: with the crash fixed and full G-buffer stereo genuinely working, the
user reported still not perceiving a real sense of depth, and wanted the
underlying cause found before doing more stereo-quality tuning.

- [x] A/B tested full G-buffer stereo vs. the old single-target-only path
      live via the new F11 hotkey — no perceptible difference either way.
      Reframed as a real, informative result rather than a null result:
      G-buffer completeness mainly affects shading *quality* in the right
      eye (why the rain shader broke), not raw geometric parallax, which
      the constant-buffer position patch already provides identically
      regardless of G-buffer completeness.
- [x] A/B tested IPD amplification up to 8x via the new F7/F8 hotkeys —
      also no improvement; at 8x the near view-model got pushed
      completely off-screen instead. Root-caused as expected/correct
      behavior, not a bug: disparity scales inversely with distance, so
      extreme IPD mostly overwhelms *near* objects (breaking fusion on the
      weapon) without proportionally helping the *distant/mid-range*
      geometry that "does this feel 3D" should actually be judged against.
- [x] Ruled out an eye-routing bug directly: added a temporary solid-color
      debug marker (red left-eye / blue right-eye, `ClearView` on a
      centered rect in each eye's final swapchain image, matching this
      project's original Companion Milestone A solid-color-per-eye
      convention) into `companion/main.cpp`. First attempt placed it in
      the top-left corner and was invisible — corrected by moving it to
      dead center, since the corner almost certainly falls outside the
      lens's visible area once the FOV-margin/"cover" crop work is
      accounted for. User confirmed red-left/blue-right correctly — eye
      routing (game → shared handles → companion → OpenXR swapchains) is
      genuinely correct end to end. Marker removed again afterward, not
      shipped.
- [x] With eye routing and IPD both cleared, pulled bioshock-vr's actual
      head-tracking code for direct comparison
      (`https://github.com/mohamad-balouza/bioshock-vr`, already the
      source project for the companion-process architecture and future
      motion-controls work — see `docs/motion_controls_research.md`).
      Findings: their `apply_eye_offset()` is the same simple lateral-
      position-shift IPD technique MOHW already uses (no asymmetric/
      off-axis frustum either) — confirms that part of the approach was
      never the gap. But their camera code separately, explicitly applies
      **head position** every frame: raw HMD position delta from recenter,
      converted through an axis remap and a `-recenterYaw` → `+gameYaw`
      rotation (so real-world head movement translates the game camera
      relative to wherever the character is *currently* facing, not the
      raw room axes), scaled by a documented world-scale constant (100
      Unreal units/meter for their engine).
- [x] Checked MOHW's own code against this and confirmed the gap directly:
      `HeadPoseBlock.positionX/Y/Z` (`shared/ipc_protocol.h`) is published
      every frame by the companion from real OpenXR tracking, and is even
      read for logging (`companion_bridge.cpp`'s `LogHeadPoseIfDue`) — but
      grepping every render-path caller of `GetHeadPose()` shows only
      `orientationX/Y/Z/W` is ever consumed, in both
      `commit_view_transform_hook.cpp` and `draw_duplication_hook.cpp`.
      Head translation reaches the proxy DLL and is then silently
      discarded. Motion parallax — plausibly the stronger, more robust
      depth cue of the two versus static binocular disparity — is
      completely absent from the current pipeline.
- **Done when:** root cause identified with enough confidence to act on.
  ✅ **Fix not yet built — see *Head position / motion parallax* below,
  the explicit next-session task.**

## Head position / motion parallax (done — correct on first live test)

Goal: apply real head *translation*, not just orientation, to the game
camera — as a direct test of how much it improves the sense of depth,
following directly from the *Depth-sense investigation* above.

- [x] Extended `commit_view_transform_hook.cpp` (the same hook that already
      owned rotation) rather than the per-draw CB patch — rotation and
      position now happen on the same single per-tick upstream write.
      `ApplyHeadPoseRotation` renamed `ApplyHeadPoseTransform` to reflect
      the wider scope; reads/writes all 16 floats of the transform now
      (was 12 -- `trans` at floats[12..14] is the new part), gated behind
      the same non-unit-basis rejection check as before.
- [x] Added a recenter-on-first-pose zero-position reference (`zeroPosition`),
      parallel to the existing `zeroOrientation` pattern, captured together
      on first pose (`haveZeroPose`).
- [x] Reprojected the raw HMD position delta into world space, reusing the
      EXISTING local-frame composition machinery already proven for
      rotation rather than bioshock-vr's raw yaw-angle trig: `rawDelta` =
      `currentPos - zeroPosition` (OpenXR LOCAL space, meters) → un-rotated
      by `QuatConjugate(zeroOrientation)` to express it relative to how the
      head was facing at recenter (`localDelta`) → re-projected onto the
      camera's CURRENT world-space left/up/forward basis the same way the
      rotation code projects its rotated local axes (`worldDelta`). Axis-
      mapping assumption (OpenXR local +X/+Y/+Z <-> game left/up/forward,
      no remapping) matches what the already-validated rotation code
      implicitly assumes -- **not yet independently live-validated for
      translation specifically**, flagged in-code as the first thing to
      revisit if leaning produces a wrong-direction or wrong-axis shift.
- [x] World-scale: reused the existing IPD assumption (1 game unit = 1
      meter) rather than introducing a new constant, simpler than
      bioshock-vr's 100-units/meter UE case -- not yet confirmed live
      whether this holds for translation magnitude.
- [x] Added throttled position telemetry (`LogPositionIfDue`, 2s interval,
      `mohwvr_committransform.log`) logging raw/local/world delta together
      from the start, rather than adding it reactively after a sign bug is
      suspected (matches this session's separate FOV-logging request).
- [x] **Live-tested (2026-08-04): correct on the first try.** User: "Perfect
      first try head tracking actually." No sign/frame-of-reference bugs
      needed fixing, unlike the rotation work's three-bug history — the
      axis-mapping assumption (OpenXR local +X/+Y/+Z <-> game left/up/
      forward, no remapping) held. `LogPositionIfDue`'s telemetry
      (`mohwvr_committransform.log`, NOT `mohwvr_stereo.log` — worth
      remembering since each hook module logs to its own file: F5/F6 and
      position telemetry are `commit_view_transform_hook.cpp`'s, F7/F8/F11
      are `draw_duplication_hook.cpp`'s) confirmed sensible small-magnitude
      deltas tracking real head movement live.
- **Done when:** leaning/shifting your head in the headset visibly and
  correctly translates the in-game view, and the user can assess whether
  it meaningfully improves the sense of 3D depth over rotation-only
  head tracking. ✅

## FOV/IPD minification hypothesis (confirmed live)

Goal: test a second candidate explanation for the missing sense of depth,
raised by the user (2026-08-04) alongside the head-position plan above —
whether the game-internal FOV scale itself (2.4x, for comfort) is
suppressing perceived depth, independent of the head-position gap.

**Confirmed correct.** User: "I definitely see a difference adjusting the
IPD having brought the fov down." Lowering FOV scale first (via the new F5
hotkey), then re-adjusting IPD with the widened F7/F8 controls, produced a
real, perceptible depth difference for the first time — where amplifying
IPD alone at the original 2.4x FOV scale had produced none. Confirms the
minification reasoning below: the original 2.4x FOV/real-FOV mismatch was
suppressing stereo disparity regardless of how much IPD was dialed in,
which is exactly why the earlier IPD-only A/B test read as a dead end.
Live values explored across this session's testing ranged FOV ~1.6x-3.0x
and IPD up to ~14.5x. **Decision: move on to head-position tracking now**
(per the user); revisit exact FOV/IPD values together with that work
rather than over-tuning them in isolation first.

**Baseline recorded (2026-08-04, after head-position tracking landed):**
the session's final settled values — **FOV scale 2.2x (110° vertical,
native 50°) + IPD scale 3.5x** — were confirmed as a good working pair and
set as the new compile-time defaults (`g_fovYScale`/`g_ipdScale`'s initial
values), specifically so there's a known-good reset point rather than
requiring hotkey retuning from scratch every session. Not claimed final —
just the confirmed-working reference.

**Reasoning:** `commit_view_transform_hook.cpp`'s `ApplyFovScale` widens
what the GAME renders at (internal FOV), but `companion/main.cpp` submits
to the compositor using the REAL per-eye FOV from `xrLocateViews` (the
actual headset optics), not the scaled value. A rendered-FOV vs.
submitted-FOV mismatch minifies the displayed image (working through the
tan-based projection math: a world feature the game places at ~50°
off-axis using its 132° internal FOV ends up displayed at roughly 32° of
real visual angle once mapped onto the real ~100° headset FOV) —
compressing every depth cue by roughly that same factor, including stereo
disparity. This would explain why amplifying IPD up to 8x barely helped
(*Depth-sense investigation* above): whatever extra disparity it added was
likely getting minified right back down before reaching the eyes.

- [x] Made FOV scale runtime-adjustable (`commit_view_transform_hook.cpp`'s
      `g_fovYScale`, was a `constexpr`) with new **F5/F6** hotkeys (0.2x
      steps, floor at 1.0x native) — `commit_view_transform_hook.h`'s
      `CheckFovScaleHotkeys()`, polled from `Hooked_Present` alongside the
      existing F7/F8/F11 stereo debug hotkeys.
- [x] Per the same request, widened the IPD hotkeys: step changed from
      0.5x to 1.0x, the 8x upper cap removed (kept only a large numerical
      safety ceiling, not a real limit) — `draw_duplication_hook.cpp`'s
      `kIpdScaleStep`/`kIpdScaleMax`.
- [x] **Added real degree-level FOV telemetry** (`commit_view_transform_hook.cpp`'s
      `LogFovIfDue`, 2s-throttled, unconditional/not gated behind a verbose
      flag) — logs native and scaled fovY in both radians and degrees
      every 2s, so "what is the FOV right now" has a directly-readable
      answer in `mohwvr_committransform.log` instead of requiring a hand
      computation from a remembered multiplier (the F5/F6 keypress log
      only ever showed the multiplier).
- [x] **Tested live — see Confirmed above.**
- **Done when:** live-tested with a clear yes/no on whether FOV scale
  reduction restores perceptible depth. ✅ Yes. Exact final FOV/IPD values
  intentionally left untuned — revisit together with head-position work.

## Next — harden before extending

Not yet scheduled into a numbered phase; captured here so a future session
picks these up deliberately instead of by accident.

- [ ] **Investigate the rain-shader (and possibly other) rendering
      corruption under full G-buffer SRV redirection.** Not yet debugged —
      plausibly a shader doing something format/precision-sensitive (soft
      particles, refraction/depth sampling) that doesn't tolerate being
      handed the right eye's redirected G-buffer textures. Worth deciding
      whether to fix it or scope redirection to known-safe channels only,
      once the depth-cue work above settles whether full G-buffer stereo
      is even worth keeping.
- [ ] **Profile performance of the MRT + SRV redirection pipeline.** User
      reported "not great" performance with it active; not yet attributed
      to MRT pool overhead vs. SRV rebind churn vs. something else.
- [ ] **Address-scan robustness.** `mohw_offsets.h`'s static RVAs are
      guarded only by a whole-image-size check (`VerifyBuild`) — that
      catches "wrong build" but not "same-size patch, offsets moved." Build
      an AOB/string-xref/prologue-walk scanner (bioshock-vr's technique) for
      at least `OFFSET_UPDATEMATRICES` and the `viewConstants` buffer
      identity, so a future patch degrades to "doesn't find it, logs and
      bails" instead of silently hooking the wrong thing.
- [ ] **D3D11 entry-point audit.** A log-only pass hooking every remaining
      submission entry point (`DrawAuto`, `ExecuteIndirect`, compute
      `Dispatch` if the renderer uses one for geometry) to check for another
      blind spot like the missed `DrawInstanced` bug, before trusting stereo
      completeness further.
- [x] **Closed, won't-fix (2026-08-04):** what Phase 2B's 80-byte/VS-slot-0
      buffer is actually for (dead weight for the main shader, but still
      bound to something) — confirmed dead for anything currently
      rendered; resolving it further would need another shader-disassembly
      pass for no practical payoff, since nothing depends on the answer.
      Explicitly dropped rather than left open.
- [ ] Multi-channel G-buffer + proper scaling (carried from Companion
      Milestone C above).

## Investigate — high-level double-render seam (not started, tooling-blocked)

Goal: find out whether MOHW has a bioshock-vr-style seam — a single, safe,
re-callable "build/submit this frame's render jobs" entry point that could
be called twice per tick with a different camera each time, letting the
*entire* frame (every pass, every G-buffer channel) redo itself per eye
instead of the current surgical draw-call duplication. If this exists and
is safe to double-call, it would retire the multi-channel G-buffer gap in
*Companion Milestone C* outright rather than requiring a per-format target
pool.

**This was already attempted once and stopped for a tooling reason, not an
architectural one** — re-read `docs/phase2c_highlevel_findings.md` before
starting. It guessed at `createUpdateJob`/`joinUpdateJob`'s calling
convention, crashed twice from a wrong assumed stack-parameter count, then
a safe naked/`ECX`-only verification build proved neither function is the
per-frame trigger — and traced the real per-frame caller of `joinUpdateJob`
to one stable call site (`RVA+0x496DE4`) before stopping because no
disassembler was installed on the machine.

- [ ] Install a real disassembler/decompiler — **Ghidra** (free, scriptable,
      built-in decompiler, handles 32-bit PE) is the natural fit; x64dbg
      (32-bit build) as a live complement for breakpointing the call site
      and walking the actual stack/caller chain rather than only inferring
      from static code
- [ ] Read the function containing `RVA+0x496DE4` — is it a simple
      kick-jobs-and-wait loop, or does it also touch per-frame
      allocators/ring buffers/generation counters a second call would
      double-consume or corrupt?
- [ ] Get `createUpdateJob`/`joinUpdateJob`'s real signatures from the
      disassembly instead of guessing — worth doing even if the
      double-call idea goes nowhere, since it was the direct cause of the
      original crash
- [ ] Check for a dormant view/eye-loop remnant (split-screen or
      cascaded-shadow-map code Frostbite 2 titles commonly carry even when
      unused) — a much safer re-entry point than the raw job-kickoff
      function if one exists
- [ ] **Go/no-go test:** once a candidate seam is found, verify re-entrancy
      safety the same way Phase 2C-high-level already validated hook
      *mechanism* (naked/ECX-only, no signature assumptions) before ever
      trying a real double-call
- **The real risk, distinct from the tooling gap:** Frostbite's renderer is
  an explicit multi-threaded job graph (`UpdateMatrices` alone already
  confirmed firing from 6+ worker threads, Phase 1). Re-entering a
  job-kickoff function mid-frame risks corrupting per-frame job-graph state
  in a way UE2.5's comparatively simple render loop doesn't. Treat this as
  a bounded, time-boxed de-risking pass with an explicit go/no-go answer —
  same discipline as Phase 1's dual-view check — not an open-ended bet. If
  the answer is no, the current draw-duplication approach is the proven
  fallback, not a placeholder.

## Head-driven aim/movement direction (unblocked — investigation concluded, ready to implement)

Goal: make gameplay aim and movement-forward direction actually follow
head tracking, given "the head tracking is purely rendering" was flagged
(2026-08-03) as the priority after head-position tracking landed.

**Current status (2026-08-21 — see `docs/STATUS.md`'s "Player facing/yaw +
input pipeline" section and `project_mohw_yaw_facing_investigation.md`
memory for full detail):** the plan below — additively injecting HMD
deltas into `OFFSET_INPUTACCUMULATOR` (`FUN_008e03c0`) — is the same
approach that crashed on its one live test (2026-08-04, signature
mismatch) and was separately judged architecturally wrong even if fixed (a
per-frame delta into a shared, per-frame-reset accumulator drifts over
time; needs an absolute-write target instead, mirroring
`CommitViewTransform`). The hook is disabled, not deleted; **do not resume
it, fixed or otherwise — it's superseded by the finding below, not merely
paused.**

**The absolute-write target this whole investigation was searching for has
now been found, freeze-confirmed, AND its class identity confirmed:**
`AimingController+0xC`/`+0x10` (radians, `0`..`~2pi`) — `AimingController`
is the game's own named class (`"MOHW/Weapons/TuningProfile/
AimingController/spec/AC_Default_s3SP"`, found via vtable+trailing-string
dump), reached via `CharacterPhysicsEntity`'s axis pair ->
`AimingController+0x4C`/`+0x50` (cached rate input, also authoritative) -> an
aim-assist blend/accumulation step inside `FUN_009DDA90` -> settles into
`+0xC`/`+0x10`. Forcing it snaps and holds visible facing; real input
afterward pulls back *toward* the forced value, the same signature that
proved `CharacterPhysicsEntity+0xE0` for position. A downstream-consumer
check found the heading gets broadcast to multiple other objects per tick
(good sign — one write updates every reader) and confirmed the field
really is load-bearing, not a dead end. Full chain, the per-launch
re-derivation path for `AimingController`, and a fast-path stable
write-instruction breakpoint (`0x009DE8F1`) are all in `docs/STATUS.md` and
the memory file.

**One caveat, judged unlikely to matter, not blocking implementation:**
releasing a one-shot freeze force on this field lerps facing back to the
pre-test baseline rather than holding — suggesting a further-upstream
corrector may exist, never identified. Deliberately not chased further:
that behavior only matters for a force-once-and-release test methodology,
and the real mod hook will **continuously overwrite every frame** (not
force-once-and-release), which should sidestep it entirely. **Resume that
thread only if the built hook shows visible fighting/jitter** — otherwise
proceed straight to implementation.

**Hook implementation plan, settled 2026-08-21:** hook `FUN_009DDA90`'s
entry via a MinHook this-call trampoline (same pattern as
`commit_view_transform_hook.cpp`) — this hands us `AimingController` as
`this`/`ecx` directly from the function's own calling convention, so mod
code never needs to replicate the `ClientSoldierEntity+0xB0+0x4A4+0x430`
chain at runtime. Call through to the original unconditionally, then
overwrite `AimingController+0xC`/`+0x10` with HMD-derived yaw/pitch
afterward, so the game's own computation runs first and our write is the
last thing that happens before consumption — same shape as
`CommitViewTransform`. Considered and rejected vtable hooking for this
target: everything found downstream is ordinary statically-addressed code,
not virtual dispatch, so a direct address hook is simpler and matches
this project's already-proven pattern.

**Built (2026-08-04):** `hooks/gameplay_input_hook.h/.cpp`, hooking a new
address (`OFFSET_INPUTACCUMULATOR` = `FUN_008e03c0`, added to
`sdk/mohw_offsets.h`) that accumulates each frame's gamepad/mouse-derived
look/move deltas into `this+0x10C`(yaw)/`+0x110`(pitch). Detour forwards to
the original unconditionally every call, then additively injects the HMD's
**frame-to-frame** rotation (not delta-since-recenter like the render
hooks -- this needs an incremental look input, computed via
`QuatMultiply(QuatConjugate(lastOrientation), current)` and a small-angle
linearization to yaw/pitch radians) on top, scaled by a tunable
sensitivity. Gated behind `sdk/settings.h`'s new `HeadAimEnabled` (default
**OFF**, unlike FOV/IPD) + `HeadAimSensitivity` fields, toggled live via
**F12** (on/off) and **F1/F2** (sensitivity step), same persistent-settings
pattern as FOV/IPD. Throttled telemetry to `mohwvr_gameplayinput.log`.

**Real, flagged risk — read before testing:** unlike every other hook in
this project, `OFFSET_INPUTACCUMULATOR`'s exact signature was never fully
confirmed — only "ECX=this" was ever verified live (a breakpoint at entry
logging `{ecx}`), not whether it also takes stack parameters. A `__thiscall`
MinHook detour with the wrong assumed stack-parameter count is exactly what
caused this project's first-ever crash (Phase 2C-high-level). The hook
assumes zero stack parameters (best guess, not re-verified this session).
**If the game crashes immediately after this hook installs** (not after
F12/F1/F2 — the hook forwards to the original every call regardless of
whether head-aim is enabled), **that mismatch is the first thing to
suspect** — comment out `InstallGameplayInputHook()`'s call in
`proxy_dll/dllmain.cpp` and rebuild, not a deeper investigation.

**Design settled (2026-08-04), superseding the two earlier, more cautious
framings below.** The user's read on the earlier "wait for controllers"
caution: only **weapon aim specifically** needs to eventually become
controller-driven and detached from head orientation — that's a real,
later requirement (see *Motion controls* below), but not a reason to
withhold head-driven aim/movement in the meantime. It's more practical to
get head-forward movement and head-driven aiming working now, using
existing head-tracking infrastructure, and treat "decouple weapon aim from
body facing, drive it from a controller instead" as a separate, later
refinement once motion controllers actually exist — not a blocker on doing
anything now.

**Concrete plan (superseded — kept for the parts still valid once the
active investigation finds a safe write target):** additively inject the
HMD's yaw/pitch delta into a persistent gameplay-side facing value the
same way `commit_view_transform_hook.cpp` already does for the render
camera. The `this+0x10C`/`+0x110` accumulator and the "action-value table
indices 7/8" framing below are both now known to land on the same
input/rate-shaped pipeline, one just earlier in the chain than the
other — neither is the absolute-write target this needs; see the status
note above. Since MOHW's movement direction is computed relative to the
SAME facing this pipeline drives (a traditional FPS, no separate
body/weapon-aim decoupling in the base engine), whatever the real
injection point turns out to be should still drive both character
facing/movement-forward AND weapon aim together — matching how mouse-look
already works today, just fed by head orientation as well. The full
input pipeline (device binding, action-value table, master axis resolver,
`SetAxis`, down to `CharacterPhysicsEntity`) is now mapped in full detail
in `docs/STATUS.md` and `project_mohw_yaw_facing_investigation.md`
memory — reuse that mapping rather than re-deriving it.

**Needs to be player-adjustable** (the user's third concern, 2026-08-04) —
an enable/disable toggle (and likely a sensitivity scale) belongs in
`sdk/settings.h`'s new persistent settings file alongside FOV/IPD scale,
not a hardcoded on/off, so it can be tuned or turned off entirely without a
rebuild, and so a later controller-driven-aim setting can coexist as a
separate toggle in the same file once that's built.

**Superseded framings, kept for context:**
- (2026-08-03, first pass) originally assumed plan: additively inject HMD
  yaw/pitch into the accumulator, same as now settled on above.
- (2026-08-03, walked back same session) reconsidered to NOT feed head
  yaw/pitch into aim at all, possibly only roll, treat head XY as cursor-
  style input — reasoning was that motion-controller aim should be fully
  detached from head orientation. Superseded 2026-08-04: that reasoning
  only actually applies to weapon aim specifically, not movement-facing,
  and doesn't justify withholding a practical interim solution now.

## Motion controls (not started — design researched)

Goal: read Quest 3 Touch controllers, drive game input, and eventually show
a tracked weapon/hand model — and, per the design above, provide the
eventual controller-driven weapon-aim path that will run alongside (and
for aim specifically, override) the head-driven approach above.

- [ ] Companion-side OpenXR action set (pose + buttons/sticks) — same shape
      as bioshock-vr's `openxr_input.cpp`, but living in `companion/`
      instead of in-process (MOHW can't reach OpenXR from the 32-bit side)
- [ ] New companion → proxy_dll shared-memory channel for composed pad
      state, parallel to `HeadPoseBlock`
- [ ] Confirm which XInput version `MOHW.exe` actually imports (`dumpbin
      /imports`, the same technique already used for `dxgi.dll` —
      `docs/dxgi_exports.txt`) before assuming `xinput1_3.dll`
- [ ] Proxy XInput DLL, forward-everything + post-state-hook shape — same
      pattern as the proxy `dxgi.dll`
- [ ] Visible weapon/hand tracking — genuinely new Frostbite-specific
      reverse-engineering; nothing here transfers from bioshock-vr's
      UE2.5-specific bone code
- Reference: `docs/motion_controls_research.md` (design only, no code yet)

## Depth of field (researched, deliberately deferred)

Goal (if pursued later): a focus-adaptive depth-of-field pass as an
additional VR immersion/depth cue, layered on top of correct stereo —
NOT a substitute for fixing the underlying sense-of-depth problem
(*Depth-sense investigation* / *Head position / motion parallax* above).

**Technique, as researched (2026-08-04, from the user, describing a known
gaze/focus-adaptive DOF approach used elsewhere in VR modding, not MOHW-
specific yet):**
1. Fill a half-resolution RGBA "DOF buffer" from the final composited image
   (before bloom/tonemapping), alpha channel = blur amount, computed as
   `k * (1/focalPoint - 1/distance)`.
2. Blur the alpha channel horizontally then vertically. Must be depth-
   aware: farther fragments never contribute to a nearer fragment's blur
   (prevents background bleeding onto foreground edges); nearer fragments
   DO contribute to farther ones, scaled by their own alpha (lets blurry
   near objects bleed outward over their true extent).
3. Blur the RGB channels the same depth-aware way, driven by the
   already-blurred alpha.
4. Composite the DOF buffer's RGB back into the final image using the
   blurred alpha as the mix factor.
5. **Refinement:** rather than a single focal plane (physically accurate
   but tends to lose small near-field foreground objects that a focus-point
   raycast/sample only weakly hits), use a forced IN-FOCUS RANGE anchored
   at the focus-point-computed distance, widened to include anything
   closer that the sample would otherwise miss. Not physically accurate
   (a real lens has one focal plane) but reads as more pleasant — keeps
   center-screen content sharp while letting periphery and background
   fall out of focus. Known drawback: still lets the player consciously
   focus on the blur (though this reads as most objectionable in the near
   field specifically, which the forced range mostly prevents unless
   looking away from center).

**Why deferred rather than started:** this is a polish layer that assumes
the base stereo experience already reads as three-dimensional — adding
blur on top of a scene that doesn't yet feel 3D wouldn't establish that,
and would make it harder to judge whether the FOV/IPD and head-position
work (above) is actually landing, since blur itself changes perceived
sharpness/depth judgment. It would also be a genuinely new capability for
this project: a custom compiled HLSL depth-aware separable-blur pass (only
prior art is `companion/main.cpp`'s much simpler blit shader), needing its
own investigation to find a stable post-composite/pre-tonemap injection
point in this reverse-engineered pipeline. Revisit once the fundamentals
are settled and validated live.

## Release readiness (not started)

- [ ] Comfort pass once motion controls exist (turning/movement options,
      IPD calibration UI)
- [ ] Config/installer polish
- [ ] Licensing/distribution review before any public release — unlike
      bioshock-vr, there's no confirmed precedent for redistributing a mod
      for this specific title
