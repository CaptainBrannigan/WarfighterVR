#pragma once
// Vertical-only FOV for the inner-edge view mode (2026-09-20).
//
// Why a separate hook: writing RenderViewDesc::aspect (+0x58) in the CommitViewTransform hook has NO effect on the
// world projection (tested live -- only the overall FOV scaled). The engine's projection builder FUN_00704C70 is
// called from the matrix-rebuild routine FUN_00707CB0 (OFFSET_UPDATEMATRICES) with the camera object's OWN
// fov (+0x48) and aspect (+0x58); the frustum half-width is aspect * half-height. So the aspect is changed HERE, right
// before that routine builds the matrices, which keeps every derived matrix (projection, viewProjection, inverses)
// consistent with each other.
//
// Which call is the player's world view: the one whose object fov equals, bit for bit, the value
// hooks/eye_matched_fov.cpp just wrote in CommitViewTransform.

namespace mohw {

bool InstallProjectionAspectHook();
void RemoveProjectionAspectHook();

// Called every frame from ApplyEyeMatchedFov. commitFovRad = the vertical FOV just written (<=0 disables the aspect
// change), trim = factor to divide the projection aspect by.
void SetInnerEdgeProjection(float commitFovRad, float trim);

// TRUE PER-EYE FRUSTUM (companion view mode 3). Also armed each frame from ApplyEyeMatchedFov. For the call whose fov matches
// commitFovRad, sets the projection builder's own inputs so the frustum is EXACTLY asymmetric as requested (decompile of
// FUN_00704C70, perspective branch, with h = near*tan(fov/2) and viewport scale (sx,sy) = (1,1)):
//   top = h*(1+2*offY), bottom = -h*(1-2*offY), left = -aspect*h*(1-2*offX), right = aspect*h*(1+2*offX)
// i.e. aspect = width/height of the wanted frustum, offX/offY = its centre in units of (aspect*h*2)/(h*2). Pass
// commitFovRad <= 0 to disable. Takes priority over SetInnerEdgeProjection. eyeIdx (0=left, 1=right, -1=unknown)
// is stamped alongside the other values purely for the MIXED-EYE BATCH diagnostic (2026-09-24, see
// projection_aspect_hook.cpp's g_frustumEyeIdx comment) -- does not affect matching/write behavior at all.
void SetTrueFrustumProjection(float commitFovRad, float aspect, float offX, float offY, int eyeIdx);

// True while the game is rendering the 3D world, judged from the camera matrix rebuild rate over withinMs windows
// (thousands a second with the world, ~150 during a pre-rendered movie). False during movies, and menus/loading
// screens without a 3D scene, whose whole final pass is 2D (hooks/draw_trace_diag.cpp places all of it, not just the
// HUD after draw 1). Render thread.
bool WorldRenderedRecently(unsigned withinMs);

// DIAGNOSTIC selector (2026-09-21), NOT a "pick the right camera" tool -- CORRECTED same day after live testing: the
// several objects matching within a position-filtered true-frustum session are NOT independent cameras with their own
// viewpoint. The player's view position never moves when a different one is picked; what changes is which render
// LAYERS show up -- some shadows or meshes disappear. These are almost certainly different PASSES of the same player
// camera (a main pass, a depth/shadow pass, ...) that all legitimately need this eye's projection, not decoys to
// exclude. -1 (default, and the only mode that should be left running) = write to every matching object, which is why
// leaving this at -1 is what actually looks correct with nothing missing. >=0 restricts the write to ONE pass only,
// for inspecting what that pass renders (expect visible dropouts elsewhere) -- not a setting to leave selected.
void SetFrustumCandidateIndex(int index);

} // namespace mohw
