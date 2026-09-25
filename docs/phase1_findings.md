# Phase 1 findings: native dual-view stereo path

## Question

Does `GameRenderViewParams::secondaryStreamingView` (plus its
`secondaryStreamingViewEnable` flag) represent a live, drivable second
render pass suitable for VR stereo (i.e. could we feed it an OpenVR eye
pose/projection instead of building a full DX11 draw-call-duplication hook)?

## Method

1. Hooked `OFFSET_UPDATEMATRICES` (0x707CB0) via MinHook from `proxy_dll`,
   logging which `RenderView*` gets passed in each call, thread ID, and the
   view's `fovY`/`aspect`/`stereoSeparation`/`stereoConvergence`.
2. First run: compared the `self` pointer against
   `&GameRenderer::Singleton()->m_viewParams.{view,prevView,secondaryStreamingView}`
   computed from the *old* (different-build) header's class layout. Every
   call classified as "unknown" and `secondaryStreamingViewEnable` read
   garbage -- meaning that layout is stale for this build.
3. Second run: added an empirical scan of `GameRenderer`'s own memory for a
   float pair matching a `self->m_desc.fovY/aspect` we already trusted (a
   fixed +0x10 byte gap between those two fields), to locate real `RenderView`
   instances inside `GameRenderer` without needing a fresh reversed class dump.
4. Third run: used the stride discovered in (3) to directly read (not
   pattern-match) the candidate `secondaryStreamingView` location and its
   projected enable-flag location.

## Results

- `m_viewParams` starts at `GameRenderer+0x50` -- this matches the old
  header's `PAD(0x48)` assumption exactly.
- `prevView` starts at `GameRenderer+0x4E0`, an 0x490 gap from `view`, not
  the 0x480 the old header assumed. **`RenderView` is 0x490 bytes in this
  build**, 16 bytes larger than previously assumed. (`sdk/renderview.h` has
  not been corrected for this yet -- the extra 16 bytes' exact location
  within the struct is unknown, only the total size discrepancy is
  confirmed.)
- Projecting the same 0x490 stride forward, `secondaryStreamingView` would
  sit at `GameRenderer+0x970`. Reading that location directly (three
  independent samples over ~30s, both across two separate game sessions)
  consistently shows: `type=0, fovY=1.2217 (70.0 deg), aspect=1.3333 (4:3),
  stereoSeparation=0.0000, stereoConvergence=0.0000`.
- The main view (`GameRenderer+0x50`) consistently shows
  `fovY=0.9599 (55.0 deg), aspect=1.7778 (16:9)`, matching the actual
  gameplay window.
- A `self` pointer with `fovY=1.2217/aspect=1.3333` was independently
  observed passed into `UpdateMatrices` during the first (pre-scan) run --
  i.e. this candidate slot is being actively driven by real per-frame
  updates, not stale/zeroed memory.
- The projected enable-flag location (`GameRenderer+0xE00`) does not read as
  a sane bool/int (consistently a large nonsense value) -- the stride
  assumption likely doesn't extend cleanly a third time (there may be
  additional fields between `prevView` and `secondaryStreamingView`, or its
  size/layout differs from the two view slots). Not resolved, and not
  important given the conclusion below.

## Conclusion

`secondaryStreamingView` is real and live, but its FOV/aspect (70 deg, 4:3)
differ from the main camera (55 deg, 16:9) while `stereoSeparation`/
`stereoConvergence` are zero, same as the main view. That combination --
wider conservative frustum, different aspect, no stereo offset -- is the
signature of an **asset-streaming look-ahead camera**, consistent with its
literal name, not a second eye for stereo rendering. The `stereoSeparation`/
`stereoConvergence` fields most likely exist generically on the shared
`RenderViewDesc` struct (every `RenderView` instance has them) rather than
being specifically wired for this slot.

**Recommendation: treat the native dual-view path as a dead end for stereo
and move to the fallback path (full DX11 draw-call duplication hook,
per-eye) for Phase 2.** This is a real pivot from the plan's default
assumption and is flagged to the user for confirmation before committing
further implementation effort to it.

## Side findings worth keeping

- `UpdateMatrices` is called from (at least) 6 distinct worker threads, each
  consistently reusing its own `self` pointer across many calls --
  consistent with a job-system architecture computing per-thread scratch
  `RenderView` copies rather than working directly on the persistent
  `GameRenderer::m_viewParams` storage in place. Confirms the threading risk
  already flagged in the plan; any future hook touching `RenderView` state
  must not assume single-threaded access.
- `RenderView`'s true size for this build (0x490) should be corrected in
  `sdk/renderview.h` before it's relied on for anything beyond the `m_desc`
  fields already validated here (`type`, `fovY`, `aspect`,
  `stereoSeparation`, `stereoConvergence`) -- the matrix fields further into
  the struct (`m_viewMatrix` etc.) have not been re-verified for this build
  and their offsets may have shifted by the same missing 16 bytes or more.
