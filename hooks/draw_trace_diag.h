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

} // namespace mohw
