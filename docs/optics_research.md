# Optics: scoped eye, the game's reticle, render-to-texture (2026-11-14)

Background (methods, costs, MOHW's zoom data) is in the 2026-11-07 research note; this is where the prototypes and the
new findings are.

## Scoped eye (prototype, engine two-view): `hooks/scope_view.cpp`

- **Sight line:** the weapon profile's optic zero (OpticHeight / OpticWindageOffset from the weapon hand, along its
  forward), the line the optic dot sits on.
- **Which eye:** the one within `ScopeEyeBox` (3.5 cm) of the line, 0 to `ScopeEyeRelief` (45 cm) behind the hand.
- **Magnification:** the profile's `ScopeMagnification`, or the game's own ADS zoom: `tan(hip/2) / tan(fov/2)` with fov
  = view 0's fovY in the view list before `MakeEyes` writes the eyes (in two-view `ApplyEyeMatchedFov` doesn't run, so
  it is the game's), and hip learned while ADS isn't held. Only zooms of at least `ScopeMinZoom` (2.5x) count: the
  game's zoom levels are 1.1-1.9x for iron sights and red dots, 3x / 4x / 6x / 24x for scopes.
- **The view:** in `MakeEyes`, after the eyes: the scoped eye's RenderView, its copy and its previous frame get a camera
  on the sight line (level with the eye's up) and the eye's frustum tangents shrunk about the line's point (x0, y0) in
  the eye's view: `((tL - x0) / m, (tR - x0) / m, (tU - y0) / m, (tD - y0) / m)`, through the usual fovY / aspect /
  offset inputs. The raw camera (ViewDesc +0xE00) stays the eye's, so the first-person batches still find their eye;
  they're moved 50 m behind it, except a batch with a "1pzoom" mesh (below).
- **Present:** the scoped half is masked with `ID3D11DeviceContext1::ClearView` rectangles (64 bands for the circle's
  edge), plus a duplex crosshair while `ScopeReticle` is on and no game reticle was drawn in the last 200 ms.
- **Logs:** `mohwvr_scope.log` (the eye going in and out, the magnification and its source, the frustum),
  `mohwvr_cameramatrix.log` ("zoom optic mesh ... kept in the scoped eye").
- **To check in the headset:** the magnified image on the scope's line (the crosshair on the laser dot at range), the
  eye box's size, whether the HUD inside the eyepiece is in the way, and whether the one-frame mismatch between the
  view and the mask shows when the eye goes in or out.

## The game's own reticle

- `Weapons.sb` (tools/fb2dump list): **every optic ships as two meshes**, `opt_<name>_1p_mesh` and
  `opt_<name>_1pzoom_1p_mesh` (aimpoint_cs, acog_ta31f, acog_ta31ecos, elcan_m145, trij_rmr, trij_srs02,
  aimpoint_microt1, ...), with shader state assets pairing the two (`..._1p_mesh_opt_..._1pzoom_1p_mesh_win32`).
- Reticle shaders: `mohw/shaders/mohw/mohw_weapons_reticle`, `_reticle_eotech`, `_reticle_acog`. Reticle textures:
  `mohw/weapons/common/reticles/{reddot_center, acog_ecos, elcan_reticles_cm, trijicon_reticles_cm}` and
  `.../opt_aimpoint_microt1/textures/micro_t1_reticle_cm`. Multiplayer has `ReticleCustomizationUnlock`.
- So the AR's reticle that "only shows with ADS" is the game swapping to the 1pzoom mesh while zoomed. The scoped eye
  keeps any batch whose mesh asset name contains "1pzoom" (the name is read from the mesh: a pointer among its first
  dwords leads to an object holding the name at +0x58), so the game's reticle should show in the scope.
- **Unconfirmed:** whether the 1pzoom mesh is a batch of its own (it's kept per batch; in a multi-part batch with the
  gun, the gun would come along), and whether its reticle shader projects to infinity (red dot style) or sits on the
  glass. `mohwvr_cameramatrix.log`'s BATCH NEW lines (every instance's probed names) will show it on the first ADS.

## Render-to-texture: the planar reflection sub-view

- `FUN_0100c090` (`__thiscall`, this = the WorldRenderer, param_2 = the view list `{+0x14 count, +0x18 ViewDesc[]}`)
  sets up each main view's block, stride 0x1B920 (0x6E48 dwords):
  - the view index at WorldRenderer + 0x8CF0 + i*0x1B920 (0x1DFB8 - 0x54B2*4);
  - the view's ViewDesc copied (0xE80 bytes) to + 0x8D00 + i*0x1B920 (0x1DFB8 - 0x54AE*4);
  - with WorldRenderSettings (WorldRenderer + 0x8984) + 0x110: six "DynamicEnvmapFace" sub-views (type 5), their count
    at + 0x1DFB8 + i*0x1B920, each a 0xE80-byte ViewDesc from + 0x1E140 (0x1DFB8 + 0x188);
  - with WorldRenderSettings + 0x232 (**PlanarReflectionEnable**): a sub-view named "PlanarReflection", **type 4**, a
    ViewDesc-like record at WorldRenderer + 0x9B80 + i*0x1B920 (0x1DFB8 - 0x510E*4): +0x00 the main ViewDesc copy,
    +0x04 the name, +0x08 the type, +0xE40 = settings + 0x150 (TerrainLodScaleReflection).
- `FUN_0101d780` builds the frame's pass list: with the view's flag at WorldRenderer + 0x24516 + i*0x1B920 it adds a
  "PlanarReflection" pass (id 0x2E, target from WorldRenderer + 0x8C54, its state at + 0x2456C + i*0x1B920).
- **Why it matters:** the planar reflection is a real second camera rendered into a texture of its own, inside each
  view, outside the two-view limit. With its RenderView (sub-view + 0x40, if it has the ViewDesc layout) overwritten by
  the scope camera, the way `MakeEyes` overwrites the eyes, its texture would be the scope picture: drawable on the
  lens for both eyes, the game's reticle material over it.
- **Next steps:**
  1. Live: force WorldRenderSettings + 0x232 on in a two-view level, log the sub-view's name / type / RenderView (fovY,
     transform) each frame, and find its target in a draw trace (the pass's render target view).
  2. Find who fills the sub-view's RenderView (the mirror about PlanarReflectionComponentData.GroundHeight +0xA0): the
     hook point, after it, for the scope camera. (Static searches for +0x9B80 / +0x9BC0 displacements found nothing:
     it's reached through a computed base.)
  3. The mesh mask: the pass draws only meshes in `MeshSubsetCategoryFlags_PlanarReflection` (opt-in through
     `CastPlanarReflection` / `MeshPlanarReflectionEnable`): find the category test in its culling and open it to all.
  4. Composite the target onto the lens (a shader pass at Present, or the reticle mesh's own material).

## Screen space: the lens picture (prototype, built 2026-11-14, untested)

- Magnifying the eye's own image needs the world without the gun in front of the sight line, and the gun is drawn in
  the same deferred frame; a capture point between the world and the first-person pass hasn't been found.
- The trick that avoids it in two-view: the **other eye's** image has the sight line's far field unblocked (the eyes are
  6.3 cm apart, the scope about 4 cm wide at 20-40 cm, so the line passes about 10 degrees off the scope in the other
  eye), so each eye's lens is filled from the other eye's half around the line's vanishing point, angles divided by
  the magnification.
- `hooks/scope_view.cpp UpdateLensPictures` works out, per eye, the lens disc (ScopeLensRadius, ScopeLensForward ahead of
  the hand on the sight line) in its view and the affine map from its half's uv to the side-by-side frame's uv;
  `hooks/present_hook.cpp DrawLensPictures` draws it with `hooks/shaders/scope_lens.hlsl` (compiled offline by
  `build_scope_lens.cmd` into the committed `scope_lens_vs.h` / `scope_lens_ps.h`: no d3dcompiler import), from the
  unchanged capture into each half before the scoped eye's mask, saving and giving back the immediate context's state.
- Off while an eye is scoped (that half is the scope's magnified view, no source for the other eye). Drawn over
  everything: it ignores depth, so a hand in front of the lens doesn't cover it.
- **To check:** the picture's alignment (its centre on the laser dot at range), the lens's place on the actual scope
  (distance and radius are global for now; per weapon would be next), and whether the other eye's view really is clear
  of the gun at the line's far field on every gun.
