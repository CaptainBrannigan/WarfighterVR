#pragma once
// Phase 2B: identifies which constant buffer(s) carry the view/projection
// matrix, by hooking ID3D11DeviceContext::Map/Unmap on constant buffers and
// scanning their CPU-written contents for a value we can independently
// compute from data we already trust (the live camera's fovY, read from
// GameRenderer+0x50 -- confirmed in Phase 1): for a standard D3D perspective
// projection, proj[1][1] = 1 / tan(fovY/2). That's a distinctive enough
// float to search for with high confidence, without needing to trust any
// unverified RenderView matrix-field offsets.
//
// Phase 2C reuses this hook's identification: the 80-byte buffer specifically
// (not the larger, noisier matches -- see docs/phase2b_findings.md) is
// tracked as "the known projection buffer", with its last-captured 80 bytes
// cached, so draw_duplication_hook.cpp can patch a per-eye copy without
// needing to re-derive which buffer it is on every draw call.

#include <d3d11.h>

namespace mohw {

bool InstallConstantBufferHook();
void RemoveConstantBufferHook();

// Delete toggle / Numpad+,Numpad- step range start / Numpad*,Numpad/ step
// range end / End captures the player bone buffer's ENTITY ID (see
// mohw_offsets.h's OFFSET_FUNCTIONCONSTANTBUFFERUPDATE_CALLER_ENTITY) on
// the next qualifying write and locks onto it permanently -- buffer
// identity itself is a confirmed no-op discriminator (same shared object
// for every skinned draw); press End again to re-capture. A single End
// press is opportunistic though -- it locks onto WHICHEVER entity's write
// happens to land first, which live testing showed can be the equipped
// weapon instead of the player's own body (both are close enough to the
// camera to qualify). Numpad0 cycles the lock through every distinct
// entity id seen so far this session (g_entityCandidates, populated
// passively any time a qualifying write occurs, no need to time it) --
// use this to A/B test candidates until the right one (body vs. weapon vs.
// an NPC) is found. Numpad8,Numpad9 step the camera-distance
// disambiguation threshold down/up (sdk/settings.h's
// GetPlayerBoneDistanceThreshold -- still used as the pre-capture
// bootstrap signal and to gate which writes are even considered as
// candidates). Numpad1,Numpad2 step a SINGLE bone index (setting range
// start and end to the same value each press, instead of adjusting them
// independently) -- for precisely identifying which one index is which
// body part (e.g. the hand) once an entity is locked, rather than testing
// broad ranges. Live controls for the bone-hide proof-of-concept (see
// sdk/settings.h's GetBoneHideEnabled). Call once per Present from
// present_hook.cpp, same pattern as every other hook's hotkey poll.
void CheckBoneHideHotkeys();

// Returns the current 80-byte projection buffer instance, or nullptr if not
// yet identified this session (buffers are recreated across sessions/scene
// loads, so callers should re-check rather than cache the pointer long-term).
ID3D11Buffer* GetKnownProjectionBuffer();

// Copies the last-captured 80 bytes (the game's own left-eye/original data)
// into outBytes80 (caller-owned, must be >= 80 bytes). Returns false if
// nothing has been captured yet.
bool GetLastKnownProjectionBytes(void* outBytes80);

// True if the buffer currently bound at VS slot 0 is the known projection
// buffer -- i.e. this is a "main scene" draw worth duplicating per eye, as
// opposed to some other pass (shadow maps, UI, etc.) that doesn't use it.
bool IsKnownProjectionBufferBoundToVS0();

// Scans all 14 VS constant-buffer slots for the known projection buffer,
// returning the slot index it's currently bound to, or -1 if not bound
// anywhere. Diagnostic for when slot 0 specifically doesn't match during
// real draws -- see IsKnownProjectionBufferBoundToVS0()'s doc comment.
int FindKnownProjectionBufferVSSlot();

// Raw buffer pointer currently bound at the given VS slot (0-13), or
// nullptr if unbound/out of range. Diagnostic-only, for logging what IS
// there when the known buffer isn't found in any slot.
void* GetVsSlotBufferRaw(int slot);

// Writes exactly 80 bytes into the known projection buffer using the
// ORIGINAL (un-hooked) Map/Unmap trampolines directly -- bypassing this
// module's own Map/Unmap hook so the write doesn't get mistaken for the
// game's own data and overwrite the cached "left eye" bytes returned by
// GetLastKnownProjectionBytes(). Used by draw_duplication_hook.cpp to swap
// in per-eye data and restore it afterward. Returns false if no known
// buffer is set yet.
bool WriteRawBytesToProjectionBuffer(ID3D11DeviceContext* context, const void* bytes80);

// View matrix, identified structurally within the same combined 352-byte
// buffer as the projection match above (see constantbuffer_hook.cpp's
// UpdateKnownViewBuffer comment for why this doesn't cross-check against
// a CPU-side struct field the way projection does). Same usage shape as
// the projection accessors above, just a 64-byte 4x4 matrix instead of 80.
ID3D11Buffer* GetKnownViewBuffer();
bool GetLastKnownViewMatrixBytes(void* outBytes64);
bool IsKnownViewBufferBoundToVS0();
int FindKnownViewBufferVSSlot();

// Patches all four camera-related sub-regions of the known view buffer:
// the plain viewMatrix and viewProjMatrix fields (viewBytes64/
// precombinedBytes64, kept in sync defensively but confirmed [unused] by
// the main-scene shader -- see constantbuffer_hook.cpp's
// kCrViewProjMatrixByteOffset comment), and the two fields the shader
// ACTUALLY reads: crViewProjBytes64 (a translation-free view*projection,
// 64 bytes) and cameraPosBytes12 (the per-eye camera position, 12 bytes
// -- the shader subtracts this from world position before applying
// crViewProjMatrix, so it must carry the eye offset, not just the
// rotation).
bool WriteRawBytesToViewMatrix(ID3D11DeviceContext* context, const void* viewBytes64,
                                const void* precombinedBytes64, const void* crViewProjBytes64,
                                const void* cameraPosBytes12);

// The FULL 352-byte combined buffer (not just the 64-byte view sub-region)
// -- for searching the still-unidentified ~176 bytes past the projection
// region for a precombined view*projection matrix (see
// docs/companion_process_findings.md: patching just the separate view
// sub-matrix had no visible effect on gameplay geometry, most likely
// because the shader actually multiplies vertices by a single precombined
// matrix instead). outBytes352 must be >= 352 bytes.
bool GetLastKnownViewBufferFullBytes(void* outBytes352);

// Player's own bone-matrix buffer (60-bone skinning array, boneVectors[60]
// in the player-body vertex shader -- see docs/shader_dumps/
// acf0b7e0f77c6772-vs_replace_player_body_partial_hide.txt). Identified
// live (2026-08-23): a 4096-byte constant buffer, written via
// UpdateSubresource (not Map -- see .cpp), shared by every skinned draw
// each frame (player/NPCs/weapon), disambiguated to the player specifically
// via bone-0's world-space distance to the already-trusted live camera
// position. Same usage shape as the accessors above. Motion-controls
// groundwork -- not yet used by any real feature (hiding specific bones,
// finding hand/wrist indices for controller-driven tracking).
ID3D11Buffer* GetKnownPlayerBoneBuffer();
bool GetLastKnownPlayerBoneBytes(void* outBytes4096);
bool IsKnownPlayerBoneBufferBoundToVS0();
int FindKnownPlayerBoneBufferVSSlot();

// Uses UpdateSubresource internally, not Map/Unmap, unlike every other
// write function above -- see the .cpp's comment for why (this buffer is
// confirmed written via UpdateSubresource live, suggesting
// D3D11_USAGE_DEFAULT, which Map's WRITE_DISCARD mode doesn't support).
bool WriteRawBytesToPlayerBoneBuffer(ID3D11DeviceContext* context, const void* bytes4096);

} // namespace mohw
