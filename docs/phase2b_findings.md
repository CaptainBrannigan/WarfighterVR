# Phase 2B findings: identifying the view/projection constant buffer

## Question

Which GPU constant buffer(s) carry the view/projection matrix, and at what
byte offset, so Phase 2C can patch it per-eye before each draw pass?

## Method

Rather than trust `RenderView`'s matrix-field offsets (Phase 1 only verified
`m_desc`'s fields, not the matrices further into the struct -- and found the
struct's total size differs from the old header by 16 bytes, so those
offsets are unverified for this build), used an independent, first-principles
signature: for a standard D3D perspective projection matrix,
`proj[1][1] = 1 / tan(fovY / 2)`. `fovY` is already trusted (read live from
`GameRenderer::Singleton()->m_viewParams.view.m_desc.fovY`, confirmed in
Phase 1). This gives a distinctive float value to search for without relying
on any GPU/CPU offset correlation.

Hooked `ID3D11DeviceContext::Map`/`Unmap` (vtable indices 14/15) and, in a
follow-up pass, `VSSetConstantBuffers`/`PSSetConstantBuffers` (indices 7/16)
-- all obtained via the same dummy-device-on-a-background-thread technique
as Phase 2A's `Present` hook (`hooks/constantbuffer_hook.cpp`). On `Map`, if
the resource is a buffer with `D3D11_BIND_CONSTANT_BUFFER` and the map type
is a CPU write, stash the returned pointer/size. On `Unmap` (once the game
has finished writing new data into it), scan the buffer's floats for a value
within 0.01 of the live-computed `1/tan(fovY/2)`; any buffer that matches is
added to a small tracked set, and the `*SetConstantBuffers` hooks log
whenever a tracked buffer gets bound, with shader stage and slot.

## Result

Immediate, clean, repeated match, confirmed across two separate play
sessions:

```
resource=<buffer ptr> byteWidth=80 matchFloatIndex=5 (expected proj[1][1]=1.920982)
Row0: 1.0806  0.0000  0.0000  0.0000
Row1: 0.0000  1.9210  0.0000 -1.9210
Row2: 0.0000  0.0000 -1.0000 -0.0600
Row3: 0.0000  0.0000 -1.0000  0.0000
Extra:0.0003  0.0005  0.0000  0.0000
```

- **80 bytes** = a 64-byte 4x4 matrix (16 floats) plus one extra 16-byte
  float4 (values `0.0003, 0.0005, 0, 0` -- small, non-zero, purpose not yet
  identified; candidates include a jitter/TAA offset or a depth-related
  constant).
- **`floatIndex=5`** is exactly row 1, column 1 in a row-major 4x4 layout --
  precisely where a standard D3D perspective projection puts
  `1/tan(fovY/2)`. Structural match, not just a numeric coincidence.
- **Consistently bound to VS (vertex shader) slot 0.**
- **Bit-for-bit identical across 8 samples spread over ~14 seconds**
  (throttled to one sample per 2s) covering the menu-to-gameplay transition.
  Since a combined view-projection matrix would change with camera rotation
  and a pure projection matrix would not, this is strong evidence it's
  **projection-only**, not combined view-projection -- Phase 2C can treat
  view and projection as separately patchable.
- The buffer is mapped and rewritten every frame (the scan's log cap was hit
  within ~1 second of hook install both times), consistent with being the
  active per-frame camera constant buffer.
- Confirmed stable across three separate sessions now (Phase 2A alone, then
  Phase 2A+2B together, then this refinement pass): no crashes, clean
  `DLL_PROCESS_DETACH` on quit each time.

## Caveat / not fully closed

The 8-sample window in the refinement pass likely captured mostly
menu/loading time rather than confirmed active gameplay with real camera
rotation (the user needed a few seconds to get from launcher to menu to
in-game, and the log-count cap was hit within that window, before real
play). The "identical across time" evidence is still meaningful (menu
transitions alone typically don't hold a camera perfectly framerate-locked
static either), but a follow-up pass with the sample cap raised and/or
delayed until confirmed active gameplay would make this fully conclusive.

## Not yet resolved

- **What the extra float4 (bytes 64-79) contains** -- small stable values,
  not yet identified.
- **The exact projection convention** -- rows 2/3 (`[0,0,-1,-0.06]` /
  `[0,0,-1,0]`) don't match a naive symmetric-frustum layout at first
  glance; likely a reversed-Z or infinite-far-plane convention, or the
  matrix is stored transposed relative to the "obvious" reading. Needs
  correlating against known near/far values (`RenderViewDesc::nearPlane`/
  `farPlane`, already in `sdk/renderview.h`, offsets `0x50`/`0x54` relative
  to `m_desc` -- validated indirectly via `m_desc`'s other fields in
  Phase 1 but not these two specifically) before Phase 2C computes
  replacement per-eye matrices in the same convention.
- **The buffer pointer itself is per-session/per-allocation** -- Phase 2C
  should re-identify it by the same signature at runtime, not hardcode
  today's pointer value.

## Follow-up: multiple buffers match the signature (resolved by decision, not further verification)

A longer, real-gameplay session (raised sample cap to 90, ~3 minutes,
confirmed active play with camera rotation) surfaced **five other buffers**
also matching the single-float `proj[1][1]` signature: sizes 352B (34 hits),
4096B (19 hits), 12800B x2 -- likely a double-buffered pair (11+9 hits), and
1472B (1 hit). This is a real methodological gap: the large buffers
(12800 bytes = 3200 floats each) make a coincidental near-match plausible
across dozens of samples, since only one float out of thousands needs to
land within the 0.01 tolerance. The 80-byte buffer remains the strongest
candidate on structural grounds (small, the *entire* surrounding matrix
forms a coherent projection matrix, not just the one matched float; a large
buffer would need the same 4-value pattern to appear together by chance,
much less likely). Rather than build a stricter multi-value structural
matcher and re-verify, the user opted to accept the 80-byte / VS-slot-0
buffer as sufficient and proceed to Phase 2C, treating the other matches as
probable noise. If Phase 2C's patched output doesn't visually behave like a
projection change (e.g. wrong FOV/eye separation direction), revisit this
and build the stricter matcher before assuming the hook itself is wrong.

## Recommended next step for Phase 2C

Correlate `nearPlane`/`farPlane` against rows 2/3 to nail down the exact
projection convention (reversed-Z vs. standard, row-major vs.
transposed-in-CB), then design the per-eye patch: since this buffer is
projection-only, Phase 2C likely needs a *second* investigation pass (same
Map/Unmap signature technique, searching for a translation-bearing or
rotation-varying matrix elsewhere) to find the separate view matrix's
constant buffer before both can be combined per-eye.
