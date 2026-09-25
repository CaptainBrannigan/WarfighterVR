#pragma once
// Game-side half of the eye-matched view method (2026-09-20): sets the game's per-frame
// render vertical FOV from the headset's REAL per-eye frustum (published by the companion
// as HmdViewBlock), instead of multiplying the native FOV by FovScale. The companion half
// (crop + declared FOV) lives in companion/main.cpp using shared/eye_frustum.h.
//
// Swappable: hooks/fov_scale_hook.cpp's CommitViewTransform hook calls exactly ONE of
//   ApplyEyeMatchedFov(param2)   -- this method
//   ApplyFovScale(param2)        -- the old FovScale multiplier (still in that file)
// Swap back by changing that one call (and the companion's matching #if, see
// companion/main.cpp). Nothing else references this file.

namespace mohw {

// Writes RenderViewDesc::fovY (transformPtr + 0x48). Returns true if it wrote. Returns
// false (leaving the native FOV untouched) if the companion hasn't published the headset's
// view geometry yet or the struct looks invalid.
bool ApplyEyeMatchedFov(void* transformPtr);

// Last fovY value this file actually wrote to transformPtr+0x48, from EITHER branch (true-frustum or
// enclosing/inner-edge). 0.0f if never written yet. Added 2026-09-23 for constantbuffer_hook.cpp's
// ExpectedProjM11 -- see g_lastWrittenFovY's declaration comment in the .cpp for why this replaces
// GameRenderer::Singleton()'s own stale fovY copy for that specific purpose.
float GetLastWrittenFovY();

} // namespace mohw
