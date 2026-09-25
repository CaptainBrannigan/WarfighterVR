#pragma once
// Live test targeting GameWorld::RayCast (sdk/mohw_offsets.h's
// OFFSET_GAMEWORLD_RAYCAST) -- confirmed this session to be called from
// BulletEntity::updateTransformSync every simulation tick, casting the
// traveling bullet's own per-tick displacement segment against world
// geometry for wall/prop collision. This is the real general-purpose
// raycast, distinct from FUN_007F4D80 (hooks/player_hitscan_redirect_hook.h),
// which only tests against ClientSoldierEntity candidates and never against
// level geometry.
//
// GameWorld::RayCast is generic/shared infrastructure used by many
// unrelated systems -- this hook filters strictly on the `ident` string
// this specific call site passes ("BulletEntity updateTransformSync",
// confirmed live via a stack dump), redirecting ONLY that call. Every other
// RayCast use in the engine is left completely untouched.
//
// Unlike hooks/shot_redirect_hook.cpp's approach of restoring the caller's
// buffer after the call, this hook leaves the redirected end-point in
// place: `end` here represents the bullet's actual new position for this
// tick, which should persist so the trajectory change compounds naturally
// tick over tick rather than snapping back.
//
// Redirects to the right motion controller's live aim direction (see
// sdk/motion_controller_aim.h) rather than a fixed test angle, as of
// 2026-09-11 -- this is the confirmed-real visual bullet-path/impact-VFX
// hook, re-enabled alongside hooks/fire_candidate_redirect_hook.cpp so the
// user can visually see where a controller-directed shot travels while
// that hook's effect on the actual hit-scan is evaluated separately. Falls
// back to leaving the segment unmodified if the companion process isn't
// running or hasn't published a controller pose yet.
//
// Diagnostic/test only, same as engine_function_hook.h -- no settings.ini
// toggle, always-on once installed.

namespace mohw {

bool InstallBulletRaycastRedirectHook();
void RemoveBulletRaycastRedirectHook();

} // namespace mohw
