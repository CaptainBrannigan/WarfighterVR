#pragma once
// ADS sway removal (2026-09-30). Aiming down sights sways the view (breath control, holding the left stick, stills it),
// which is hard to bear in VR. Each weapon zoom level's ZoomLevelData carries the sway multipliers for that zoom
// (SwayPitch/YawMultiplier, and the Supported... pair for a supported weapon). A low-priority background thread finds
// every ZoomLevelData in memory by its vtable after a level loads and scales those four by the AdsSwayScale setting
// (0 = none, 1 = the game's own), restoring the originals when the setting goes back to 1. See sdk/mohw_offsets.h
// OFFSET_ZOOMLEVELDATA_VTABLE. (GunSwayData's deviation scale was tried first: no effect.)

namespace mohw {

// Starts the background thread (once). Needs the module resolved (Offset<>).
void StartWeaponSwayPatch();

} // namespace mohw
