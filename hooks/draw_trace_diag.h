#pragma once
// DIAGNOSTIC (2026-09-21): one-frame D3D11 draw trace, to find where the game draws its 2D UI/HUD relative to the 3D scene
// (needed to split the UI onto its own quad layer -- in the true-per-eye-frustum view mode the UI, drawn at the frame centre,
// ends up 15 deg off the forward direction in each eye and shows up doubled).
//
// Press Scroll Lock (the companion also uses that key for its source dump -- harmless) and the NEXT full frame is traced to
// mohwvr_drawtrace.log: every render-target change (texture size/format, whether it is the swap-chain backbuffer) with the
// number of draws issued into each target before the next change, plus clears and copies.
//
// FINDINGS (2026-09-22, for whenever UI element separation is resumed -- the walker/hide/redirect code this session
// built on top of this file was reverted back out, but these results are real and shouldn't need re-discovering):
// within the tail backbuffer bind, gameplay context, draw index 1 = the whole world/tonemap composite (never touch
// it), indices 2-7 = the reticle, 9-16 = general HUD (ammo/health/etc, ONLY element confirmed correlated via a
// per-index \ toggle test), 17+ = mission markers and beyond (index ~23 specifically flagged as a mission marker in
// an earlier, less precise pass -- 17+ supersedes it). Index 8 was an unclaimed gap. The pause-menu bind is a
// DIFFERENT draw sequence entirely (its own indices don't mean the same thing) and reached at least ~54 draws in one
// capture, vs ~20 for a normal gameplay HUD bind -- both counts drift with how much HUD is actually active, so
// neither is a reliable fixed threshold on its own. None of this is currently wired to any hiding/redirecting.

#include <d3d11.h>

namespace mohw {

// Call once per Present. `backbuffer` is the swap chain's buffer-0 texture (used to flag "this target is the backbuffer").
void DrawTraceOnPresent(ID3D11Device* device, ID3D11Texture2D* backbuffer);

// Called by the DrawIndexed hook that already exists in constantbuffer_hook.cpp (a second hook on the same target is
// impossible). `context` is that hook's own `self` (the ID3D11DeviceContext the draw was issued on).
void DrawTraceNoteDraw(ID3D11DeviceContext* context, unsigned indexCount);

// UI-snapshot capture (2026-09-21, see this file's own top-of-.cpp comment for the plan): the backbuffer's content
// right after the Nth draw call under a backbuffer-targeted bind, updated every frame regardless of Scroll Lock
// tracing. Returns null until the first frame has produced one. Caller must not Release() it -- owned here.
ID3D11Texture2D* GetUiSnapshotTexture();

// Whether this frame's final pass into the backbuffer has drawn anything yet (reset at Present). Work done after it
// belongs to the next frame -- see hooks/driving_camera_hook.cpp's late rebuilds.
bool FinalPassStartedThisFrame();

// Any thread: the HUD rectangle for the eye being rendered, as fractions of the frame {left, top, width, height}
// (y down), while the HUD is being placed over the 3D world (not during whole 2D frames or with placement off).
// hooks/marker_projection_hook.cpp maps the in-world markers into it.
bool GetHudRectForMarkers(float rect[4]);

// Any thread, native two-view (hooks/native_stereo.h): each eye's HUD rectangle inside its own half, fractions of the
// half {left, top, width, height} (y down): the placed one, or the whole half with placement off. The markers come from
// one projection for both halves; hooks/marker_projection_hook.cpp puts each where the two halves agree on its
// direction. False when two-view isn't running.
bool GetNativeHudRectsForMarkers(float rects[2][4]);

// Any thread (hooks/native_stereo.cpp): whether the frame is a whole 2D frame -- a movie, a menu, a loading screen, a
// cutscene before the player has control -- as last decided on the render thread (Is2DFrame, at every bind of the
// backbuffer), and in *forMs for how long it has been that way. Engine two-view stereo stands down for those, so they
// get the floating screen in both eyes instead of being split between the halves (2026-11-08).
bool IsWhole2DFrameNow(unsigned long long* forMs);

} // namespace mohw
