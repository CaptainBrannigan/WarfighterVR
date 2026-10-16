#pragma once
// In-world markers inside the HUD rectangle (2026-10-16). The HUD placement (hooks/draw_trace_diag.cpp) moves the
// game's whole 2D pass into a head-locked rectangle, so the HUD and menus are readable -- but the in-world markers
// (nametags, objective markers, grenade / mine / vehicle icons) are drawn in that pass too and moved with it, off
// their targets, scaled down, doubled. The game projects each marker per eye with the eye's own camera (they sit on
// their targets with placement off), so the projection is hooked instead (sdk/mohw_offsets.h OFFSET_HUDICON_PROJECT):
// its 0..1 frame position is mapped into the rectangle, u' = (u - left) / width, so that after the rectangle's
// viewport the marker lands exactly where the world point is. The game's own off-screen handling then works in the
// rectangle: markers outside it are pinned to its edge like the game's edge arrows. Only while the HUD is being
// placed over the 3D world (not in movies / menus, not with placement off).

namespace mohw {

bool InstallMarkerProjectionHook();

} // namespace mohw
