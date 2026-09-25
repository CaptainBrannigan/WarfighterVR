# Phase 2C findings: low-level draw-call duplication

## Goal

Render the main scene twice per frame (once per eye) by hooking D3D11 draw
calls directly, since Phase 2C's high-level attempt (calling the engine's
own frame-render function twice) was abandoned for lack of a disassembler
(`docs/phase2c_highlevel_findings.md`).

## What was built

`hooks/draw_duplication_hook.cpp` hooks `Draw`/`DrawIndexed` and
`OMSetRenderTargets` on the immediate `ID3D11DeviceContext`. For each
qualifying draw: let the original call render normally (left eye), then
swap to a second offscreen render target + depth buffer
(`hooks/stereo_render.cpp`), patch the Phase 2B projection constant buffer
with different data, re-issue the *same* draw call, then restore both the
render target and constant buffer before returning control to the game.

## Identifying "main scene" draws: three attempts

1. **Constant-buffer identity (VS slot 0 == known projection buffer).**
   Two separate live tests, 1.5M+ real draws combined, found this gate
   *never* matched during actual per-object draws, despite the buffer being
   confirmed correct by content (Phase 2B) and briefly bound to VS slot 0
   at frame start.
2. **DX11.1 `VSSetConstantBuffers1`** (partial-constant-buffer-range
   binding, a separate interface/vtable method from the classic API).
   Hypothesis: the game might bind camera data via this newer API instead.
   Confirmed `ID3D11DeviceContext1` is supported on this system and hooked
   it; zero bindings observed across another large sample. Ruled out.
3. **Render-target size** (current path). Rather than identify "main
   scene" by what data feeds the shaders, identify it by the render
   target's *size*: track the largest render target seen this session:
   the main view/G-buffer will be the largest, while post-process/bloom/
   shadow passes are smaller. Confirmed working: filtered out mip-chain
   passes that an earlier unconditional-duplication test exposed (see
   below), landing on 261 duplicated draws in a ~1.5 min session versus
   thousands with no filter.

## Unconditional-duplication test (attempt before size filtering)

To decouple "does the render-target-swap mechanism work" from "which draws
should we duplicate," ran a build with the CB-identity gate removed
entirely -- every single draw call got duplicated, no filtering. Result:
**no crash**, draws succeeded with CB patching (confirmed via log), proving
the core swap/restore mechanism is sound. But it also duplicated
post-process/bloom mip-chain passes, visible in the log as targets
thrashing through many sizes in sequence (`64x32->32x16->...->1x1`,
`1920x1080->960x540->...`), each forcing our single second-target pair to
be recreated -- wasteful, and confirmed the real need for filtering.
User-observed effect during this test: noticeably worse performance (expected --
roughly doubling total draw calls plus the resource-recreation churn), but
no visual glitches, including UI. That's expected, not evidence of failure:
the duplicated draws render to a fully offscreen target that is never
displayed (no Present, no on-screen view built yet), so the only way this
test could show a visible difference is through performance, which it did.

## Current state: render-target size filtering

Tracks the largest render-target size seen (monotonic within a session) and
skips duplication for any draw whose current target is smaller. Confirmed
via live test: correctly filtered a 128x128 target and multiple 2048x2048/
1920x1080 targets down to only 3840x2160 (matching the actual swapchain/
backbuffer resolution), landing on 261 duplicated draws, no crash, clean
shutdown.

**New limitation surfaced by this same test:** even among the correctly-
sized (3840x2160) draws, the render target's *pixel format* changes
repeatedly (`fmt=34`, `fmt=10`, `fmt=34`, `fmt=10`, `fmt=28`, ...) --
`EnsureRightEyeTargets` recreates on any format change too, confirming this
is a **deferred renderer**: multiple full-resolution G-buffer channels
(likely albedo, normals, depth/other) rendered as separate passes, not one
single forward-rendered target. Our current design keeps only *one* shared
second-eye color+depth pair, so each G-buffer channel's duplicate
overwrites the previous one rather than accumulating into a coherent full
second G-buffer -- meaning a hypothetical lighting/resolve pass consuming
the "second eye" G-buffer wouldn't have simultaneous access to all of its
channels. This is a known, not-yet-addressed gap for genuinely correct
full-scene stereo output; the duplication mechanism itself (confirmed
working) is the harder, now-resolved part.

## Stability across all Phase 2C tests

Every variant tested (CB-gated, DX11.1-gated, unconditional, size-filtered)
ran without crashing across multiple live sessions, several exceeding a
minute of real gameplay with clean `DLL_PROCESS_DETACH` shutdowns. The
underlying hook/swap/restore mechanics are solid; remaining work is about
*what* to duplicate and *how many simultaneous targets* are needed for a
deferred renderer, not about hook safety.

## Next steps (not yet done)

- Handle multiple simultaneous G-buffer channels for the second eye (a
  small pool of per-format offscreen targets, rather than one shared pair),
  if truly correct full-scene stereo is the goal.
- Revisit whether a lighting/resolve pass needs to be duplicated too (once
  G-buffer duplication is multi-channel-correct), or whether forward-
  rendered elements (transparents, particles, UI) need separate handling.
- Real per-eye view/projection data (Phase 2D territory) -- current fake
  FOV-scale difference (`floats[0] *= 0.7f; floats[5] *= 0.7f;`) proves the
  mechanism, not real stereo parallax (still no view-matrix buffer
  identified).
