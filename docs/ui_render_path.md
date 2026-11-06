# The UI: where the HUD is drawn, and how to give it its own buffer (2026-11-06)

Static (Ghidra on `MOHW_dump_SCY.exe`), plus a logging hook to confirm the key assumption live.

## What draws the UI

- The UI is **Scaleform GFx 3.x** movies (`GFxMovieRoot::Advance` and `::Display`), driven by Frostbite's UI framework.
  - `UIComponentData` widgets: `UICompassDataBinding`, `UICameraCompData`, `UICapturepointCompData`, … on UI screens.
  - `ClientUIGraphEntity` and the other `ClientUI*Entity` classes for the logic.
- Frostbite's Scaleform backend is `UIScaleformRenderer` (its vertex and index buffers are named
  "UIScaleformRenderer/VertexBuffer" and "UIScaleformRenderer/IndexBuffer", at `FUN_00CB7610`, `FUN_00CB7680` and
  `FUN_00CC3600`). Its code is around `0x00CB7000`–`0x00CC4000`.
- **The UI is a render module of GameRenderer.** `GameRenderer::setRenderModule` (vtable +0x2C, `FUN_008ABDF0`)
  fills three slots at `GameRenderer+0xEDC`, run in order every frame by `joinUpdateJob` (`FUN_008B4A30`):
  - **Slot 0 and slot 2:** the same object, set by `FUN_008A5CA0`. That's the UI module, run before and after the world.
  - **Slot 1:** the client world module, set at a level's load by `FUN_00857390`, from that object's `+0xC4`.
- The UI module's render callback is `FUN_008F42F0`. It fills a parameter block and calls **`FUN_00CBE3D0`**
  (`__thiscall`, ECX = the UI's movie manager, `RET 4`, `OFFSET_UI_PASS`):

  | block | contents |
  |---|---|
  | [0] | the renderer |
  | [2] | the movie manager (its +0x44 is the Scaleform renderer state) |
  | [3] | which list: 0 = UI drawn before the world into a target of its own, 1 = the HUD over the finished frame |
  | [4] | the screen renderer (`GameRenderer` vtable +0x38) |
  | [5] | list 0's target |

- `FUN_00CBE3D0`:
  - For list 0 it saves the current target, binds `[5]` (render interface `DAT_02AC16F4`: vtable +0x00 sets the
    target, +0x04 gets it, +0x08 clears, +0x0C sets the viewport), clears it and draws.
  - For list 1 it draws into whatever is bound, which is the backbuffer at the end of the frame. **When the engine's
    stereo is on (`DxRenderer+0x44`) it draws list 1 once per eye**, `setActiveEye` before each.
- The lists are recorded commands, replayed by `FUN_00CBB4E0(list)` from the Scaleform renderer state at
  `+0x30C + list * 0x1010` (vector begin/end). Each command is one of:
  - 0: draw a movie (`FUN_00CBB3C0`)
  - 1: set the viewport
  - 2: set the render target
  - 3: clear

## Why this matters

Today the HUD is found inside the final backbuffer pass by draw order and viewports (`hooks/draw_trace_diag.cpp`:
draw 1 = the world composite, 2–7 the reticle, 9–16 the HUD, 17+ markers, with the menus different). That's the
source of the "shared buffer" problems. The UI pass gives the exact boundary instead: every draw between the start
and end of `FUN_00CBE3D0` with list 1 is the HUD and nothing else.

## Confirming it (the logging hook in this build)

`hooks/ui_pass` marks the pass per thread, and the existing D3D draw hooks count the draws that land inside it.
`mohwvr_uipass.log`, every 5 s, shows each list's calls and the draws inside them:

- **Draws inside the HUD pass:** the D3D calls are made in the pass itself (immediate), and everything below works
  directly.
- **None inside:** the commands are executed later on another thread, and the boundary has to come from the replay
  (`FUN_00CBB4E0`) or the render-command execution instead.

## The plan, if the draws are inside

1. **Own target.** At the start of the HUD pass, bind an RGBA texture of the mod's own (backbuffer size), cleared to
   transparent. While the pass runs, `draw_trace_diag`'s `OMSetRenderTargets` hook swaps any bind of the backbuffer
   for that texture. At the end, rebind what was there before. The HUD is then its own image, with alpha.
2. **Show it.**
   - *Per eye, as now but exact:* draw that texture into each eye's image at the HUD rectangle (the current placement
     math, `ComputeHudRect`), with a small quad shader, instead of moving viewports inside the game's own pass. It
     works the same for one eye per frame and for both halves of a native two-view frame.
   - *Or as a SteamVR overlay:* hand the texture to `IVROverlay` as a quad at a fixed depth in front of the head (the
     mod already runs overlays in `openvr_direct/vr_overlay.cpp`). Head-locked or body-locked, at its own resolution,
     with no per-eye frustum work at all.
3. **The markers and reticle** come for free. They're movies in the same list, so they move with the HUD or are
   hidden as a whole. In-world markers drawn by the 3D icon code (`FUN_016A8F20`) stay as they are now.
4. **Menus** (full-screen UI) are the same list. Whole-2D frames can go to the overlay as a flat screen, which is what
   the current whole-frame placement does by hand.

Native two-view stereo (`docs/native_stereo_render_path.md`) and this fit together: the HUD pass runs once per frame,
the texture goes to both eyes.
