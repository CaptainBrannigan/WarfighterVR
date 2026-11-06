# The engine's own view list: native stereo for MOHW (2026-11-06)

All addresses are for MOHW.exe as dumped in `Scylla/MOHW_dump_SCY.exe` (image base 0x400000, the same values as
`sdk/mohw_offsets.h`). Found statically with Ghidra; nothing here has been confirmed live yet except where marked.

## Summary

Frostbite 2 renders a **list of views** every frame, and MOHW's world renderer has state for **two** of them. Three
switches in the shipped game turn that list into two views:

| Path | Switch | What the two views are | Needs |
|---|---|---|---|
| Split-screen test | `GameRenderSettings::SplitScreenTestViewCount` = 2 | "Root" in the left half, "SecondaryRoot" in the right half of the same target, each half the width | nothing |
| NVIDIA 3D Vision | `DxRenderer+0x44` (stereo active, set when NVAPI reports stereo activated) | "RootRight" and "RootLeft", both full size, `NvAPI_Stereo_SetActiveEye` between them | the 3D Vision driver duplicating every surface per eye |
| AMD HD3D quad-buffer | `DxRenderer+0x328` (via `IAmdDxExtQuadBufferStereo`) | the same two full-size views, the second eye's final pass drawn `GetLineOffset` rows lower in a double-height backbuffer | the AMD extension object and a frame-packed backbuffer |

For VR this is the same idea as the AvP mod's camera slots: the engine renders both eyes in one frame, from one
simulation tick, instead of one eye per frame. The split-screen path needs no driver support at all, so it's the
first experiment (`hooks/native_stereo.cpp`, setting "Engine two-view stereo", off by default).

## The frame, top down

1. `FUN_00896C20` (OFFSET_FRAMERENDERSUBMIT) calls `GameRenderer::joinUpdateJob`.
2. `GameRenderer` is the singleton at `DAT_02AC8FD0`, vtable `027959E0`. `joinUpdateJob` = vtable +0x14 =
   `FUN_008B4A30`. Per frame it calls each of the three render modules at `GameRenderer+0xEDC` (vtable +0x08), handing
   them `GameRenderer+0x50` (the `GameRenderViewParams`: view, prevView, secondaryStreamingView). Then the job groups
   "syncJobs", "buildJobs" and "renderDispatch".
3. The client world render module (vtable `027AC1B4`, created by `FUN_00A6B510`, which also creates the world renderer
   and the entity renderers) runs `FUN_00A70EF0` (vtable +0x08). It calls the **view list builder** and passes the
   list to the world renderer:
   - `FUN_00A6BDC0(this, viewParams, uint32* count, ViewDesc views[2])`: `__thiscall`, `RET 0xC`.
   - `WorldRenderer` vtable +0x08 = `FUN_0101CFA0(&{..., +0x14 count, +0x18 views}, ...)`.
4. The world renderer (`FUN_0101BB00` constructs it, 0x40000 bytes, vtable `02831CC8`) keeps per-view state for up
   to two views: blocks of 0x1B920 bytes from +0x91D0 and 0x4480 bytes from +0x80; `[+0x8CE4]` views active. It builds
   one job per view (`FUN_00FCD960`).

`GameRenderer+0x3C` is `GameRenderSettings` (what `getSettings`, vtable +0x34, `MOV EAX,[ECX+0x3C]`, returns).
Its reflected field records sit at `02BD06xx` (`{name, offset << 16, type}`): `SplitScreenTestViewCount` +0x44
(uint32), `LockView` +0xAA, `ResetLockedView` +0xAB, `FadeEnable` +0xAE, `FadeWaitingEnable` +0xAF.

## The view descriptor (0xE80 bytes)

| Offset | Field |
|---|---|
| +0x04 | name, `char*`: "Root", "SecondaryRoot", "RootLeft", "RootRight" (strings at `027AC1D0..027AC1F8`) |
| +0x10, +0x14 | target width, height |
| +0x20 | viewport x, y, w, h (uint16 each) |
| +0x30 | stereo eye: 1 = right, 2 = left (NVAPI's own values) |
| +0x40 | the view's `RenderView` (0x490 bytes) |
| +0x4D0 | a second copy of it (the locked view when `LockView` is set, the same camera otherwise) |
| +0x960 | the previous frame's `RenderView` (from `viewParams+0x490`) |
| +0xE00 | 16 dwords from `viewParams+0xDC0` |

`RenderView` starts with the `RenderViewDesc`: transform (16 floats, rows right / up / back / position), type +0x40,
fovY +0x48, near +0x50, far +0x54, aspect +0x58, ortho width/height +0x5C/+0x60, stereo separation +0x64, stereo
convergence +0x68, viewport offset +0x6C (2 floats), viewport scale +0x74 (2 floats). Dirty flags at +0x90:
`FUN_00701340` (build a RenderView from a desc) sets 0xD, `FUN_006FC2A0` (`RenderView::setTransform`) ORs 9 in,
`UpdateMatrices` (`FUN_00707CB0`) clears bit 0. **Every projection input is a desc field**: `UpdateMatrices` calls the
projection builder `FUN_00704C70(out, type, fovY, aspect, orthoW, orthoH, near, far, separation, convergence,
&viewportOffset, &viewportScale, ...)`, so a view can be given any asymmetric frustum by writing fovY, aspect and the
viewport offset (`hooks/projection_aspect_hook.h` has the formula).

## The builder in detail (`FUN_00A6BDC0`)

- Always: view 0 = "Root", full size, its `RenderView` built from the current camera (`FUN_00701340`), count = 1.
- `SplitScreenTestViewCount > 1`: count = min(value, 2). With 2, view 0's aspect is halved (`FUN_006FC1B0`) and its
  viewport becomes the left half; view 1 is a copy named "SecondaryRoot" with the right half as viewport and a test
  camera: view 0's turned (`FUN_0048A0A0`, a fixed angle) and moved. With 3 or 4 the viewports are quarters.
- `DxRenderer+0x44` set (stereo): count = 2, both views full size, `RenderView` rebuilt for each with
  `stereoSeparation = DxRenderer+0x4C` (negated for eye 0), `stereoConvergence = DxRenderer+0x48`, flags |= 9, eye
  index 1 ("RootRight") for view 0 and 2 ("RootLeft") for view 1.
- `FUN_00A6B510` (the module's creation, at a level's load) also reads the view count: 2 halves a width, 3 or 4 halve
  width and height. So the split-screen count must be set before the level loads.

## DxRenderer stereo (`DAT_02AC1E20`, OFFSET_DXRENDERER)

| Offset | Field |
|---|---|
| +0x40 | NVAPI stereo handle |
| +0x44 | stereo active this frame |
| +0x48 | convergence |
| +0x4C | separation |
| +0x50 | GPU vendor id (0x10DE NVIDIA, 0x1002 AMD) |
| +0x84 | `DxRenderSettings`: +0x41, +0x42 fullscreen, +0x4B `NvApiEnable`, +0x4D `StereoEnable`, +0x20 / +0x24 / +0x30 depth, separation and convergence scales |
| +0x318 | AMD extension (`AmdDxExtCreate11` from atidxx32.dll, `FUN_00731D30`) |
| +0x31C | its quad-buffer stereo interface (`GetExtInterface(2)`): +0x0C `EnableQuadBufferStereo(BOOL)`, +0x14 `GetLineOffset(IDXGISwapChain*)` |
| +0x320 | line offset of the second eye |
| +0x324 | the current eye's offset, added to the viewport Y whenever the target is the backbuffer (`FUN_00734480`) |
| +0x328 | quad-buffer stereo enabled |

- `FUN_0073ED90` (device creation): on NVIDIA with `NvApiEnable` and `StereoEnable`, `NvAPI_Stereo_IsEnabled`, then
  `SetDriverMode` and `CreateHandleFromIUnknown`.
- `FUN_007314D0`, per frame: with a handle, `NvAPI_Stereo_IsActivated` sets +0x44, and +0x4C and +0x48 come from
  `GetEyeSeparation`, `GetSeparation` and the settings.
- `FUN_00731B30`: with the AMD interface, +0x320 = `GetLineOffset(swapChain at +0xF4)`, and +0x44 = (+0x328 and +0x320
  != 0).
- `FUN_00733E80` = `setStereoEnabled(bool)` (the AMD enable, through a job when not on the render thread). The
  per-frame housekeeping `FUN_00739B00` (vtable data at 0277E698) switches it when `StereoEnable` changes, and only in
  exclusive fullscreen (`DxRenderer+0x18`).
- `FUN_00731480` = `setActiveEye(eye)`: `NvAPI_Stereo_SetActiveEye(handle, eye ? LEFT : RIGHT)`, and +0x324 = +0x320
  for eye 1 when the AMD path is on.
- `FUN_00CBE3D0`, a per-view pass that loops over both eyes with `setActiveEye` when stereo is on and writes the eye
  into `[+0x44]+0x2340`, is probably the UI: the game has stereo HUD settings (`StereoCrosshairRadius`,
  `StereoCrosshairMaxHitDepth`, `StereoCrosshairDampingFactor`, `StereoSoldierZoomConvergenceScale`).

NVAPI is linked statically. The stubs, found by interface id, are: `NvAPI_Stereo_Enable` 01893880, `IsEnabled`
018939E0, `CreateHandleFromIUnknown` 01893AA0, `DestroyHandle` 01893B60, `Activate` 01893C20, `Deactivate` 01893CE0,
`IsActivated` 01893DA0, `GetSeparation` 01893E60, `SetSeparation` 01893F20, `GetConvergence` 01894160,
`SetConvergence` 01894220, `ReverseStereoBlitControl` 01894760, `SetActiveEye` 018948F0, `SetDriverMode` 018949B0 and
`GetEyeSeparation` 01894A70. The old 3DMigoto `nvapi_log.txt` in the game folder shows the game calling
`Stereo_CreateHandleFromIUnknown` at startup (result -140: no stereo driver).

## The experiment (`hooks/native_stereo.cpp`)

Setting: View > "Engine two-view stereo (experimental, reload the level)", `NativeStereo` in the ini, default off.

- Present writes `SplitScreenTestViewCount = 2` every frame while it's on (the game's own value back when it's off).
- After the builder: when it made two views, both are given the head-centre camera (view 0's), moved by -IPD/2 /
  +IPD/2 along its right axis. Each view also gets its eye's real asymmetric frustum (from the same HMD data as the
  true-frustum mode), written into fovY, aspect and viewport offset. This is done for the current RenderView, its copy
  and the previous frame's (for motion vectors).
- While the engine keeps building two views, the per-eye machinery stands down:
  - CommitViewTransform adds no eye offset and doesn't arm the projection hook.
  - The parity stays on the left eye.
  - Present copies the left and right halves of the backbuffer into two textures and hands them over as one pair,
    with that frame's render-pose stamp.
  - Every frame goes to the desktop, paced to the headset's rate rather than twice it.

**Not adapted yet** (they still assume one eye per frame, the left one):

- **Weapon and body:** the first-person rewrite (`hooks/camera_matrix_test_hook.cpp`) will be off by one IPD in the
  right eye.
- **HUD:** the placement (`hooks/draw_trace_diag.cpp`) lays the HUD over the whole double-wide frame.
- **Markers:** the marker projection.

**To test:**

1. Turn the setting on.
2. Reload the level (restart the mission or load a checkpoint), since the world module reads the count at creation.
3. Check `mohwvr_nativestereo.log`. "the engine built 2 view(s)" and a "two views: left viewport (0,0 ...), right
   (W/2,0 ...)" line every 5 s mean the engine path works.
4. In the headset, look for:
   - correct depth;
   - no head-turn ghosting between the eyes (both come from one tick);
   - lighting, shadows and post effects intact in both halves. The split-screen test was never shipped, so a pass
     drawn full screen with one view's data would show up here.

The desktop window shows both halves side by side.

## What could go wrong

- **Post effects:** some full-screen passes may assume one view (post effects, reflections, the sky). The
  split-screen code path is a test feature.
- **Render cost:** both eyes are now rendered every frame, at half width each. Raising the game's resolution
  (e.g. 2x the eye width) keeps the per-eye resolution.
- **Halved width at load:** the halving at the level's load (`FUN_00A6B510`) applies to something the module
  creates. With the setting turned on mid-level, that object stays full size until the level is reloaded.

## Next steps, in order

1. Live first light with the split-screen path (above).
2. Make the viewmodel rewrite per view. The weapon and body batches are drawn with a camera of their own, built per
   batch (`FUN_008B2AB0`, through `RenderView::setTransform`, the hooked `FUN_006FC2A0`) from the game's raw camera,
   not from the view being drawn. So the eye can't be read from the batch's camera; it has to come from the view whose
   draw job is running. The world renderer builds one job per view (`FUN_00FCD960`, per-view state at
   `WorldRenderer+0x91D0 + i*0x1B920`). Mark the running view per thread there, then pick the eye offset in the rewrite
   from that mark instead of the parity. Check live first that the two views' batch draws run in separate jobs.
3. Draw the HUD per eye. Either render the game's 2D pass twice, into each half with that eye's HUD placement, or use
   the engine's own per-eye UI pass (`FUN_00CBE3D0`, gated on `DxRenderer+0x44`).
4. If split-screen rendering has full-screen-pass bugs, the AMD quad-buffer path is the complete one. It needs:
   - a fake `IAmdDxExtQuadBufferStereo` at `DxRenderer+0x31C` (Enable returns S_OK, `GetLineOffset` returns the eye
     height);
   - `DxRenderer+0x328` set without the fullscreen check;
   - a double-height backbuffer from the proxy.

   The engine then renders both eyes at full size, the UI per eye included, with the second eye's final pass at the
   offset. Present submits the top and bottom halves.
