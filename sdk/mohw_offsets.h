#pragma once
// Static offsets for MOHW.exe, plus the runtime machinery to use them safely.
//
// CONFIRMED FOR THE CURRENTLY-INSTALLED BUILD (C:\Games\Medal of Honor
// Warfighter\MOHW.exe, dated 2012-12-17): the values below come from a
// "GameDefines.h" generator dump (CypherPresents, dated 2012-12-18) whose
// MOHW_MP_SIZE (0x2865000) matches this machine's installed exe's measured
// PE SizeOfImage exactly -- confirmed via `dumpbin /headers`. An earlier
// header this SDK started from had MOHW_MP_BASE=0x500000 and a different
// MOHW_MP_SIZE (0x2843400): that was a different patch/build and has been
// fully replaced, not merged -- do not resurrect those values.
//
//   1. MOHW_MP_BASE (0x400000) is the address the game happened to load at
//      when these were captured. If ASLR relocates the module, every offset
//      below is wrong by a fixed delta -- ResolveModule() computes that delta
//      once at startup and Offset<T>() applies it on every use.
//   2. MOHW_MP_SIZE pins the exact build these offsets belong to. VerifyBuild()
//      must pass before any offset is dereferenced; a mismatched build means
//      every class layout below may be silently wrong, not just off-by-a-constant.
//
// Never dereference OFFSET_* directly -- always go through mohw::Offset<T>().

#include <windows.h>
#include <cstdint>

#define MOHW_MP "MOHW.exe"
#define MOHW_MP_BASE 0x400000u
#define MOHW_MP_SIZE 0x2865000u

// --- Confirmed for this build ---
#define OFFSET_MAIN 0x2A4D480u
#define OFFSET_GAMECONTEXT 0x2AC5FE4u
#define OFFSET_CLIENTGAMECONTEXT 0x2AC5FE4u // generator dump gives the same address for both -- unconfirmed whether that's intentional or a generator quirk
#define OFFSET_DXRENDERER 0x2AC1E20u
#define OFFSET_GAMERENDERER 0x2AC8FD0u
#define OFFSET_BORDERINPUTNODE 0x2AC9888u
// Per-frame render-submit function (2026-09-23, found via Ghidra decompile of the joinUpdateJob call site already
// traced in docs/phase2c_highlevel_findings.md -- RVA+0x496DE4, i.e. this same address's return point). __thiscall,
// param_1 in ECX is presumably the RenderView/camera context (not GameRenderer itself -- it separately reads
// GameRenderer::Singleton() via OFFSET_GAMERENDERER internally). Confirmed via raw disassembly: RET 0xc, so 3 real
// stack dwords are part of the ABI even though Ghidra's decompiler only shows one being read (param_2) -- the other
// two are presumably unused by this code path but must still be passed/cleaned correctly. Body: fetches the current
// camera transform, conditionally calls the already-known viewmodel batch matrix setter (FUN_006FC2A0, see
// project_mohw_viewmodel_batch_hook memory), then calls GameRenderer's joinUpdateJob (vtable+0x14) with that
// frame's camera/fov/time data -- see hooks/framerender_hook.cpp for the live investigation of whether this can
// safely be invoked twice per frame for genuine simultaneous per-eye stereo.
#define OFFSET_FRAMERENDERSUBMIT 0x896C20u
// Globals FUN_00896c20 itself reads/writes (2026-09-23, from the Ghidra decompile in converged_chain_dump.txt), for
// snapshotting state before/between/after the recursion test's two calls -- see hooks/framerender_hook.cpp.
// DAT_02ac8f90: byte, bit0 -- looks like a one-time lazy-init guard (checked-then-set, calls FUN_00537890 once),
// not a per-frame counter. DAT_02ac5fe0: pointer to an object whose (+0x844+0x42) byte the function sets to 0 at
// entry and back to 1 on every exit path -- self-consistent per call, so calling twice should leave it the same
// either way. DAT_02ac5fe4: only ever read (fov-scale-related config), never written here.
#define OFFSET_FRAMERENDER_FLAG1 0x2AC8F90u
#define OFFSET_FRAMERENDER_PTR1 0x2AC5FE0u
#define OFFSET_FRAMERENDER_PTR2 0x2AC5FE4u
#define OFFSET_UPDATEMATRICES 0x707CB0u
// Found via Ghidra static analysis of a LIVE memory dump (this build's EA
// DRM encrypts code on disk, so the on-disk exe never resolved to a real
// function at this address -- only the live, already-decrypted image
// did). Confirmed via a literal debug string in the decompiled output:
// "m_deviceContext->Map(m_viewCons..." -- independently matches the
// "viewConstants" HLSL cbuffer name already found via shader disassembly
// (see docs/stereo_view_matrix_investigation.md). __thiscall, param_1 in
// ECX is a "this"-like object pointer -- see hooks/engine_function_hook.cpp
// for the live param_1-vs-GameRenderer::Singleton() comparison this was
// added to run.
#define OFFSET_UPDATEVIEWCONSTANTS 0x7738C0u
// Player-bone-buffer disambiguation (2026-08-25): the CPU-side updater for
// the 4096-byte skinning constant buffer (constantbuffer_hook.cpp's
// GetKnownPlayerBoneBuffer), called from INSIDE OFFSET_UPDATEVIEWCONSTANTS
// (FUN_007738c0) itself, at two nearby return addresses. __thiscall, but
// with a DIFFERENT `this` than FUN_007738c0's own param1 -- confirmed live
// (x32dbg log breakpoint) to always be one fixed singleton (0xED2AC800 in
// that session), not per-entity. 5 real stack params (confirmed via this
// function's own `ret 0x14`); arg1 is the entity-identifying one -- see the
// two _CALLER offsets below.
#define OFFSET_FUNCTIONCONSTANTBUFFERUPDATE 0x771670u
// The two return addresses inside FUN_007738c0 that call
// OFFSET_FUNCTIONCONSTANTBUFFERUPDATE, confirmed live via an x32dbg
// log-only breakpoint at the callee's entry (caller={[esp]}): _ENTITY's
// arg1 took on dozens of distinct, per-call-stable values (confirmed
// identical across two separately-captured play sessions with completely
// different objects in view, ruling out a camera-visibility correlation --
// this call site's arg1 set is apparently independent of what's rendered,
// more like a broader nearby/active-entity list), while _SHARED's arg1
// only ever cycled through ~5 addresses clustered within 0x240 bytes of
// each other (an internal fixed table, not per-entity). Use _ENTITY's arg1
// as the real per-entity identity signal -- buffer identity and content-
// based distance were both confirmed dead ends for this buffer (see
// constantbuffer_hook.cpp's UpdateKnownPlayerBoneBuffer comment).
#define OFFSET_FUNCTIONCONSTANTBUFFERUPDATE_CALLER_ENTITY 0x773BA6u // == OFFSET_UPDATEVIEWCONSTANTS + 0x2E6
#define OFFSET_FUNCTIONCONSTANTBUFFERUPDATE_CALLER_SHARED 0x773B8Cu // == OFFSET_UPDATEVIEWCONSTANTS + 0x2CC
// Camera-class matrix setter (2026-09-19, found via Cheat Engine "what writes"
// on the camera object): FUN_006FC2A0, __thiscall(this=camera, float* 4x4),
// RET 4; copies 16 dwords to this+0 and ORs 9 into this+0x90 (dirty flags).
// The camera-class object also has FOV at +0x48 (FUN_006FC090), +0x50
// (FUN_006FC150), +0x6C/+0x70 (FUN_006FBFF0). ~25 callers; the one that
// feeds the player camera is inside FUN_0099C2A0 (per-frame camera update,
// called from FUN_008A6DB0 at 008A793D). The CALL instruction is at
// 0x99C473 (5 bytes, E8 rel32), so the RETURN address is 0x99C478.
#define OFFSET_CAMERAMATRIXSET 0x6FC2A0u
#define OFFSET_CAMERAMATRIX_CALLER_UPDATE 0x99C478u
// Inside FUN_008B2AB0 (per-batch draw builder): the second FUN_006FC2A0 call
// (CALL at 0x8B2D93, RETURN 0x8B2D98) sets the CAMERA-RELATIVE matrix (origin
// at batch+0x60 subtracted) on a stack-local camera object used only for
// batches with a non-zero origin -- the 12-batch first-person group. Its
// derived view/projection end up in that batch's "shaderState/view" block.
#define OFFSET_CAMERAMATRIX_CALLER_VIEWMODEL 0x8B2D98u
// FUN_006FC090 (camera FOV setter) call inside FUN_008B2AB0: CALL 0x8B2C9D,
// RETURN 0x8B2CA2. Argument is batch+0xC4 (degrees, 55.0) converted to radians.
#define OFFSET_FOVSET_CALLER_VIEWMODEL 0x8B2CA2u
// FUN_00723800: per-instance draw-item submit, cdecl(item, drawState), caller cleans.
// item+0 = mesh ptr, item+4 = second ptr, item+0x2C = ptr to the batch matrix
// (batch+0x10). Called from FUN_008B2AB0's instance loop: CALL 0x8B370F, RETURN 0x8B3714.
// FUN_006FC090: camera-class FOV setter, __thiscall(this, float fovRadians), RET 4.
// The static player camera object (see debugging tips) holds the live NATIVE world
// FOV (radians) at +0x48, before the mod's FovScale is applied at commit.
#define OFFSET_CAMERAFOVSET 0x6FC090u
#define OFFSET_PLAYERCAMERAOBJECT 0x2AE2890u
#define OFFSET_DRAWITEMSUBMIT 0x723800u
#define OFFSET_DRAWITEMSUBMIT_CALLER_INSTANCELOOP 0x8B3714u
// Draw-call tracing (2026-08-26) -- task: find the real mesh/render
// component for whichever entity is locked, since bone-buffer patching
// can't fix the occlusion/G-buffer artifact confirmed independent of
// hide technique (see constantbuffer_hook.cpp's kBoneHideHeightOffset
// history). This is the "submit this draw" dispatcher live-traced from
// DrawIndexed's own caller (0076A55A, itself called from inside this
// function): __thiscall, this=a SHARED render-pass context (not
// per-mesh -- confirmed live, its own `this` becomes EBX and stays one
// object across the whole call). 4 stack args (confirmed via its `ret
// 0x10`); arg1 is the one that matters -- it's a genuine per-item
// render descriptor pointer (into a distinct EB9xxxxx-range pool),
// confirmed live to vary across dozens of values per frame and to
// reappear identically in repeated per-frame bursts (a stable per-mesh
// pool, not per-frame garbage).
#define OFFSET_DRAWBATCHDISPATCH 0x76A4D0u
// Found via live x64dbg tracing (2026-08-03): hardware write breakpoint on
// GameRenderer::Singleton()->m_viewParams.view.m_desc.transform.forward.x
// (LinearTransform, see mohw_common.h) led here. __thiscall,
// param_1 (ECX) = GameRenderer-relative destination "this", param_2 (one
// stack arg) = source pointer to an already-computed GameRenderViewParams-
// shaped structure -- this function bulk-copies param_2 into
// param_1+0x50 (m_viewParams) every simulation tick. Confirmed live: the
// source pointer is stable across frames (not a per-call transient), and
// its LinearTransform fields (at the very front, offset 0x00) smoothly
// track real per-frame camera rotation/position during gamepad look
// input. Called from a large per-tick player/pawn update function
// (job-dispatched, ultimately from the GameRenderer vtable's
// createUpdateJob/joinUpdateJob-shaped calls) -- runs once per tick, not
// once per draw call like OFFSET_UPDATEVIEWCONSTANTS, making this a much
// cheaper and more architecturally correct injection point for HMD
// rotation than the constant-buffer patch. See hooks/
// commit_view_transform_hook.cpp for the live proof-of-concept hook.
#define OFFSET_COMMITVIEWTRANSFORM 0x8AB580u
#define OFFSET_SCREENSHOTBUFFER 0x2ABEA38u // was OFFSET_BLOCKSCREENSHOT in the old/mismatched header

// Found via live x64dbg breakpoint during the original input-pipeline
// investigation (project history, pre-dating hooks/gameplay_input_hook.cpp):
// "applies a rotation/scale transform pass and an invert/sensitivity
// multiply, then ACCUMULATES into its own 'this' object" -- confirmed
// __thiscall (ECX=this) via a breakpoint at this exact address logging
// {ecx}. Called once per frame; this+0x104=move-X, this+0x108=move-Y,
// this+0x10C=look-yaw, this+0x110=look-pitch (all float*, BYTE offsets,
// not float-index -- an earlier session already got burned mixing those up
// once, see hooks/gameplay_input_hook.cpp).
//
// CAUTION, unlike every other OFFSET_* above: only "ECX=this" was ever
// confirmed live for this function -- the ORIGINAL investigation never
// pinned down whether it also takes stack parameters (unlike
// OFFSET_COMMITVIEWTRANSFORM, whose exact signature was read straight out
// of a Ghidra decompile). A __thiscall MinHook detour with the wrong
// assumed stack-parameter count is EXACTLY what caused this project's
// first-ever crash (Phase 2C-high-level, docs/phase2c_highlevel_findings.md)
// -- if the game crashes immediately after this hook installs, that
// mismatch is the first thing to suspect, not a deeper bug. Best guess
// (zero stack parameters, a plain accumulator-update method) is what
// hooks/gameplay_input_hook.cpp currently assumes; not independently
// re-verified this session.
#define OFFSET_INPUTACCUMULATOR 0x8E03C0u

// AimingController's per-frame update method -- writes the authoritative
// absolute yaw/pitch heading (AimingController+0xC/+0x10, radians, see
// project_mohw_yaw_facing_investigation.md's "SOLVED" section) from real
// mouse/gamepad input every call. Found via live x64dbg register logging
// (2026-08-21): across hundreds of hits during normal gameplay (player + 2
// AI active), ECX at this function's entry NEVER varied from a single
// fixed value -- strong evidence this is a thiscall method called ONLY in
// the local player's context, never for AI. Confirmed ECX really is
// AimingController by reading [ecx+0xC]/[ecx+0x10] at that same breakpoint
// (got plausible yaw/pitch radians) plus corroborating fields:
// [ecx+0x58]/[ecx+0x5C] read as a plausible pitch-clamp pair (~-75/72
// degrees) and [ecx+0x3C] a plausible sensitivity scalar (~1.0). ALSO
// empirically confirmed live (same session, user-verified): this function
// is NEVER called while a menu is open, during loading, or in the main
// menu -- only during live gameplay with the player's soldier loaded. A
// hook here therefore needs no separate game-state gate.
//
// SIGNATURE CONFIRMED (2026-08-21, live x64dbg check on the function's own
// RET at 0x009DE92C, ~0x3B bytes past the confirmed unconditional write --
// almost certainly the function's real, single exit): `RET 0x94` -- ECX=
// this (implicit, not counted in the RET N total) plus 0x94=148 bytes (37
// dwords) of real stack parameters, never individually decoded
// field-by-field. hooks/aiming_controller_hook.cpp handles this via a
// byte-size-matching flat dummy struct passthrough rather than a full
// per-field decompile -- see that file's own comment on
// AimingControllerUpdateFn/Passthrough37 for why this is safe on x86 MSVC
// specifically (by-value structs are always a raw stack copy on x86,
// unlike x64's invisible-reference ABI). This resolves the exact
// stack-parameter-count risk that crashed this project once already
// (OFFSET_INPUTACCUMULATOR above, docs/phase2c_highlevel_findings.md) --
// no further verification needed before enabling this hook.
#define OFFSET_AIMINGCONTROLLER_UPDATE 0x9DDA90u

// FUN_00913C50 -- a freshly-fired bullet/projectile entity's Initialize
// method (vtable slot 0x9C on the class also confirmed to own the per-tick
// dispatch FUN_0091DA40/FUN_0091DB10 -- vtable base 0x02815200, live-read
// this session; FUN_0091DB10 is genuinely shot-gated, confirmed live to
// fire ONLY on an actual fired shot, by the player too). Found via a long
// chain of live hardware-write-breakpoints (position field -> constructor
// FUN_00DE5730 -> this Initialize method -> spawn-params record -> a
// generic copy-assignment operator FUN_00A84D10 -> a stack-local source
// built by FUN_00A94830 -> FUN_00A93C30 -> FUN_00606BB0, the
// already-known muzzle-direction-basis function), not from a decompile of
// this function's own callers (those are all generic engine dispatchers
// used by hundreds of unrelated classes, a dead end for narrowing down).
//
// __thiscall, param_1 (ECX) = the bullet object being initialized, one
// stack parameter (a pointer to a spawn-params record). CONFIRMED LIVE:
// after this function runs, param_1+0xD0..0xDC holds the fired velocity
// vector (x,y,z,w) -- captured as a real, non-normalized vector
// {344.7, -4.235, 45.7, 0.0} (magnitude ~347.7, a genuine muzzle speed,
// not a placeholder). param_1+0xE0..0xEC is a confirmed-live duplicate of
// the same vector, written together with it in the original code -- any
// override must keep both in sync. Y is the vertical (up/down) axis,
// confirmed this session by cross-referencing multiple ClientSoldierEntity
// positions on flat ground (Y stayed ~constant while X/Z varied).
//
// This is the actual fired-direction source for the real physical bullet,
// not just the cosmetic muzzle-flash effect -- FUN_00A93C30/FUN_00606BB0
// were previously (see project_mohw_weapon_fire_open_questions.md item 2)
// closed as "visual muzzle-flash only, not confirmed hit-scan"; that
// verdict was incomplete, not wrong about what it directly observed -- the
// same transform feeds both consumers. See hooks/shot_redirect_hook.cpp
// for the live test of overriding this vector.
#define OFFSET_BULLETINITIALIZE 0x913C50u

// FUN_007F4D80 -- the REAL, authoritative player-side hit-scan raycast.
// This is the "else" branch of a function (FUN_007CF810, not itself hooked
// here -- see below) that's called from BOTH player and AI shot processing;
// this specific branch, gated by an internal condition (FUN_007E9640), was
// confirmed LIVE this session to fire ONLY on player-fired shots -- never
// once across dozens of AI-fired captures, including AI shots that landed
// real hits on the player. Found via live hardware-write-breakpoint tracing
// PLUS a redirect-and-observe test: an earlier hook on
// OFFSET_BULLETINITIALIZE's velocity field visibly redirected the impact
// VFX but NOT the real hit (the shot still authoritatively hit the
// original target) -- proving that velocity is cosmetic bullet-travel
// presentation, not the real hit-scan. This function is.
//
// __thiscall, param_1 (ECX) = a validation-context object, 5 real stack
// parameters. CONFIRMED via full Ghidra decompile: param_2/param_3 are
// float* to vec4s -- the ray's origin and end point (direction = param_3 -
// param_2). Internally loops over every real candidate (count read live
// from the object graph, not hardcoded), running a genuine geometric
// capsule/segment intersection test per candidate (FUN_0080BF60, which
// itself calls FUN_0080A910 -- a textbook closest-point-between-two-3D-
// -line-segments algorithm, the canonical Voronoi-region-case form,
// unmistakably real collision code), tracking the closest valid hit. If a
// hit is found within range, writes a complete hit-result record into
// param_6 (undefined8*): bytes [0x00..0x0F] = normalized hit normal
// (vec4), byte 0x10 = hit distance (float), byte 0x18 = a per-candidate
// data lookup, byte 0x1C = the hit entity's ID/pointer (indexed by the
// winning candidate) -- and returns 1 (else returns 0, no valid hit).
//
// This explains why AI has no equivalent: AI doesn't aim through a
// screen-space reticle, so it has no reason for a reticle-based target-
// lock/validation layer -- its one calculation (see OFFSET_UPDATEVIEWCONSTANTS-
// adjacent muzzle-transform chain, FUN_00A93C30/FUN_00606BB0, docs in
// project_mohw_weapon_fire_transform_investigation.md) IS authoritative
// for AI shots. The player's version of that same chain only drives
// cosmetic bullet-travel/impact-VFX; THIS function is what actually
// determines what the player's shot hits.
//
// To redirect the player's real shot, override param_2/param_3 (the ray)
// before the original runs -- see hooks/player_hitscan_redirect_hook.cpp.
// Deliberately NOT hooking the shared FUN_007CF810 entry point both
// player and AI call into: hooking here instead needs no player-vs-AI
// filtering logic at all (AI code structurally never reaches this
// function, confirmed empirically) and carries zero risk of altering AI
// behavior.
#define OFFSET_PLAYERHITSCAN 0x7F4D80u

// FUN_00E3C870 -- part of the CONFIRMED-real damage/hit-message chain
// found in an earlier session from the AI-inflicted-hit-on-player
// direction: FUN_00DCC8B0 (real damage calc) -> FUN_00E3C870 ->
// FUN_00B53140 (populates a NetworkPlayerViewHitMessage -- confirmed via
// the literal string "NetworkPlayerViewHitMessage" found live 2026-09-11).
// Whether this same chain is ALSO used for player-inflicted hits on AI is
// untested -- see hooks/hit_message_redirect_hook.cpp.
//
// __thiscall, param_1 (ECX) = this, 10 real stack parameters. param_5
// (float*, 4th stack param) is a vec4 position -- the function computes a
// normalized direction as param_5 minus a reference position fetched via
// FUN_007EECB0 (OFFSET_HITMESSAGE_REFPOS below), then forwards most params
// (reordered) into FUN_00B53140.
#define OFFSET_HITMESSAGE_NORMAL 0xE3C870u

// FUN_007EECB0 -- called from inside FUN_00E3C870 to resolve the reference
// position the hit direction is computed relative to. Calling convention
// NOT independently confirmed (only ever observed as FUN_00E3C870's own
// caller-side usage: one explicit stack arg, a 28-byte local buffer, no
// visible explicit "this" setup -- inferred to reuse FUN_00E3C870's own
// "this" via the same implicit-ECX-forwarding pattern seen throughout this
// project). Treat as unverified; hooks/hit_message_redirect_hook.cpp wraps
// its use in SEH accordingly.
#define OFFSET_HITMESSAGE_REFPOS 0x7EECB0u

// FUN_007d4680 -- the fire-time wrapper that calls FUN_007CF810 (and
// internally, on the player branch, FUN_007F4D80's closest-point-between-
// segments test) to accumulate per-target hit candidates. Confirmed LIVE
// (2026-09-11) via a breakpoint at this function's own entry: it fires ONLY
// when actually aiming+firing AT an AI target -- never on a wall shot, and
// never during the continuous reticle/aim-assist pass through FUN_007CF810
// (that pass returns to a different, unrelated caller at 0x905762, part of
// the "SoldierAimingEnvironment::findTargetInReticle" aim-assist subsystem --
// unrelated system, do not confuse the two). This makes FUN_007d4680 a much
// more surgical player-fire-time candidate than hooking FUN_007F4D80/
// FUN_007CF810 directly, since those also fire continuously outside of
// actual shots.
//
// __thiscall, param_1 (ECX) = this. CONFIRMED via Ghidra decompile that this
// is a PER-SHOT CONTEXT OBJECT, not the persistent bullet/AI entity -- live
// capture showed ECX sitting just above ESP (stack-local), consistent with
// a fresh, short-lived accumulator built by an outer caller for this one
// shot. Two more explicit stack params (param_2/param_3) are forwarded
// unmodified into the internal FUN_007CF810 call (param_2 also gets stashed
// per-candidate as what looks like an attacker/weapon ID -- not confirmed).
// Confirmed field layout on `this`:
//   this+0x180 = ray origin (Vec3, passed to FUN_007CF810 as param_2/this+0x180)
//   this+0x190 = ray end     (Vec3, passed to FUN_007CF810 as param_3/this+0x190)
// CORRECTED 2026-09-11: the original Ghidra decompile printed this offset as
// "param_1 + 400" in DECIMAL (inconsistent with the surrounding hex-styled
// offsets like param_1+0x180/+0x1c4) -- 400 decimal = 0x190 hex, not 0x400.
// Misread as hex the first time, which meant the first two live tests of
// fire_candidate_redirect_hook.cpp read/wrote always-zero padding at
// +0x400 and never touched the real end-point at all (visible in the log
// as "original end={0.000,0.000,0.000}" on every single call) -- fully
// explaining why those tests showed no effect on real hits.
//   this+0x1c4 = a scalar (range?), forwarded to FUN_007CF810 unmodified
//   this+0x1a0 = max-candidate-count threshold
//   this+0x1b0 = running best/closest distance seen so far
//   this+0x1b4 = running candidate count (read this before/after a call to
//                detect whether a candidate was actually recorded)
//   this+0x90, stride 0x30, indexed by the count above = candidate records:
//     +0x00 vec4 (hit position?), +0x10 vec4 (hit normal?), +0x20 an ID
// This is currently the strongest candidate for the player's real,
// authoritative hit-scan -- see hooks/fire_candidate_redirect_hook.cpp.
#define OFFSET_FIRECANDIDATE 0x7D4680u

// FUN_00960050 -- the real world-space bone-transform composer at the heart
// of the 2026-09-15 bone-attach-point investigation (see project memory
// project_mohw_bone_attach_point_investigation.md). Resolves a named bone
// (e.g. "RightHand") by walking its parent-bone chain and writing the fully
// composed world transform into the soldier's "soldierWeaponBoneTransforms"
// pool buffer (FUN_008BB050), which is fed directly into a
// "worldRenderPrimitive" (FUN_00FF0450/FUN_00FE97E0) -- i.e. this is the
// last point where a bone's world transform exists before becoming the
// thing actually drawn. Chosen as the hook point over the buffer instead
// because: (a) this IS the final world-space value, one hop from render,
// so a patch here can't be undone by later composition steps; (b) the
// alternative -- patching the 16-float INPUT buffer at this+0x790 instead
// -- would require correctly round-tripping an only-partially-understood
// quaternion/axis representation (only floats 0-3 confirmed used, via real
// sin/cos calls into a matrix-build helper; floats 4-15 unconfirmed), where
// a wrong guess risks NaN-ing the whole composed chain; patching the
// already-composed output float triples is comparatively low-risk and
// self-contained.
//
// ABI CONFIRMED 2026-09-15 via raw disassembly of the call site inside
// FUN_00964170 (see Ghidra/fn960050_raw_disasm.txt) -- Ghidra's original
// decompiled signature was correct, resolving an initial ambiguity about
// whether ECX shifts between caller and callee:
//   __thiscall, ECX = the SAME "this" as the caller FUN_00964170's own this
//   (confirmed: caller does `MOV ECX,ESI; CALL 0x00960050` where ESI is
//   FUN_00964170's own this -- i.e. this function operates on the calling
//   context's object, not a distinct sub-object).
//   Exactly 5 stack parameters (confirmed via RET 0x14 = 20 bytes = 5
//   dwords), in order [EBP+8]..[EBP+0x18]:
//     param_2 ([EBP+8])  -- unconfirmed, forwarded from caller unmodified
//     param_3 ([EBP+0xC]) -- unconfirmed, forwarded from caller unmodified
//     param_4 ([EBP+0x10]) -- pointer into the bone hierarchy/output array.
//       Confirmed live: the prologue loads this into ESI and reads 3
//       consecutive 16-byte (4-float) rows at ESI+0x30/+0x40/+0x50 via
//       MOVAPS, then the tail loop does `param_4 += 0x30` before each
//       FUN_005FA250 write -- i.e. entries are 0x30 bytes apart (3 rows of
//       4 floats each), consistent with a row-major 3x4 transform where
//       each row's 4th float is that row's translation component. NOT yet
//       independently confirmed which row is which axis, or that the 4th
//       float really is translation rather than e.g. a homogeneous 1.0/pad.
//     param_5 ([EBP+0x14]) -- ancestor-chain depth/loop count. The tail
//       loop appears to run (param_5 - 1) times, each iteration advancing
//       param_4 by 0x30 BEFORE writing via FUN_005FA250 -- so the FINAL
//       written entry is at param_4_original + (param_5-1)*0x30. This
//       formula is inferred from a partial decompile read, not from the
//       raw disassembly of the loop body itself -- treat as best-effort,
//       verify against this hook's own diagnostic logging before trusting
//       it for anything beyond a visual test.
//     param_6 ([EBP+0x18]) -- the 16-float input buffer at this+0x790
//       (confirmed: caller does `LEA ECX,[ESI+0x790]; PUSH ECX` for this
//       arg). Only floats 0-3 confirmed read (fed through real sin/cos and
//       a matrix-build call, consistent with a quaternion or axis-angle
//       representation); floats 4-15's meaning is unconfirmed.
// Single write-back call site confirmed at 0x960AD2 (CALL FUN_005FA250),
// close to the function's single RET 0x14 at 0x960AEA.
//
// RETRACTED AS AN INJECTION POINT (2026-09-15, same day): confirmed DEAD via
// live x32dbg breakpoints across a real play session (movement, weapon
// switching, an AI on screen) -- this function and its caller FUN_00964170
// never fire. Root-caused via static call-graph + decompile: FUN_008BB050
// really does call FUN_00964170 (direct CALL instructions confirmed, not
// proximity), but only when a per-entity flag at entity+0x7D0 is nonzero --
// and that flag is set exactly once, at entity construction (FUN_009691E0),
// gated behind a dormant GameContext-derived feature check
// (DAT_02ac5fe4+0x10 -> +0x60 -> byte+0x73) that evaluates false under
// normal play. This whole OFFSET_BONETRANSFORMCOMPOSE chain is the
// currently-inactive alternate path, not the one driving what's on screen.
// See OFFSET_WEAPONBONECOMPOSE below for the confirmed-live replacement,
// found via a raw instruction trace (tmp9.txt) through the SAME entity+0x7D0
// check's zero branch. Left defined/documented for history; do not hook this.
#define OFFSET_BONETRANSFORMCOMPOSE 0x960050u

// FUN_00683580 -- the REAL, confirmed-live, always-executing weapon-bone
// world-transform composer, found 2026-09-15 via a raw instruction trace
// (tmp9.txt, starting at the FUN_008BB050 entry-plus-"soldierWeaponBoneTransforms"-
// allocation breakpoint 008BBDD7) after OFFSET_BONETRANSFORMCOMPOSE was
// confirmed dead (see that entry's retraction note). This is what actually
// runs on every soldier, every frame, under normal (entity+0x7D0==0)
// conditions -- traced live: FUN_008BB050 -> FUN_00955240 -> virtual calls
// through entity+0xB0 -> FUN_0096E9A0 -> here.
//
// ABI CONFIRMED via decompile + live trace: __thiscall, RET 0xC (3 stack
// params). ECX = a skeleton/pose-data object (this call: CB3AFE00), reached
// via entity->[+0x124] from an array-of-variants selected earlier in the
// caller chain.
//   param_1 ([EBP+8])  -- output buffer pointer (the allocated
//     "soldierWeaponBoneTransforms" slot). Entry N's composed 3x4 (12-float,
//     0x30-byte) world transform is written at param_1 + N*0x30 (NOT
//     param_1+0x20+N*0x30 -- that was an arithmetic error in an earlier
//     version of this comment; re-derived carefully from the decompile's
//     `pfVar23 = param_1+0x20` initial pointer plus `pfVar23[-8..3]` writes).
//   param_2 ([EBP+0xC]) -- a lookup key. The function linear-searches a
//     3-int-stride registration array (reached via ECX+0x1BC -> +0x1C ->
//     +0x10) for an entry whose first field == param_2; returns immediately
//     doing nothing if no match. CONFIRMED live this call: param_2 was
//     DFEEBD00, an object whose own +8 field points to the string
//     "Animations/Skeletons/Weapon/WeaponSke01" -- a weapon-skeleton
//     connection/binding object, not a generic pointer.
//   param_3 ([EBP+0x10]) -- a byte mode flag (0 in the confirmed capture).
//     When 0, the function uses a zeroed local quaternion; when nonzero, it
//     reads one from ECX+0x30 instead. Purpose not further investigated.
// Matched entry's fields: +4 = pointer to an int array of already-resolved
// BONE INDICES (one per output entry, NOT looked up by name per-frame --
// name resolution happens once elsewhere, likely at weapon-equip/load time,
// via the same generic FUN_0067BF40 binary search used for "RightHand"
// elsewhere in this codebase); +8 = pointer to a parallel per-bone local
// pose-data array (0x40-byte/16-float stride, same rep-movsd-0x10 copy
// convention FUN_00960050 also used); entry count stored at (+8 array) - 4.
//
// The matched connection object (DFEEBD00 in the capture) ALSO carries its
// own ordered bone-NAME array (a separate "exportAnimation" property, 25
// entries, confirmed via direct live memory read of the object -- not
// proximity/adjacency): index 0 = "Wep_Root" (confirmed live: the first
// resolved bone index for this call was 0, matching); 1 = "Wep_Extra1"; 2 =
// "Wep_Trigger"; 3 = "Wep_Slide"; 4-5 = "Wep_Grenade1/2"; 6-7 = "Wep_Mag",
// "Wep_Mag_Ammo"; 8-10 = "Wep_Physic1/2/3"; 11-15 = "Wep_Belt1"-"Wep_Belt5";
// 16-18 = "Wep_Bipod1/2/3"; 19 = "IK_Joint_LeftHand"; 20 =
// "IK_Joint_RightHand"; 21-22 = "Wep_Extra2/3"; 23 = "Wep_Aim". NOTE: this
// name array's ORDER matching the output array's index order is confirmed
// only for index 0 (Wep_Root) via the live-captured first bone index: the
// rest is inferred from ordering and not independently index-verified.
// Also NOT confirmed: whether "IK_Joint_LeftHand"/"IK_Joint_RightHand"
// actually carry live-solved IK data or a static/placeholder value in this
// build -- named entries in this array don't by themselves prove the game
// runs real IK for them.
#define OFFSET_WEAPONBONECOMPOSE 0x683580u

// fb::ClientFadeManager -- not reversed/defined yet (screen fade, e.g.
// death/loading transitions; possibly useful later for comfort vignettes).
// Offset confirmed for this build, class layout is future work.
#define OFFSET_CLIENTFADEMANAGER 0x2AC9898u

// Debug renderer draw functions -- not on the v1 critical path, kept for
// reference. Two independent user-provided sources agree on singleton/
// drawtext/fillrect/drawrect; DRAWLINE and DRAWSPHERE disagree between
// sources (0x5A94C0 vs 0x5A8F90, and 0x5A6A40 vs old 0x5A65F0) -- treat
// those two specifically as unconfirmed until cross-checked again.
#define OFFSET_DBGRENDERER2_SINGLETON 0x59C9C0u
#define OFFSET_DBGRENDRAWTEXT 0x5A6860u
#define OFFSET_DBGRENDFILLRECT 0x5A9850u
#define OFFSET_DBGRENDRAWRECT 0x5A9640u
#define OFFSET_DBGRENDRAWLINE 0x5A94C0u    // unconfirmed -- second source said 0x5A8F90
#define OFFSET_DBGRENDRAWSPHERE 0x5A6A40u  // unconfirmed -- old header said 0x5A65F0

// GameWorld::RayCast -- CONFIRMED LIVE (2026-09-11) via a breakpoint hit
// during actual player weapon fire, stack-dumped at entry. __thiscall
// (ECX = GameWorld singleton "this", confirmed EAX==ECX at entry pointing
// to a stable object). Stack layout confirmed to match the signature below
// exactly, param-by-param, from a live capture:
//   ident = "BulletEntity updateTransformSync" (a real, confirmed string --
//     ties to the previously-unreached BulletEntity::updateTransformSync
//     method flagged in an earlier session as "first direct evidence of a
//     discrete bullet/projectile entity class")
//   rayCastTest = 0
//   start/end = Vec3* pointing to a SHORT segment (~11.1 units in the
//     captured case) -- confirmed to match the bullet's own per-tick
//     displacement: OFFSET_BULLETINITIALIZE's decoded velocity (~347.7
//     units/sec) divided by the independently-confirmed ~30Hz simulation
//     tick rate is ~11.6, an almost exact match. This is the traveling
//     bullet's own continuous-collision-detection raycast against world
//     geometry (walls/props), called every simulation tick from inside
//     BulletEntity::updateTransformSync -- confirmed shot-gated live (only
//     breaks while actively firing).
//   hits, maxHitCount=5, materialFlags=0x01000000 observed live; flags and
//     excluded not individually confirmed, trusted from the signature below.
//
// GameWorld::RayCast itself is GENERIC/shared infrastructure (called by many
// unrelated systems, not just bullets) -- any hook here MUST filter on the
// `ident` string to avoid affecting unrelated raycasts. See
// hooks/bullet_raycast_redirect_hook.cpp.
//
// Signature: bool RayCast(const char* ident, fb::GameWorld::RayCastTest rayCastTest,
//   fb::Vec3* start, fb::Vec3* end, fb::RayCastHit* hits, unsigned int maxHitCount,
//   unsigned int materialFlags, unsigned int flags,
//   eastl::fixed_vector<fb::PhysicsEntityBase const*, 8, 0>* excluded)
#define OFFSET_GAMEWORLD_RAYCAST 0x7ACBF0u

// --- NOT YET RE-VERIFIED against this build (carried over from the older,
// mismatched-image-size header; do not dereference until confirmed) ---
#define OFFSET_TOSCREENFUNCTION_UNVERIFIED 0xECAAB0u
#define OFFSET_SNOWROLLER_UNVERIFIED 0x2AEB8E0u

namespace mohw {

inline uintptr_t g_moduleBase = 0;
inline uintptr_t g_baseDelta = 0; // add to every OFFSET_* value
inline bool g_buildVerified = false;

// Must be called once after MOHW.exe's module is loaded (i.e. from the proxy
// DLL's DllMain / a Present hook on first call) before any Offset<>() use.
inline bool ResolveModule()
{
    HMODULE hMod = GetModuleHandleA(MOHW_MP);
    if (!hMod)
        return false;

    g_moduleBase = reinterpret_cast<uintptr_t>(hMod);
    g_baseDelta = g_moduleBase - static_cast<uintptr_t>(MOHW_MP_BASE);
    return true;
}

// Confirms the loaded module's image size matches the build these offsets
// were captured from. Refuses (returns false) rather than guess on mismatch --
// hooking/dereferencing offsets against a different build corrupts memory
// silently instead of crashing loudly.
inline bool VerifyBuild()
{
    if (g_moduleBase == 0)
        return false;

    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(g_moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(g_moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    g_buildVerified = (nt->OptionalHeader.SizeOfImage == MOHW_MP_SIZE);
    return g_buildVerified;
}

// Applies the ASLR delta to a static offset and casts to the requested
// pointer type. Callers dereference the result (e.g. **Offset<Main**>(...)**
// for a Singleton() that stores a pointer-to-pointer at that address).
template <typename T>
inline T Offset(uintptr_t staticOffset)
{
    return reinterpret_cast<T>(g_baseDelta + staticOffset);
}

} // namespace mohw
