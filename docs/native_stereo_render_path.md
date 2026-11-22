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

Setting: View > "Render mode" = "Engine two-view" (was the toggle "Engine two-view stereo"), `NativeStereo` in the
ini, default off (alternating eye).

**One DLL, two render modes (2026-11-12).** The user chose a single DLL with a mode switch over two DLLs, since the
engine mode needs the whole alternating-eye path anyway (2D frames, the driving missions).
- The dashboard row is a two-way choice ("Alternating eye" / "Engine two-view").
- A level builds its targets for the mode in force at its load, so the client world module's constructor
  (`FUN_00A6B510`, `OFFSET_CLIENT_WORLD_MODULE_CTOR`, `RET 0x10`) is hooked to record each level's mode
  (`NoteLevelRenderMode`). While the selection differs, a note row under the setting reads "Changed: reload the level
  to apply" (`MenuSettingInfo::isNote`).
- The menu shows only the selected mode's rows (`MenuEntry::mode`): screen effects in both eyes, depth of field and
  the eye-sized backbuffer are engine-only.
- The HUD's numbers are kept per mode (`kPerMode` in `sdk/settings.cpp`; the engine mode's under `...TwoView` keys,
  seeded from the alternating-eye values when missing): menu/movie screen size and distance, Hud Element Scale, HUD
  scale, depth, IPD and offsets. The getters return the selected mode's; the driving missions always the
  alternating-eye ones.
- The HUD group moved into the first column, under View.

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

**Per view (2026-11-08, built, not yet seen in the headset):**

- **Weapon and body:** the first-person batches aren't drawn from the view's RenderView but from its raw camera, the
  ViewDesc's +0xE00 (`FUN_008B2AB0` at 0x8B2CA2: `MOV EDX,[ESI+4]; ADD EDX,0xE00`, then `setTransform`, RETURN 0x8B2CB8).
  The builder hook moves that camera to each eye too. The batch hook sees it set right before the batch-relative copy it
  rewrites, and matches its position against the eye cameras written for the last 4 frames to know the eye. With that:
  - the rig's eye offset comes from that eye (`SetThreadEyeOverride`);
  - the gun's camera gets that eye's frustum (`projection_aspect_hook.cpp`, `GetNativeEyeProjection`). The true-frustum
    mode that does this per frame is off in two-view mode.

  `mohwvr_cameramatrix.log` counts the batches matched to each eye every 5 s. "Not matched" batches keep the left eye.
- **HUD:** `hooks/ui_pass.cpp` hooks the list replay `FUN_00CBB4E0` and, in two-view mode, replays the HUD list twice,
  with the engine's own per-eye clear (render interface vtable +0x08, flags 0x200) between: that's what `FUN_00CBE3D0`'s
  eye loop does when the engine's stereo is on. Before each HUD draw into the backbuffer, `hooks/draw_trace_diag.cpp`
  sets the viewport to that eye's HUD rectangle inside that eye's half. The reticle hide uses the replay's draw numbers;
  the after-draw-1 placement and the whole-frame 2D placement are off. Logs: `mohwvr_drawtrace.log` the first
  placements, `mohwvr_uipass.log` the replay count, `mohwvr_fovscale.log` the gun's per-eye frustum ("VIEWMODEL NATIVE
  TWO-VIEW").

**Not adapted yet:** the in-world markers. Both halves are drawn from one marker projection (the game's own), so in
two-view mode they're left unmapped.

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

## First light (2026-11-08)

The user's first run in the headset: the game rendered both eyes at 120 frames a second (one game frame per display
frame), and the HUD was replayed into both halves every frame. Three problems:

- **The menu:** the left and right eye looked rendered differently, and changing the IPD moved only the right eye.
- **In play:** the gun and arms looked tiny, and the stereo came and went with flickering.
- **A crash** shortly after a cutscene handed the controls back, in all three runs.

**The crash, solved:** all three dumps crashed in `memcpy` (MSVCR100 +0x10A58 / +0x10A80 / +0x10A5D) called from
`FUN_00743F70` (a draw list's world transforms, count * 64 bytes), from `FUN_00CEFBA0` ("WaterSurface", one LOD of the
ocean), from `FUN_00CF7BC0` (the "oceanWaterDraw" job). The counts were -8 and -202: a LOD range running backwards.
The water module (vtable 027F881C, `FUN_00CF9080`) kicks its jobs once per view from `FUN_00CF2070` (vtable +0x1C),
and they all share one set of buffers:

- three "oceanWaterCull" slices (`FUN_00CF6B90`) write each patch's LOD and the counts;
- "oceanWaterPreDraw" (`FUN_00CF0DA0`) zeroes the LOD ranges at module +0x830 and refills them, and fills the patch
  transforms (64000 bytes, 1000 patches, at module +0x828);
- "oceanWaterDraw" builds the view's water draw lists from both.

With two views, the second view's culls and pre-draw ran while the first view's draw was reading. `hooks/ocean_water_
guard.cpp` makes culls and pre-draws take a lock exclusively and draws share it while NativeStereo is on, and checks
every LOD draw's range against the transform buffer (a bad one draws nothing). `mohwvr_oceanwater.log` counts the
waits and any bad range.

**Diagnostics added for the other two:**

- `mohwvr_fovscale.log` "NATIVE WORLD CAMERAS": every 5 s, how many world camera updates with each eye's frustum were
  at the left eye's written camera, the right eye's, the head centre, or elsewhere (`NativeStereoClassifyWorldCamera`).
  If the left eye's world turns out to be drawn from the head centre, that explains both the IPD moving one eye only and
  the small-looking gun (drawn from the eye camera, 3.2 cm off the world's).
- `mohwvr_native_menu.bmp` and `mohwvr_native_play.bmp`: the frame handed to the headset, both eyes side by side at half
  size, saved once after 6 s in the menu and once after 10 s of play.
- `mohwvr_drawtrace.log`: a HUD replay draw sampling a frame-sized texture isn't placed; those are now logged with the
  viewport they keep.

## Second run (2026-11-08)

No crash: the water lock caught 2,100 to 3,000 collisions every 5 s (each one a crash before) and no bad range.

**Where each eye is drawn from:** right. Every world camera with the left eye's frustum was at the left eye's position,
every right one at the right eye's, none at the head centre. So the head-centre theory was wrong. 3,600 every 5 s per
eye were "elsewhere": they have the eye's frustum but not its position (planar reflection, envmap faces or the sky are
candidates).

**The play frame dump** (`mohwvr_native_play.bmp`, both eyes): block matching between the halves, on a high-passed image:

- The city at the horizon matches at exactly 233 px of 960 (half size). That's the disparity infinity should have with
  the Quest 3's frusta: the eyes' left edges are at tangents -1.3764 and -0.8391, 0.5373 apart, over a width of 2.2155:
  0.2425 of the width, 233 px.
- The clouds above it don't: 146 to 176 px, and the right eye's features up to 50 px higher, growing towards the
  horizon. A camera rotation doesn't fit (26 px rms at best), and neither do a symmetric frustum or the other eye's.
  The game draws its sky in `FUN_00FD9020` (callers `FUN_010130C0` per view, mode 0, and `FUN_0100DDC0`, the dynamic
  envmap, mode 5): it builds its own copy of a camera (`FUN_00701340` from its third argument, the per-view block +0x50),
  rebuilds it at 0x00FD90A5 (UpdateMatrices, returning to 0x00FD90AA), and draws with `g_invViewProjMatrix`,
  `g_screenSizeInv`, `g_camPos`, per-corner rays and the cloud layers' UV transforms (`FUN_00FD4580`). With the
  engine's own stereo on (`DxRenderer+0x44`) it zeroes the copy's separation (+0x64) and blends the corner rays by eye.
  Which camera that copy really is in two-view mode is open: `mohwvr_fovscale.log` "NATIVE CAMERA REBUILDS" now lists
  every camera rebuilt while two-view is on, by the function rebuilding it (the sky's is marked), with its frustum per
  eye, position and orientation.

**Movies, menus, loading screens:** the intro movie was drawn across the whole double-wide frame, so each eye got half
of it (`mohwvr_native_menu.bmp`); the menu's background filled each eye while its items floated; and the floating
screen those get in one-view mode sat at the HUD's depth and IPD scale, 0.1 m x 0.2 = 0.5 m, cross-eyed ("too far
inward in both eyes"). Now:

- whole 2D frames (`Is2DFrame`: a movie, a menu, a loading screen, a cutscene before the player has control) run with
  the game's own view count, so they get the floating screen; two views only in 3D frames. Switched at run time
  (`NativeStereoOnPresent`), after the state holds 300 ms, at most every 2 s. The world renderer resizes its targets
  when the views' size changes (`FUN_0101CFA0` compares the largest view with +0x89CC / +0x89D0), and the world
  module's creation only sizes the first frame's (`FUN_00A6B510` -> `FUN_0101CB80`);
- the floating screen has its own distance, "Menu / movie screen distance" (`MenuScreenDepth`, 2.5 m), fused with the
  rendering eye offset;
- in two-view mode a whole 2D frame keeps the full frame's aspect, not the half's (it came out half as wide).

**The HUD** with placement off was drawn into both halves, "but moves down when my head moves up". Open: if it drifts
during head motion and settles, that's the render-pose stamp (the frame is shown as rendered for the head pose of the
time, the HUD with it, up to 2 to 3.6 deg behind in this run); the fix is the HUD on its own layer. The in-world
markers aren't adapted to two-view yet either.

## Third run (2026-11-08)

What the user saw: with HUD placement on, the HUD and the menus and loading screens are right. The ammo and player
info follow the head; the in-world markers don't, and they scale with the HUD. The TV still shows both halves. The gun
still looks tiny, better at IPD 0.5. The clouds look like two layers, one per eye: the right eye has a glare behind
the tower that the left doesn't, and looking up and down the clouds at the edges rise and turn while the centre stays.
On a head turn, world meshes move slightly with the head.

**The clouds, solved:**

- The sky's camera is right: `FUN_00701340` copies only the view's desc (0x90 bytes) and sets the dirty flags to
  0xD, so `FUN_00708370` rebuilds it (UpdateMatrices returns to 0x00708381, not 0x00FD90AA).
  `g_invViewProjMatrix` is that copy's +0x450 (inverse projection times the camera); `g_camPos` is its +0x30; and
  `g_screenSizeInv` is 1 / the current viewport's width and height (render interface vtable +0x10).
- The fault is the quad's per-corner rays. The sky is a full-screen quad, vertices at frame +0xE0, 24 bytes each
  (x, y, z, ray). Vertex 0 is (-1,-1), 1 is (1,-1), 2 is (1,1) and 3 is (-1,1); vertices 4 and 5 are copies of 0
  and 2, so the split runs along BL-TR.
- Each ray is a corner's far point minus its near point: the copy's +0x100..+0x17F, frame +0x480..+0x4F0. Vertex 0
  is +0x480 - +0x490, 1 is +0x4C0 - +0x4D0, 2 is +0x4E0 - +0x4F0, 3 is +0x4A0 - +0x4B0. The game normalises each
  ray before writing it into its vertex.
- Interpolated unit vectors aren't perspective-correct. A model of exactly that predicted every block-matched cloud
  patch of the dump to 1-3 px: dx -143 to -219 and dy -9 to -65, against -233 / 0 for the city.
- `hooks/sky_rays_fix.cpp` hooks 0x00FD9DC9, where the 3D Vision branch joins, and rewrites the four rays
  unnormalised. Each is checked against the game's own normalised one first.
- The same warp explains the haze. The same buildings are up to 60 grey levels brighter in the right eye, and that
  matches the sky's vertical warp near the horizon, where the haze brightens sharply.
- Setting: View > "Exact sky rays". Log: `mohwvr_sky.log`. This should also fix the alternating-eye mode's old
  "skybox/clouds per-eye offset".

**The tiny gun, solved:**

- `FUN_008B2AB0` makes each batch's camera from the view's raw camera (+0xE00) minus the batch origin (+0x60):
  `SUBPS XMM0,[EBX+0x60]` before the setTransform that returns to 0x8B2D98. So M is the eye in both modes.
- The rig isn't posed against the same camera in both modes:
  - One eye per frame, the game's camera is the eye, so the rig is posed against M, and the anchor T(e) moves it back
    to the head centre.
  - In two-view mode the game's camera is the head centre, so the anchor moved the rig a further e the wrong way in
    each eye. That doubled its parallax, which is why IPD 0.5 helped.
- Fix: in two-view mode the camera the rig was posed against is M moved back along its right by e. The anchor
  (which comes out as M without roll), the weapon drive, the magazine and the arms all use that camera.
- The arms' IK (solved once per batch for both eyes) gets the same toRig from both eyes now. The body's upright turn
  and the elbow model work from that camera's position too.

**Markers:**

- Both halves replay one HUD, and the game projects its markers with its own camera: the head centre, 50 deg 16:9
  (the eye frusta go to the views only).
- `MapForTwoView` takes each marker's direction from that projection (1/tan from the viewProj columns). It puts the
  marker at the HUD position whose two eye images average to that direction.
- So a marker shows in the right direction, at the HUD's depth.

**Open:** world meshes moving slightly with a head turn. The render frustum equals the HMD's (FrustumScale 1). The
user still has to say whether alternating mode does it too, and whether it's a lag that settles or a steady slide.

## Fourth run (2026-11-08)

The sky and the weapon scale are confirmed fixed (`mohwvr_sky.log`: 2,706 sky quads in 30 s, none skipped). The
desktop window's one-eye view built for the third run was removed: the user didn't want it ("never a problem"), and
it cost a full-screen draw every frame. The window shows both halves again. The "NATIVE CAMERA REBUILDS" diagnostic was
removed too: it answered its question (the sky's camera is its view's own).

**Body flicker** ("the body is flickering to a different ghost like frame somewhat frequently"):

- Each view's body batch is its own copy of the player's bones. `FUN_008BB050` copies them from what
  `FUN_009551F0` returns, each view in its own job and thread: 240 body batches a second at 120 fps, on six threads.
- A game tick landing between the two copies gives the eyes different poses.
- And `AlignRigToBones` read the frame bones from the shared read, which could be the other view's copy. One eye's
  body turned by up to the alignment's 3 deg, or wasn't aligned at all past the wobble gate, for that frame.
- The weapon isn't affected: the drive cancels the rig camera out.
- Now:
  - `NativeStereoEyeForCamera` also returns the built frame (newest first, an exact match ends the search).
  - `SyncTwoViewBodyPose` gives the second eye's body batch of a frame the first one's pose, before anything moves
    it.
  - Each eye aligns to its own batch's frame bones (`ReadBatchFrameRows`; weapon batches use their thread's body
    rows from the same frame).
- `mohwvr_rig.log` counts, every 5 s, the frames with a batch per eye and how many really had different poses: the
  test of this explanation.
- **Result (2026-11-09 run):** 311 to 414 frames every 5 s with a batch per eye, 0 with different poses: the two
  copies are always made from the same tick. And "the body did flicker constantly", so neither the pose sync nor the
  per-batch rows was the cause.
- **The likely cause, fixed next:** the arm IK's once-per-batch check (`ApplyArmIk`).
  - A batch is drawn by several passes. A pass skipped the solve when the array's right hand held what was last
    written, and that record was one global.
  - With two views each eye has its own array, drawn in several passes on different threads. The other eye's solve
    replaced the record, and the controller had moved a little in between. So this eye's later passes solved again on
    bones already on the controllers, and the arms (part of the upper-body mesh) moved twice, a pass at a time.
  - Now there's one record per bone array (`IkAlreadyWritten` / `NoteIkWritten`, 8 slots).

**HUD commands, read from that run's dumps:** block A (data block +0x1A0) is the draw's 4x4 transform to clip space.
The HUD's flat items have row 0 = (2/width, 0, 0, x) and row 1 = (0, -2/height, 0, y), so x and y are where the item
sits in clip space. The reticle is four draws at (0, ±0.139) and (±0.078, 0), shader ids 191/6 with two vertex
constants; items at the left edge sit at x ≈ -0.97. In a frame with three markers mapped, four draws (#7-#10) had
perspective-looking values (m02 -0.429, translations in the thousands). The dump now prints all 16 floats, so their
screen positions can be computed and matched to the markers. Block B and its handle are zero: the game fills no
right-eye data while its own stereo is off.

**Markers** are in the right direction but doubled ("slightly too far inside of each eye"): they share the HUD's
depth, 0.1 m at HudIpdScale 0.2, so 0.5 m. One recorded HUD list serves both halves, and its draw commands are
single draws (`FUN_00CBB4E0` case 0, `FUN_00CBB3C0` "Ui/drawIndexed"), not movies.

`FUN_00CBB100` sets each command's state from its data block (command +0x10):

| Offset (data block) | Contents |
|---|---|
| +0xB4 | count of vertex shader constants; entries of 32 bytes from +0xC0 (handle, float4 at +0x10) |
| +0x120 | count of pixel shader constants; entries from +0x130 |
| +0x190 / +0x1A0 | a handle and data, passed to the renderer's vtable +0x2C |
| +0x1E0 / +0x1F0 | the same for the right eye: used instead when the engine's stereo is on (`DxRenderer+0x44`), the renderer state's +0x2340 is 1 and +0x1E0 is set |

So the commands can carry per-eye data. `mohwvr_uipass.log` now dumps the HUD list's commands every 10 s in two-view
mode (the constants and both blocks) next to the markers mapped in the same stretch. From that, the marker draws can
be found and given their own depth per eye.

**What the dumps showed (2026-11-09 run):** no draw command sits at a marker.

- In play the HUD list has 7 to 11 commands. #1-#6 are the reticle (four lines at ±0.139 / ±0.078, two centre
  dots). The rest are flat items.
- #7-#10 are the perspective ones. Rows 2 and 3 hold cos 10° / sin 10° (0.9848 / 0.1736) with translations near
  27,660, so they're a panel turned 10° about the vertical. Solved for its vertex range, it spans the frame from its
  left edge.
- A marker mapped at HUD (0.949, 0.454) in the same stretch has no command whose transform puts it there.
- So a marker's screen position is in its vertex data (or in a panel's own movie-clip layout), not in a per-draw
  transform. Giving markers their own depth per eye needs another way in:
  - re-recording the HUD per eye with the marker projection moved for that eye;
  - or the engine's own per-eye path (block B), which needs its stereo state on.
- **Correction (2026-11-22): a marker does have its own draws.** The reading above didn't divide the translations by
  w. The perspective draws' origins (A[3] / A[15], A[7] / A[15]) sit on the mapped markers: in the 2026-11-14 log, 10
  of 15 on-screen markers had such draws, x within 0.0007 of the mapped position. The other 5 had no draw anywhere
  near them (likely not shown). So a marker can get its own depth by shifting its draws sideways per half replay
  (row 0 += shift x row 3 of block A). Not built yet.

## Particles in one eye (2026-11-09)

"A particle effect for heavy dust was only visible in the right eye."

- **Not a race like the ocean water's.** The emitter system keeps a state block per view: `FUN_01C03C90` gives each
  view `index * 0xF200 + 0x150` of the module and kicks its "emitterBuild" and "emitterDraw" jobs, and the module
  has room for exactly two blocks, its view count at +0x1E550.
- **Each view gets two quad lists.** `FUN_01C00290` builds "emitterQuadMain" and "emitterQuadHalf", plus
  "emitterDrawLists" from them.
- **The half-res list goes through a shared-target pass.** It's drawn in the world renderer's "worldHalfRes" pass,
  set up per view at half the view's size (`FUN_0101D780`, around 0x0101EF09), with its own half-res depth
  (`zBufferHalfRes`, `WriteHalfResZ`). It's then composited back (`HalfResUpsampleBilateral(Tiled)`).
- **Why that pass is the suspect:** big soft quads like heavy dust go to the half-res list. With both eyes in one
  frame, one view's clear, depth downsample or composite region can cost the other its particles.
- **Test and fix in one:** EmitterSystemSettings `QuadHalfResEnable` is turned off while two views render, which
  sends every quad to each view's main pass. The settings object is the emitter manager's (global `DAT_02B44420`)
  +0x0C, 0x68 bytes; `QuadHalfResEnable` is at +0x63, from the reflected field records at 0x02BF8058.
- **Setting:** View > "Particles at full resolution (two-view)", `FullResParticlesTwoView`, on by default. The
  game's value comes back for single views, and the switch is logged in `mohwvr_nativestereo.log`.
- **If the dust shows in both eyes now, that's the cause.** The cost is fill rate in heavy effects; a proper
  half-res fix would then go into that pass.
- **Result (2026-11-09):** the log shows "QuadHalfResEnable 1 -> 0", and the dust was still only in the right eye,
  so it isn't the particles. The user: "I don't believe it's a 3d particle effect, but rather a UI effect". The
  setting was removed, since it only cost fill rate; the offsets stay in `sdk/mohw_offsets.h` for reference.

**The screen effects** (ScreenEffectComponent: textures over the view, full frame or frame parts, with
DustAccumulationCurve / DustAccumulationLifeTime among their settings) are the likely dust.

- **Where they're drawn:** `FUN_00FD2FA0` (`__thiscall`, the world renderer's +0x8C80; the view's ViewDesc copy, its
  viewport, its visual-environment state; RET 0xC). It's called from the per-view render `FUN_010130C0`, gated on
  the view state's +0xA20 / +0xA24, two ref-counted resources. +0xA24 is drawn by `FUN_00FD2030` first.
- **Per view, on one thread:** `FUN_0101F5D0` calls the per-view render for each view in turn.
- **Each view's state is a copy of one global state.** Each view has a visual-environment state, 0xAF0 bytes at its
  block +0x1AD30, the sky's parameters among them. `FUN_0101CFA0` copies it every frame from the global state, the
  manager `DAT_02B183A0` +0xB20 (`FUN_00FCA970`), with `FUN_00FDA9E0`. Its sub-copy `FUN_00FD4DD0` AddRefs both
  screen effect resources, so both views should have the effect.
- **How it's drawn:** the effect's draws are built per call (vertex data from the frame allocator, the view's own
  viewport in "shaderState/viewport"). They're kicked as a pass of their own (`FUN_00779D40`, the dispatcher
  `DAT_02AC2D00`'s vtable +0x34) into whatever target the render interface `DAT_02AC16F4` has bound at the time.
- **Diagnosed (2026-11-09):** `hooks/screen_effect_diag.cpp` hooks `FUN_00FD2FA0` and logs, while an effect is
  active, every call: which half, the two resources, the bound target. Log: `mohwvr_screeneffect.log`. Both views
  call it every frame (left 971, right 971 in 10 s), each with its own half's viewport, the same resources, the same
  bound target (FEDD0100), on the same thread. So the left half's effect is drawn, then lost later in the frame, to
  something in the right view's own render that rewrites the left half too (a whole-target copy, clear or post pass).
- **Fix to test** (View > "Screen effects (dust) in both eyes", `ScreenEffectsBothEyes`, on by default): in two-view
  mode the left view's call is held and made right after the right view's, on the same thread, with the left view's
  own arguments (its per-view block stays valid all frame). A held call whose right view doesn't come is dropped, not
  made in a later frame. The 10 s summary line counts held, made late and dropped calls.
- If the left eye's dust is still missing with that, the rewrite comes after the right view's effect too, and the
  next step is a GPU capture to find the pass.
- **Confirmed (2026-11-10):** the dust shows in both eyes with the held call.
- **The damage effect (2026-11-11):** the view state's +0xA24 resource is `FUN_00FD2030`'s "damageEffect", a
  blur/zoom over the live image, which a level-intro cutscene also used. In two-view it showed "a smaller portion of
  the screen stretched over most of it, somewhat transparent" over both eyes: it assumes one view over the whole
  frame. The final-pass capture showed its own shader pair appearing as +0xA24 was set. A skip of it (clearing
  +0xA24 for the call) changed nothing (the user), so it was removed again (2026-11-12); whether the damage effect
  itself is right in two-view is unchecked.

**Depth of field doubles the image (2026-11-11).** The user: "the cutscene artifact only occurs once the camera moves
onto the player body ... something is getting duplicated into the eye render".

- Frame traces (`mohwvr_frametrace.log`, one frame every 4 s in two-view 3D frames) before and after the camera
  reached the body differ by passes, once per view: the whole 3840x2160 side-by-side frame downsampled into a
  1920x1080 target, blurred, then a full-frame pass reading the blur, the full image and the 3840x2160 R32 depth: a
  depth-of-field blend. Its downsample holds both eyes, so each eye blends both eyes' images in.
- The switch is the game's `GlobalPostProcessSettings` (type record `02B616A4`): `ForceDofEnable` +0xE8 (Int32, -1
  from the constructor = the game's choice), `DiffusionDofEnable` +0x1BD, `ADSDoFEnable` +0x1A9.
- The live objects: GameRenderer +0xEDC slot 1 (the client world module, vtable `027AC1B4`) → +0x18 the world
  renderer (vtable `02831CC8`) → +0x34 its post-process module → +0x14 (registered as "PostProcess") and +0x10, each
  vtable `0277BF7C`.
- `NativeStereoOnPresent` sets ForceDofEnable 0 and the two booleans false on both while two views render, unless
  View > "Depth of field (doubles the image)" (`DepthOfFieldTwoView`, off by default) is on, and puts the
  game's values back when two-view stops. Logged in `mohwvr_nativestereo.log`.
- **Confirmed (2026-11-12):** "no artifacts in the in game cutscene now".
- The saved values survive frames where the settings can't be reached (a level load). The first version forgot them
  there and saved its own zeros as the game's. While off, anything not already off is taken as the game's and
  turned off again; going back, the game's values are only written over the mod's own zeros.
- **Render scale from SteamVR (2026-11-12, experimental, off by default):** `GameRenderSettings` (type record
  `02B66190`, the object at GameRenderer +0x3C) has `ResolutionScale` +0x2C (Float32), the engine's 3D render scale;
  the profile's `GstRender.ResolutionScale 4` is probably an index into its menu's list. With View > "3D render scale
  from SteamVR resolution" on, `NativeStereoOnPresent` sets it to SteamVR's recommended eye height
  (`IVRSystem::GetRecommendedRenderTargetSize`, its resolution slider included, read every 2 s) over the backbuffer's
  height, in steps of 0.05 with no cap of the mod's own (SteamVR's resolution setting is the limit), and puts the
  game's value back when off. It supersamples the 3D scene
  only: the HUD and the image handed to SteamVR stay at the backbuffer's size. Untested whether the engine applies it
  live or only when its targets are rebuilt. `WorldRenderSettings` also has `RenderScaleResampleEnable` +0x242 and
  `RenderScaleResampleMode` +0x1C0 (the filter back down).
- The automatic frame traces were removed again; the trace keeps the first-draw inputs (pixel shader, textures,
  viewport) and writes to `mohwvr_frametrace.log` when armed.

**Eye-sized backbuffer (2026-11-12, experimental, off by default; View > "Backbuffer at SteamVR eye size").**
In two-view each eye is half the backbuffer: 1920x2160 of 3840x2160, against SteamVR's 2912x3112 per eye (its
resolution slider included). Alternating eyes gave each eye the whole 3840x2160. The HUD shows it most: its rectangle
is 1056x1188 pixels per half, against 2112x1188 with alternating eyes, which is why the driving missions' HUD (still
alternating eyes) looks sharper. The render scale can't help: it renders the 3D scene larger, then it's shrunk back into
the half. Last run's cost for comparison: two-view at render scale 1.45 rendered 5568x3132 and ran 67-83 fps.

How the game sizes its swap chain (Ghidra; `sdk/mohw_offsets.h` `DXRENDERER_*`):

| Where | What |
|---|---|
| `FUN_007392a0` | creates the swap chain from the output mode (DxRenderer +0x58 width, +0x5C height) |
| `FUN_00739680` | applies a mode: the settings' size (DxRenderer +0x84 → +0xC/+0x10, 0 = the desktop's), checked against the monitor's modes (`FindClosestMatchingMode`); windowed, the window's client size (+0x38/+0x3C). Writes it to +0x28/+0x2C |
| `FUN_00737dd0` | `ResizeBuffers(+0x28, +0x2C)` (the game's own assert text: "renderer->m_screenInfo.windowWidth, windowHeight"), then +0x20/+0x24 = +0x28/+0x2C |
| `FUN_007380f0` | the full reset: releases the backbuffer and its views (+0xF8/+0xFC/+0x100), ClearState, Flush, resize, rebuilds the views (`FUN_00733fc0`) |
| `FUN_00739b00` | the per-frame display update, which presents: resets whenever +0x20/+0x24 differ from +0x28/+0x2C; while +0x18 (fullscreen as the game believes it) is set, re-applies the mode whenever the settings' size differs from +0x28/+0x2C |

With the setting on, `NativeStereoOnPresent` writes SteamVR's eye size, two side by side (5824x3112 at the last
run's 2912x3112), into +0x28/+0x2C, and into the settings' size while the game believes it's fullscreen
(KeepGameWindowed answers its request with S_OK without carrying it out). Without that, it would re-apply its mode
every frame. The next frame's display update then runs the game's own reset to that size.
- Only with KeepGameWindowed and the swap chain really windowed: in exclusive fullscreen the size has to be a monitor
  mode.
- The game's own sizes are saved and put back when the setting is off. A size the game sets itself meanwhile (its
  video options) becomes the one put back.
- If the game sets its size back more than 5 times in 10 s, it gives up (logged). Switching the setting off and on
  tries again.
- With the backbuffer at the eye height, "3D render scale from SteamVR resolution" works out to 1.0.
- The settings' size goes into the profile at exit. On the next launch without the mod, the game snaps it to the
  nearest monitor mode itself (`FUN_007392a0`).
- Logged in `mohwvr_nativestereo.log`: "eye-sized backbuffer ...", "backbuffer WxH -> WxH" and the two views'
  viewports.
- **First test (2026-11-12):** the resize works live. The views followed within a second, and SteamVR slider changes
  too (7872x4212, 8344x4464, 5168x2764). But switched on mid-level it only showed after a level reload (the user),
  likely because a level builds its render targets at its load (the client world module, `FUN_00A6B510`). A version
  that held a new size back until the next menu or loading screen was undone at the user's request: changes apply at
  once. The game re-applied its own 3840x2160 mode three times during the load (the settings' size snapped back to a
  monitor mode); each was put back.
- **Menus got smaller the higher the resolution.** The game lays its menus out in pixels in one screen rectangle,
  `FUN_00CB7480`: DxRenderer +0x1A0 (u16 x y w h) inside the safe area (the Scaleform state's +0x2344/+0x2348, the
  `ScreenSafeAreaWidth/Height` options). Every movie's viewport comes from it (the UI manager's `FUN_008fcfd0` ->
  `FUN_008f4c70`, each movie's constructor `FUN_00a75220`). A scale above 1 divides that rectangle
  (`hooks/ui_pass.cpp`), so the UI lays out as on a screen that much smaller: menus that much bigger, still drawn at
  the full resolution. That works because the mod's placement sets the viewport the UI is drawn into. The in-world
  markers stay right: their caller `FUN_016ab1c0` maps a fraction of the screen into the HUD movie's own visible
  rectangle. Only with HUD placement on. The HUD movie itself scaled with the screen already.
- **The scale follows the resolution (2026-11-22).** It is 1.5 x the rectangle's width / 5824 (the author's 1.5 at
  5824x3112), times HUD > "Hud Element Scale" (`UiLayoutSize`, default 1 = automatic; it replaced the absolute
  `UiLayoutScale`). That's a constant layout width, since a whole-2D frame's width in the headset is fixed by
  `MenuScreenScale`. Below 1 the rectangle can't be divided: the renderer's BeginDisplay (`FUN_00CC5610`) clips a
  viewport to the backbuffer. So the rectangle stays the screen's and `FUN_00CBEA10` (the movies' SetViewport) is
  followed by GFx's own SetViewport with scale mode 0 and viewport scale (2/3) / scale, which gives the visible stage
  the larger layout would have had (`FUN_01606BA0`: visible = w x scale). See `sdk/mohw_offsets.h`.
- **Off 1, the first version doubled the menus** ("double images, which become further separated the greater the
  value"), pulled to one side. The placement was set once per backbuffer bind and left as state, and with the
  rectangle smaller than the screen the game sets its own viewport per movie. Every later movie was then drawn at the
  same spot in both eyes. `KeepPlacedViewport` (`hooks/draw_trace_diag.cpp`) puts the placement back before each
  later draw under the bind when the viewport has changed, and logs the game's viewport. Confirmed fixed by the user
  ("That works as intended").

## What could go wrong

- **Post effects:** some full-screen passes may assume one view (post effects, reflections, the sky). The
  split-screen code path is a test feature.
- **Shared per-view work:** any module that, like the ocean water, kicks per-view jobs on shared buffers can race
  with two views.
- **Culling from one eye:** `FUN_00A70EF0` hands a second consumer (`FUN_0072C480`) a copy of view 0's RenderView
  (+0x4D0) only, unless the view params' flag at +0xDB0 adds a second (from params +0x920). If that's the visibility
  pass, the right eye is culled with the left eye's frustum: objects could pop at its right edge.
- **Render cost:** both eyes are now rendered every frame, at half width each. Raising the game's resolution
  (e.g. 2x the eye width) keeps the per-eye resolution.
- **Halved width at load:** the halving at the level's load (`FUN_00A6B510`) applies to something the module
  creates. With the setting turned on mid-level, that object stays full size until the level is reloaded.

## Next steps, in order

1. Live first light with the split-screen path (above).
2. ~~Make the viewmodel rewrite per view~~: built (above). The batch camera turned out to be per view after all: it's
   the view's raw camera (+0xE00), so no job mark was needed.
3. ~~Draw the HUD per eye~~: built (above), by replaying the HUD list once per half.
4. If split-screen rendering has full-screen-pass bugs, the AMD quad-buffer path is the complete one. It needs:
   - a fake `IAmdDxExtQuadBufferStereo` at `DxRenderer+0x31C` (Enable returns S_OK, `GetLineOffset` returns the eye
     height);
   - `DxRenderer+0x328` set without the fullscreen check;
   - a double-height backbuffer from the proxy.

   The engine then renders both eyes at full size, the UI per eye included, with the second eye's final pass at the
   offset. Present submits the top and bottom halves.
