#include "constantbuffer_hook.h"
#include "draw_trace_diag.h"
#include "eye_matched_fov.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/renderview.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <intrin.h> // _ReturnAddress -- capturing real MOHW.exe call sites for Ghidra (task #16)
#include <atomic>
#include <thread>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstdio>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_cbscan.log";
constexpr wchar_t kDummyWindowClass[] = L"MohwVrCbScanDummyWindow";

using MapFn = HRESULT(__stdcall*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                    D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(__stdcall*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using SetConstantBuffersFn = void(__stdcall*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
// DX11.1 partial-constant-buffer-range bind, on the ID3D11DeviceContext1
// extended interface -- a real live test showed the known projection buffer
// never bound via classic VSSetConstantBuffers during 1.3M+ real draws
// despite being confirmed present and correctly identified by content, so
// this game likely binds via this newer API instead.
using SetConstantBuffers1Fn = void(__stdcall*)(ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*,
                                                 const UINT*, const UINT*);
// CONFIRMED LOAD-BEARING (2026-08-23), bone-buffer hunt: an exact-size
// search on the existing Map/Unmap hook alone found zero hits for the
// player bone buffer (see UpdateKnownPlayerBoneBuffer's comment) --
// bone matrices for this skinned mesh are written via UpdateSubresource,
// not Map, confirmed once this hook was added (valid for
// D3D11_USAGE_DEFAULT buffers, unlike Map which needs USAGE_DYNAMIC).
// Never hooked anywhere in this project before now.
using UpdateSubresourceFn = void(__stdcall*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                               const void*, UINT, UINT);
// Draw-call tracing (2026-08-26) -- task: find the real mesh/render
// component (and, hopefully, an engine-level visibility flag on it) for
// whichever entity is currently locked, since patching the bone matrix
// buffer only affects what the vertex shader computes and can't stop
// whatever downstream system (shadow pass, occlusion culling, G-buffer)
// still expects the mesh to be there -- confirmed live as a stale-
// rendering artifact independent of alternating-eye stereo AND
// antialiasing, and independent of HOW the bones are patched (zeroed vs.
// height-offset). Hooking DrawIndexed itself (not a new mesh/component
// hook) is the anchor: it's where the CALLER (the game's own draw-
// submission code, via _ReturnAddress()) can be identified and traced
// backward from, and self->IAGetVertexBuffers/IAGetIndexBuffer at that
// exact moment fingerprints WHICH draw call this is without needing to
// have hooked those Set calls separately.
using DrawIndexedFn = void(__stdcall*)(ID3D11DeviceContext*, UINT, UINT, INT);

MapFn g_originalMap = nullptr;
UnmapFn g_originalUnmap = nullptr;
SetConstantBuffersFn g_originalVSSetConstantBuffers = nullptr;
SetConstantBuffersFn g_originalPSSetConstantBuffers = nullptr;
SetConstantBuffers1Fn g_originalVSSetConstantBuffers1 = nullptr;
UpdateSubresourceFn g_originalUpdateSubresource = nullptr;
DrawIndexedFn g_originalDrawIndexed = nullptr;
void* g_mapAddress = nullptr;
void* g_unmapAddress = nullptr;
void* g_vsSetAddress = nullptr;
void* g_psSetAddress = nullptr;
void* g_vsSet1Address = nullptr;
void* g_updateSubresourceAddress = nullptr;
void* g_drawIndexedAddress = nullptr;

// Set (regardless of whether bone-hide is even enabled) whenever a bone-
// buffer write is confirmed to be for the CURRENTLY LOCKED entity --
// consumed by the very next DrawIndexed call, which should be the draw
// that actually renders this entity's mesh (the write immediately
// precedes its own draw, same pattern as everything else discovered
// about this buffer this session). Cleared unconditionally on every
// DrawIndexed call so it only ever attributes to the one draw
// immediately following a matching write.
std::atomic<bool> g_nextDrawIsLockedEntity{false};

// Tracks constant buffers currently mapped for CPU write, so Unmap (where we
// read back what the game just wrote) knows the pointer/size Map returned.
// Low-frequency (a handful of CBs mapped per frame, not per draw call), so a
// mutex-guarded map is plenty fast here -- unlike Present/UpdateMatrices this
// is not a hot path needing lock-free counters.
struct MappedCbInfo
{
    void* data;
    UINT byteWidth;
    // Address inside MOHW.exe that called Map for this resource -- captured
    // via _ReturnAddress() at Map time (task: branch off DX11 constant-
    // buffer patching, which has needed a lot of iteration, and instead
    // find a stable engine function to hook directly via Ghidra). This is
    // ground-truth: guaranteed real code, confirmed by having actually
    // executed live, unlike an address merely traced through a call chain
    // and never independently verified against static disassembly (which
    // is exactly what went wrong with an earlier candidate address that
    // turned out to point at non-code data).
    void* mapReturnAddress;
};
std::mutex g_mappedMutex;
std::unordered_map<ID3D11Resource*, MappedCbInfo> g_mapped;

// Buffers confirmed (at Unmap time) to contain the projection signature.
// Populated so the VSSetConstantBuffers/PSSetConstantBuffers hooks below
// know which bound buffers are worth logging the slot for.
std::mutex g_knownMutex;
std::unordered_set<void*> g_knownProjectionBuffers;

// Phase 2C tracking: specifically the 80-byte buffer (the one confirmed in
// Phase 2B, not the larger/noisier matches). Held with a real AddRef since
// draw_duplication_hook.cpp uses this pointer outside the Map/Unmap call
// that discovered it.
std::mutex g_projMutex;
ID3D11Buffer* g_projBuffer = nullptr;
unsigned char g_projBytes[80];
bool g_projValid = false;

void UpdateKnownProjectionBuffer(ID3D11Resource* resource, const void* data, UINT byteWidth)
{
    if (byteWidth != 80)
        return;

    ID3D11Buffer* buffer = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || !buffer)
        return;

    std::lock_guard<std::mutex> lock(g_projMutex);
    if (g_projBuffer && g_projBuffer != buffer)
        g_projBuffer->Release();
    g_projBuffer = buffer; // holds the QueryInterface's AddRef
    memcpy(g_projBytes, data, 80);
    g_projValid = true;
}

std::atomic<int> g_totalMatchesLogged{0};
// Raised from 8: the first pass hit that cap within ~14s of hook install --
// almost certainly still launcher/menu/loading, not confirmed in-game
// camera with real rotation (menu-to-gameplay took longer than that). At
// one sample/2s, 90 samples covers a full 3 minutes, comfortably spanning
// the transition into confirmed active gameplay.
constexpr int kMaxMatchesLogged = 90;
std::atomic<unsigned long long> g_nextDumpAllowedMs{0};

constexpr int kMaxBindingsLogged = 20; // per-stage budget, see BindingCounterFor()

float ExpectedProjM11()
{
    // FIXED (2026-09-23): was reading GameRenderer::Singleton()->m_viewParams.view.m_desc.fovY -- confirmed via live
    // logging to be a STALE copy that never reflects eye_matched_fov.cpp's dynamically-written override (it's
    // bulk-copied from a different engine structure once per simulation tick, per sdk/mohw_offsets.h's
    // OFFSET_UPDATEVIEWCONSTANTS comment, not from what we write to transformPtr+0x48). That mismatch meant the
    // projection-signature match below could never succeed once eye-matched/true-frustum FOV went live, which broke
    // buffer identity discovery entirely -- draw_duplication_hook.cpp's simultaneous-stereo path (and anything else
    // depending on GetLastKnownViewBufferFullBytes) silently never got real data. Now prefers our own live value
    // (mohw::GetLastWrittenFovY(), set every frame by ApplyEyeMatchedFov), falling back to the old GameRenderer
    // path only if we haven't written anything yet (e.g. very early startup, before the first CommitViewTransform).
    float fovY = GetLastWrittenFovY();
    const char* source = "eye_matched_fov (live)";
    if (fovY <= 0.0f)
    {
        GameRenderer* gr = GameRenderer::Singleton();
        if (!gr)
        {
            static std::atomic<unsigned long long> nextNullLogMs{0};
            unsigned long long now = GetTickCount64();
            unsigned long long allowed = nextNullLogMs.load(std::memory_order_relaxed);
            if (now >= allowed && nextNullLogMs.compare_exchange_strong(allowed, now + 2000, std::memory_order_relaxed))
                MOHW_LOG(kLogFile, "ExpectedProjM11: no live fovY yet and GameRenderer::Singleton() is NULL");
            return 0.0f;
        }
        fovY = gr->m_viewParams.view.m_desc.fovY;
        source = "GameRenderer (fallback, no live value yet)";
    }

    static std::atomic<unsigned long long> nextFovLogMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextFovLogMs.load(std::memory_order_relaxed);
    if (now >= allowed && nextFovLogMs.compare_exchange_strong(allowed, now + 2000, std::memory_order_relaxed))
        MOHW_LOG(kLogFile, "ExpectedProjM11: fovY = %.4f rad (%.1f deg) [source: %s]", fovY, fovY * 57.29577951f, source);

    if (fovY <= 0.0f || fovY >= 3.14159f)
        return 0.0f;
    return 1.0f / tanf(fovY * 0.5f);
}

// View-matrix identification, structural approach (see
// docs/companion_process_findings.md's stereo follow-up work): an earlier
// attempt tried to cross-check a candidate matrix's translation against
// GameRenderer's CPU-side RenderView struct (the same "trust a known-good
// field" approach Phase 2B used for the projection scalar), but the CPU
// values for view/prevView/secondaryStreamingView never updated at all
// (stuck at identity/implausible constants) even during active gameplay --
// pointing at an unverified offset deeper in RenderView (there's a
// PAD(0x16C) gap between the confirmed-good fields like fovY and
// everything from m_fovX onward, including all the matrices) rather than a
// timing/precision issue. Without a disassembler to verify that offset,
// those CPU fields can't be trusted this deep into the struct.
//
// Instead: trust the STRUCTURAL/positional evidence. Live buffer dumps
// repeatedly show a textbook 4x4 transform (valid-looking rotation
// submatrix + [0,0,0,1] bottom row, translation changing plausibly with
// camera movement) at a consistent fixed byte offset, inside the specific
// 352-byte buffer that ALSO contains the CPU-confirmed projection
// signature (fovY-derived, independently trustworthy). Anchoring off that
// already-trusted projection match to identify "this is the right
// buffer", then reading a fixed relative offset within it, needs no
// unverified CPU field at all.
constexpr UINT kCombinedViewProjByteWidth = 352;
constexpr size_t kViewMatrixByteOffset = 48; // empirically observed across many live buffer dumps
constexpr size_t kViewMatrixByteSize = 64;   // one 4x4 matrix

// Precombined view*projection matrix (transposed, same layout convention
// as the two separate matrices above) -- found by computing what it
// SHOULD equal from the already-identified view/projection sub-matrices
// and searching the buffer's still-unidentified remainder for a match.
// Patching just the separate view sub-matrix above had no visible effect
// on gameplay geometry; this is almost certainly why -- the shader reads
// THIS precombined matrix instead of multiplying view and projection
// itself. Confirmed at a consistent offset across many live samples with
// different camera orientations, not a one-off coincidental match.
constexpr size_t kPrecombinedMatrixByteOffset = 176;
constexpr size_t kPrecombinedMatrixByteSize = 64;

// The ACTUAL matrix consumed by the main-scene vertex shaders -- found by
// dumping and disassembling the real shader bytecode via 3Dmigoto (task:
// patch per-eye view matrix with HMD pose + IPD offset). HLSL reflection
// on the skinning vertex shader showed this buffer as HLSL struct
// "viewConstants" bound at b2 (matching this buffer/slot exactly), with
// viewMatrix/projMatrix/viewProjMatrix (the three fields above) ALL
// marked [unused] by the compiler -- explaining why patching them
// produced literally zero visible effect despite being confirmed bound.
// The shader instead does camera-relative rendering (standard technique
// to avoid float precision loss far from the origin): it first computes
// `worldPos - cameraPos` (cameraPos at the offset below), THEN multiplies
// by crViewProjMatrix here -- which must therefore be a TRANSLATION-FREE
// view*projection (rotation only), since cameraPos already accounts for
// the camera's position and baking it into this matrix too would
// double-count it.
constexpr size_t kCrViewProjMatrixByteOffset = 240; // HLSL packoffset(c15), confirmed via shader disassembly
constexpr size_t kCrViewProjMatrixByteSize = 64;
constexpr size_t kCameraPosByteOffset = 320; // HLSL packoffset(c20), float3 (12 bytes)
constexpr size_t kCameraPosByteSize = 12;

std::mutex g_viewMutex;
ID3D11Buffer* g_viewBuffer = nullptr;
unsigned char g_viewFullBytes[kCombinedViewProjByteWidth];
bool g_viewValid = false;

// Player's own bone-matrix buffer -- motion-controls groundwork, task
// "find the real per-bone skinning array" (see
// project_mohw_alternating_eye_investigation.md memory's sibling thread and
// docs/shader_dumps/acf0b7e0f77c6772-vs_replace_player_body_partial_hide.txt
// for the shader this feeds: `float4x3 boneVectors[60]` at register b0).
//
// IDENTIFIED LIVE (2026-08-23), replacing an earlier diagnostic-only pass:
// the real per-draw upload is a 4096-byte constant buffer (not the
// shader-reflection-derived 2880 -- allocated at a rounder, allocator-
// friendly size with ~1216 bytes of padding past the 60 real bone entries),
// written through ONE consistent call site, shared by EVERY skinned draw
// each frame -- player, NPCs, and the weapon's own local-space bones all
// go through this same upload path, so byte size alone can't tell them
// apart (unlike the view/projection buffer above, a single global
// per-frame camera buffer that's safe to cache by identity alone).
//
// Disambiguated via bone-0's world-space distance to the already-trusted
// live camera position (view buffer's cameraPos): live samples showed the
// player's own root bone at distance 1.4-1.9 units from camera, while
// NPCs sharing the same call site sat at 14+ up to 700+ units away, and
// the weapon's local-space bones (small values clustered near local
// origin, not world space) produced distances that don't mean anything in
// this comparison at all. kPlayerBoneDistanceThresholdUnits sits
// comfortably above the player cluster and well below the nearest NPC
// value seen.
//
// Layout confirmed directly from the shader disassembly (NOT the 4-row
// left/up/forward/trans convention used for the camera transform elsewhere
// in this project): each bone is a 3-row x 4-column affine matrix --
// `dp4 r2.x, r0.xyzw, cb0[bone+0].xyzw` etc., where r0=(vertexPos,1) -- so
// translation is packed as the .w component of EACH of the 3 rows
// (tx=row0.w, ty=row1.w, tz=row2.w), floats [3],[7],[11] of the 12-float
// (48-byte) entry, not a separate 4th row. Byte 0 of the buffer is always
// structurally bone 0 (that's what the shader's own cb0[0] indexing reads),
// regardless of whether its rotation submatrix happens to look "clean" in
// any given frame -- no need to search for a "valid-looking" start offset
// the way the discovery-phase diagnostic did.
// Entity-identity hook (2026-08-25) -- see mohw_offsets.h's
// OFFSET_FUNCTIONCONSTANTBUFFERUPDATE_CALLER_ENTITY comment for the live
// investigation this came from. Hooks the CPU-side bone-buffer updater
// directly (a raw internal function, not a D3D11 API -- same MinHook +
// __thiscall-member-function-pointer-as-plain-address technique
// engine_function_hook.cpp already established, see that file's
// ThisCallTrampoline comment for why this is safe on the MSVC x86 ABI),
// stashes arg1 into g_lastEntityPoseArg1 whenever called from the
// "entity-rich" caller specifically, then forwards to the original
// unconditionally -- this hook must never alter real game behavior, only
// observe it. UpdateKnownPlayerBoneBuffer (below) reads the stash moments
// later, same thread, same call chain (this function itself calls Map()
// internally) -- ordering is safe under this project's existing single-
// render-thread assumption.
using FunctionConstantBufferUpdateFn = void(__thiscall*)(void* thisPtr, void* arg1, void* arg2, void* arg3,
                                                             void* arg4, void* arg5);
FunctionConstantBufferUpdateFn g_originalFunctionConstantBufferUpdate = nullptr;
void* g_functionConstantBufferUpdateAddress = nullptr;
void* g_expectedEntityRichCaller = nullptr; // resolved via Offset() at install time

// Most recent arg1 seen from the entity-rich caller -- read by
// UpdateKnownPlayerBoneBuffer/ApplyBoneHideIfEnabled as "which entity is
// this write for", replacing buffer identity (confirmed live to be the
// same single shared object for every skinned draw, no discrimination at
// all) as the real per-entity signal.
std::atomic<void*> g_lastEntityPoseArg1{nullptr};

// Same MSVC __thiscall-via-member-function-pointer technique as
// engine_function_hook.cpp's ThisCallTrampoline -- see that struct's
// comment for why reinterpret_cast<void*>(this) safely recovers the real
// ECX the game passed in, and why a plain member-function pointer can be
// type-punned to a raw code address here (no virtuals/multiple
// inheritance).
// Defined further down (needs SehSafeRead and g_lockedEntityId, both
// declared later in this file) -- forward-declared here so
// BoneEntityIdTrampoline::Hooked can call it. Task: find the arm bone
// hierarchy for an IK solver (2026-08-25) -- decodes arg3, the pose/
// index-pair table read by 00771670's own per-bone loop (disassembly:
// word bone-count at [arg3], then bone-count 8-byte records starting at
// [arg3+4], each a dword + 3 individual bytes, fed into FUN_0076F1A0 per
// bone). Meaning unconfirmed from static analysis alone -- this dumps the
// live values for whichever entity is currently locked so the real
// content can be inspected for a plausible parent/hierarchy encoding.
void LogPoseTableIfLockedEntity(void* entityId, void* arg3, void* caller);

// Forward-declared for the same reason -- defined near Hooked_DrawIndexed
// further down (needs BufferPairHash, declared there), called here and
// from CheckBoneHideHotkeys' Numpad0 handler whenever the lock moves to a
// DIFFERENT entity, so stale vertex/index-buffer pairs from whatever was
// PREVIOUSLY locked can't keep suppressing draws for the new lock target.
void ClearKnownEntityBuffers();

struct BoneEntityIdTrampoline
{
    void Hooked(void* arg1, void* arg2, void* arg3, void* arg4, void* arg5)
    {
        void* thisPtr = reinterpret_cast<void*>(this);
        void* caller = _ReturnAddress();
        if (caller == g_expectedEntityRichCaller)
            g_lastEntityPoseArg1.store(arg1, std::memory_order_relaxed);
        // Check BOTH call sites' arg3 (2026-08-26), not just the
        // entity-rich one -- live data showed the entity-rich caller's
        // arg3 is ALWAYS an empty table (boneCount=0), completely
        // consistently, across every entity tried. The two callers
        // strictly alternate for the same object (confirmed early in this
        // investigation), so tag whichever call this is with the most
        // recently identified entity (from the entity-rich caller) --
        // their roles may be split: arg1 identity from one, real bone
        // data in arg3 from the other.
        void* currentEntity = g_lastEntityPoseArg1.load(std::memory_order_relaxed);
        LogPoseTableIfLockedEntity(currentEntity, arg3, caller);
        g_originalFunctionConstantBufferUpdate(thisPtr, arg1, arg2, arg3, arg4, arg5);
    }
};

bool InstallBoneEntityIdHook()
{
    if (g_moduleBase == 0 || !g_buildVerified)
    {
        MOHW_LOG(kLogFile, "InstallBoneEntityIdHook: module base not resolved / build not verified yet -- "
                            "refusing to install");
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "InstallBoneEntityIdHook: MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    g_functionConstantBufferUpdateAddress = Offset<void*>(OFFSET_FUNCTIONCONSTANTBUFFERUPDATE);
    g_expectedEntityRichCaller = Offset<void*>(OFFSET_FUNCTIONCONSTANTBUFFERUPDATE_CALLER_ENTITY);

    auto memberFn = &BoneEntityIdTrampoline::Hooked;
    static_assert(sizeof(memberFn) == sizeof(void*),
                   "BoneEntityIdTrampoline::Hooked's member-function-pointer isn't a plain code address -- "
                   "check for accidental virtual functions or multiple inheritance");
    void* detour = nullptr;
    memcpy(&detour, &memberFn, sizeof(detour));

    MH_STATUS s = MH_CreateHook(g_functionConstantBufferUpdateAddress, detour,
                                  reinterpret_cast<void**>(&g_originalFunctionConstantBufferUpdate));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "InstallBoneEntityIdHook: MH_CreateHook(@ %p) FAILED: %s",
                  g_functionConstantBufferUpdateAddress, MH_StatusToString(s));
        g_functionConstantBufferUpdateAddress = nullptr;
        return false;
    }
    s = MH_EnableHook(g_functionConstantBufferUpdateAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "InstallBoneEntityIdHook: MH_EnableHook FAILED: %s", MH_StatusToString(s));
        return false;
    }

    MOHW_LOG(kLogFile, "Bone entity-id hook installed @ %p (expected entity-rich caller = %p)",
              g_functionConstantBufferUpdateAddress, g_expectedEntityRichCaller);
    return true;
}

void RemoveBoneEntityIdHook()
{
    if (g_functionConstantBufferUpdateAddress)
    {
        MH_DisableHook(g_functionConstantBufferUpdateAddress);
        MH_RemoveHook(g_functionConstantBufferUpdateAddress);
        g_functionConstantBufferUpdateAddress = nullptr;
    }
}

constexpr UINT kPlayerBoneBufferByteWidth = 4096;
// Was a fixed constant (5.0) -- now sdk/settings.h's
// GetPlayerBoneDistanceThreshold, live-tunable (bracket keys). See that
// declaration's comment for why: the confirmed true player-distance range
// (0.48-1.9 units) overlaps the range at which nearby objects sharing this
// same buffer also get caught, so no single hardcoded value works across
// every scene -- this needs to be dialed in interactively.

std::mutex g_playerBoneMutex;
ID3D11Buffer* g_playerBoneBuffer = nullptr;
unsigned char g_playerBoneBytes[kPlayerBoneBufferByteWidth];
bool g_playerBoneValid = false;
std::atomic<int> g_playerBoneIdentityLogs{0};

// Manual capture (2026-08-25), see CheckBoneHideHotkeys' End hotkey --
// live-diagnosed bug fix: the passive auto-lock below kept re-evaluating
// and potentially re-locking to a DIFFERENT buffer on every single
// qualifying write, forever -- meaning a nearby NPC whose own separate
// buffer object also happened to pass the distance check at some point
// could silently hijack the cache away from the player later in the same
// session (confirmed live: an adjacent AI body got hidden alongside the
// player, consistent with exactly this). g_captureRequested arms a
// one-shot capture on the very next qualifying write; once
// g_playerBoneManuallyCaptured is true, the passive logic below stops
// ever changing WHICH buffer is "the player" -- only a fresh hotkey press
// can move the lock again. Load-bearing for testing near NPCs, e.g. the
// user's own case: loading into a level with an AI already standing next
// to the player.
std::atomic<bool> g_captureRequested{false};
std::atomic<bool> g_playerBoneManuallyCaptured{false};

// The real per-entity lock target (2026-08-25), replacing buffer identity
// (g_playerBoneBuffer, proven to never change -- see the comment below).
// Set either by End (locks whatever entity happens to be the very next
// qualifying write -- live-diagnosed to sometimes grab the WEAPON's
// entity instead of the player's own BODY, since both are close enough to
// the camera to pass the distance bootstrap and there's no way to tell
// which one a single opportunistic capture landed on) or by cycling
// through g_entityCandidates with Numpad0 below.
std::atomic<void*> g_lockedEntityId{nullptr};

// Render-item correlation hook (2026-08-26) -- see mohw_offsets.h's
// OFFSET_DRAWBATCHDISPATCH comment. Solves a real problem hit live: manually
// correlating x32dbg log-breakpoint output against this mod's own DRAW
// TRACE log by eyeballing timestamps proved too unreliable (many hits
// within the same millisecond, no way to line them up with confidence).
// This does the correlation IN-PROCESS instead -- reads
// g_nextDrawIsLockedEntity (a peek via .load(), NOT the consuming
// .exchange() Hooked_DrawIndexed uses, so this doesn't disturb that
// logic) at the exact moment arg1 (the per-item EB9xxxxx-range render
// descriptor) is available, giving a deterministic yes/no instead of a
// guess.
using DrawBatchDispatchFn = void(__thiscall*)(void* thisPtr, void* arg1, void* arg2, void* arg3, void* arg4);
DrawBatchDispatchFn g_originalDrawBatchDispatch = nullptr;
void* g_drawBatchDispatchAddress = nullptr;
std::mutex g_drawBatchLogCountsMutex;
std::unordered_map<void*, int> g_drawBatchLogCounts;
constexpr int kMaxDrawBatchLogsPerEntity = 30;

struct DrawBatchDispatchTrampoline
{
    void Hooked(void* arg1, void* arg2, void* arg3, void* arg4)
    {
        void* thisPtr = reinterpret_cast<void*>(this);
        if (g_playerBoneManuallyCaptured.load(std::memory_order_relaxed) &&
            g_nextDrawIsLockedEntity.load(std::memory_order_relaxed))
        {
            void* lockedEntityId = g_lockedEntityId.load(std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(g_drawBatchLogCountsMutex);
            int count = ++g_drawBatchLogCounts[lockedEntityId];
            if (count <= kMaxDrawBatchLogsPerEntity)
                MOHW_LOG(kLogFile, "RENDER ITEM CORRELATION #%d: entity=%p arg1(render item)=%p", count,
                          lockedEntityId, arg1);
        }
        g_originalDrawBatchDispatch(thisPtr, arg1, arg2, arg3, arg4);
    }
};

bool InstallDrawBatchDispatchHook()
{
    if (g_moduleBase == 0 || !g_buildVerified)
    {
        MOHW_LOG(kLogFile, "InstallDrawBatchDispatchHook: module base not resolved / build not verified yet -- "
                            "refusing to install");
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "InstallDrawBatchDispatchHook: MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    g_drawBatchDispatchAddress = Offset<void*>(OFFSET_DRAWBATCHDISPATCH);

    auto memberFn = &DrawBatchDispatchTrampoline::Hooked;
    static_assert(sizeof(memberFn) == sizeof(void*),
                   "DrawBatchDispatchTrampoline::Hooked's member-function-pointer isn't a plain code address -- "
                   "check for accidental virtual functions or multiple inheritance");
    void* detour = nullptr;
    memcpy(&detour, &memberFn, sizeof(detour));

    MH_STATUS s = MH_CreateHook(g_drawBatchDispatchAddress, detour,
                                  reinterpret_cast<void**>(&g_originalDrawBatchDispatch));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "InstallDrawBatchDispatchHook: MH_CreateHook(@ %p) FAILED: %s",
                  g_drawBatchDispatchAddress, MH_StatusToString(s));
        g_drawBatchDispatchAddress = nullptr;
        return false;
    }
    s = MH_EnableHook(g_drawBatchDispatchAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "InstallDrawBatchDispatchHook: MH_EnableHook FAILED: %s", MH_StatusToString(s));
        return false;
    }

    MOHW_LOG(kLogFile, "Draw-batch-dispatch correlation hook installed @ %p", g_drawBatchDispatchAddress);
    return true;
}

void RemoveDrawBatchDispatchHook()
{
    if (g_drawBatchDispatchAddress)
    {
        MH_DisableHook(g_drawBatchDispatchAddress);
        MH_RemoveHook(g_drawBatchDispatchAddress);
        g_drawBatchDispatchAddress = nullptr;
    }
}

// Candidate list (2026-08-25): every DISTINCT entityId seen with a
// qualifying (within-threshold) distance gets recorded here, unconditionally,
// regardless of capture state -- normal play near the player's own body and
// weapon populates this within seconds. Numpad0 cycles g_lockedEntityId
// through these, letting the user A/B test which candidate is actually the
// body vs. the weapon (or an NPC) without needing to precisely time a fresh
// End press.
constexpr int kMaxEntityCandidates = 24; // was 8 -- live testing showed terrain chunks alone can burn through a small cap before the player's own body is ever seen
std::mutex g_entityCandidatesMutex;
void* g_entityCandidates[kMaxEntityCandidates] = {};
int g_entityCandidateCount = 0;
std::atomic<int> g_entityCandidateCycleIndex{-1};

void TrackEntityCandidate(void* entityId, float distance)
{
    if (!entityId)
        return;
    std::lock_guard<std::mutex> lock(g_entityCandidatesMutex);
    for (int i = 0; i < g_entityCandidateCount; ++i)
    {
        if (g_entityCandidates[i] == entityId)
            return; // already known
    }
    if (g_entityCandidateCount >= kMaxEntityCandidates)
        return; // candidate list full -- keep the first N found this session
    g_entityCandidates[g_entityCandidateCount] = entityId;
    MOHW_LOG(kLogFile, "new entity candidate #%d: entityId=%p distance=%.3f (Numpad0 cycles the lock through these)",
              g_entityCandidateCount, entityId, distance);
    ++g_entityCandidateCount;
}

// Isolated in its own function with no C++ objects requiring unwinding in
// its frame -- MSVC's __try/__except can't coexist with those in the same
// function (C2712), same reason engine_function_hook.cpp's own copy of
// this exists (not shared/exported from there -- small and self-contained
// enough to not be worth a header dependency for).
bool SehSafeRead(void* dst, const void* src, size_t size)
{
    __try
    {
        memcpy(dst, src, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Pose/index-pair table decode (2026-08-25) -- see the forward
// declaration's comment above BoneEntityIdTrampoline. Gated to the
// CURRENTLY LOCKED entity only (once you've cycled to a specific group
// with Numpad0/End) and capped PER ENTITY (not one shared counter -- a
// planned multi-group test session, cycling gun/rifle/legs/hands one at a
// time, would otherwise burn the whole budget on whichever group gets
// animated first), since this runs inside a hot per-draw hook and would
// otherwise spam every frame.
// BUG FIX (2026-08-26), live-diagnosed: the cap used to count every
// ATTEMPT (including calls where the per-bone loop turns out to be empty,
// boneCount==0 -- a separate, independent gate from whether Map() itself
// fires at all, confirmed live: `cmp dx,word[ebx]; jae [skip loop]` is its
// own check, distinct from the dirty-flag check earlier in the function).
// Empty calls happen far more often than non-empty ones for a given
// entity, so a shared attempt-based cap exhausted itself on all-zero
// reads before ever catching a real one. Now tracks attempts and
// successes separately per entity -- only a genuinely non-empty table
// counts against kMaxPoseTableLogsPerEntity, and "implausible" logging is
// separately throttled so it doesn't spam forever while waiting for a
// real one.
struct PoseTableEntityState
{
    int attempts = 0;
    int successes = 0;
};
std::mutex g_poseTableLogCountsMutex;
std::unordered_map<void*, PoseTableEntityState> g_poseTableEntityState;
constexpr int kMaxPoseTableLogsPerEntity = 3;
constexpr int kMaxImplausibleLogsPerEntity = 5;

void LogPoseTableIfLockedEntity(void* entityId, void* arg3, void* caller)
{
    if (!entityId || !arg3)
        return;
    if (!g_playerBoneManuallyCaptured.load(std::memory_order_relaxed))
        return;
    if (entityId != g_lockedEntityId.load(std::memory_order_relaxed))
        return;

    unsigned short boneCount = 0;
    if (!SehSafeRead(&boneCount, arg3, sizeof(boneCount)))
        return; // transient read failure -- don't spam, just skip this call

    std::lock_guard<std::mutex> lock(g_poseTableLogCountsMutex);
    PoseTableEntityState& state = g_poseTableEntityState[entityId];
    if (state.successes >= kMaxPoseTableLogsPerEntity)
        return;

    if (boneCount == 0 || boneCount > 128) // sane upper bound -- kBoneCount (60) isn't declared yet at this point in the file
    {
        ++state.attempts;
        if (state.attempts <= kMaxImplausibleLogsPerEntity)
            MOHW_LOG(kLogFile,
                      "LogPoseTableIfLockedEntity: implausible bone count %u at arg3=%p caller=%p for entity=%p, "
                      "skipping (attempt %d, still watching for a non-empty call)",
                      boneCount, arg3, caller, entityId, state.attempts);
        return;
    }
    int thisEntityCount = ++state.successes;

    // Record layout confirmed from 00771670's per-bone loop disassembly:
    // record i starts at byte offset (4 + 8*i) from arg3 -- a 4-byte dword
    // followed by 3 individual bytes (a 4th byte at the record's end is
    // read nowhere in that loop, likely padding). Dumping the 2 header
    // bytes at offset 2-3 too in case they carry meaning.
    constexpr size_t kRecordsOffset = 4;
    constexpr size_t kRecordStride = 8;
    size_t totalBytes = kRecordsOffset + static_cast<size_t>(boneCount) * kRecordStride;
    std::vector<unsigned char> bytes(totalBytes);
    if (!SehSafeRead(bytes.data(), arg3, totalBytes))
    {
        MOHW_LOG(kLogFile, "LogPoseTableIfLockedEntity: access violation reading full table (arg3=%p, %zu bytes)",
                  arg3, totalBytes);
        return;
    }

    std::string dump;
    char buf[48];
    snprintf(buf, sizeof(buf), "boneCount=%u header[2..3]=%02X%02X ", boneCount, bytes[2], bytes[3]);
    dump += buf;
    for (unsigned short i = 0; i < boneCount; ++i)
    {
        const unsigned char* record = bytes.data() + kRecordsOffset + i * kRecordStride;
        unsigned int dwordPart;
        memcpy(&dwordPart, record, 4);
        snprintf(buf, sizeof(buf), "[%u]:dw=%08X b4=%02X b5=%02X b6=%02X ", i, dwordPart, record[4], record[5],
                  record[6]);
        dump += buf;
    }
    MOHW_LOG(kLogFile, "pose table dump #%d for locked entity=%p arg3=%p caller=%p: %s", thisEntityCount, entityId,
              arg3, caller, dump.c_str());
}

// Diagnostic (2026-08-25) -- task: find a real engine-level mesh/entity
// visibility flag near entityId (arg1 -- see
// OFFSET_FUNCTIONCONSTANTBUFFERUPDATE_CALLER_ENTITY) as a cleaner
// alternative to patching bone matrices at all. Patching bones (whether
// zeroed or height-offset -- both tried, both left the exact same "stale
// rendering behind where the mesh was" artifact, live-confirmed
// independent of alternating-eye stereo AND antialiasing) only affects
// what the VERTEX SHADER computes; it can't stop the engine's own
// occlusion-culling/shadow/G-buffer submission systems from still
// expecting that mesh to be there, which any real per-mesh visibility
// flag would fix instead. Dumps 0x20 bytes BEFORE entityId (in case it
// points mid-struct) through 0x200 bytes after, as hex, so the actual
// struct layout can be eyeballed for a plausible bool/flag byte without
// needing a live x32dbg session for this step.
void DumpEntityStructIfPossible(void* entityId)
{
    if (!entityId)
        return;
    constexpr ptrdiff_t kBefore = 0x20;
    constexpr size_t kAfter = 0x200;
    unsigned char* base = reinterpret_cast<unsigned char*>(entityId) - kBefore;
    unsigned char bytes[kBefore + kAfter]{};
    bool ok = SehSafeRead(bytes, base, sizeof(bytes));
    if (!ok)
    {
        MOHW_LOG(kLogFile, "DumpEntityStructIfPossible: access violation reading around entityId=%p, skipping",
                  entityId);
        return;
    }

    std::string dump;
    char buf[16]; // "%08X " is 9 chars + null -- was 8 (too small, truncated/corrupted the log output)
    for (size_t i = 0; i < sizeof(bytes); i += 4)
    {
        unsigned int word;
        memcpy(&word, bytes + i, 4);
        snprintf(buf, sizeof(buf), "%08X ", word);
        dump += buf;
        if (i % 16 == 12)
            dump += "| ";
    }
    MOHW_LOG(kLogFile,
              "entity struct dump: entityId=%p, showing [entityId-0x%tX .. entityId+0x%zX), 4 bytes per group, "
              "entityId-0x%tX first: %s",
              entityId, kBefore, kAfter, kBefore, dump.c_str());
}

// BUG FIX (2026-08-25), live-diagnosed: identity locking (g_playerBoneBuffer
// above) turned out to be a no-op discriminator, not a real one -- live
// evidence (a captured/locked buffer STILL hiding an adjacent NPC body and a
// nearby trashcan, and objects reliably reappearing the instant they're
// turned out of view) means this 4096-byte buffer is a single object
// rewritten in place for EVERY skinned draw call in sequence (player, each
// visible NPC, props, weapon), not a dedicated per-entity allocation -- so
// "same ID3D11Buffer* as before" is true for literally every draw, and
// "currently within the distance threshold" just picks out whichever
// object's turn it happens to be when a nearby one is also being drawn.
// Neither signal can ever discriminate on its own. This diagnostic tracks
// every distinct RESOURCE pointer (in case it's actually a small round-robin
// POOL rather than truly one buffer -- would show up as >1 distinct
// pointers) and every distinct WRITE CALL SITE (mapReturnAddress/
// callerAddress, already captured at Map/UpdateSubresource time but unused
// until now) seen for this exact byte width, to find out whether the
// upload path differs per object type (an easy win, if so) or is one shared
// utility function used by all of them (meaning identifying the true
// object requires a call-stack level ABOVE this one -- see
// docs/motion_controls_research.md-style live x32dbg breakpoint-and-walk-up
// at whatever call site this logs, same approach already validated project-
// wide for exactly this kind of static-analysis ambiguity).
std::mutex g_boneDiagMutex;
std::unordered_set<ID3D11Resource*> g_distinctBoneResourcesSeen;
std::unordered_map<void*, long long> g_boneCallSiteCounts;

void LogBoneBufferDiagnostics(ID3D11Resource* resource, void* callerAddress, float distance)
{
    std::lock_guard<std::mutex> lock(g_boneDiagMutex);
    bool newResource = g_distinctBoneResourcesSeen.insert(resource).second;
    bool newCaller = false;
    long long callerCount = 0;
    if (callerAddress)
    {
        newCaller = (g_boneCallSiteCounts.find(callerAddress) == g_boneCallSiteCounts.end());
        callerCount = ++g_boneCallSiteCounts[callerAddress];
    }
    if (newResource || newCaller)
    {
        MOHW_LOG(kLogFile,
                  "bone buffer diag: resource=%p (newResource=%d totalDistinctResources=%zu) "
                  "callSite=%p (newCallSite=%d totalDistinctCallSites=%zu thisCallSiteCount=%lld) distance=%.3f",
                  static_cast<void*>(resource), newResource, g_distinctBoneResourcesSeen.size(), callerAddress,
                  newCaller, g_boneCallSiteCounts.size(), callerCount, distance);
    }
}

float DistanceBone0ToKnownCamera(const unsigned char* boneBufferBytes)
{
    unsigned char viewFull[kCombinedViewProjByteWidth];
    {
        std::lock_guard<std::mutex> lock(g_viewMutex);
        if (!g_viewValid)
            return -1.0f;
        memcpy(viewFull, g_viewFullBytes, kCombinedViewProjByteWidth);
    }
    const float* cameraPos = reinterpret_cast<const float*>(viewFull + kCameraPosByteOffset);
    const float* bone0 = reinterpret_cast<const float*>(boneBufferBytes);

    float dx = bone0[3] - cameraPos[0];
    float dy = bone0[7] - cameraPos[1];
    float dz = bone0[11] - cameraPos[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

void UpdateKnownPlayerBoneBuffer(ID3D11Resource* resource, const void* data, UINT byteWidth, void* callerAddress)
{
    if (byteWidth != kPlayerBoneBufferByteWidth)
        return;

    float distance = DistanceBone0ToKnownCamera(reinterpret_cast<const unsigned char*>(data));
    LogBoneBufferDiagnostics(resource, callerAddress, distance); // see g_boneDiagMutex's comment above

    // Read whatever the entity-id hook most recently stashed -- this write's
    // Map()/UpdateSubresource() call happens synchronously, moments after
    // OFFSET_FUNCTIONCONSTANTBUFFERUPDATE_CALLER_ENTITY ran (same thread,
    // same nested call), so this is that same call's entity id, not a stale
    // one from some earlier draw.
    void* entityId = g_lastEntityPoseArg1.load(std::memory_order_relaxed);

    if (distance < 0.0f || distance > GetPlayerBoneDistanceThreshold())
        return; // camera not known yet, or this write belongs to an NPC/the weapon, not the player

    TrackEntityCandidate(entityId, distance); // unconditional -- see its own comment above

    ID3D11Buffer* buffer = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || !buffer)
        return;

    std::lock_guard<std::mutex> lock(g_playerBoneMutex);

    bool captureArmed = g_captureRequested.load(std::memory_order_relaxed);
    bool manuallyCaptured = g_playerBoneManuallyCaptured.load(std::memory_order_relaxed);

    // Once manually captured (and no fresh capture is armed), ignore every
    // OTHER candidate entirely, no matter how well it satisfies distance --
    // this is what stops a nearby NPC from hijacking the lock later. Still
    // refresh the cached BYTES for the already-captured buffer, just never
    // change WHICH buffer that is.
    if (manuallyCaptured && !captureArmed && buffer != g_playerBoneBuffer)
    {
        buffer->Release(); // this QueryInterface's own ref -- not the one being kept
        return;
    }

    if (buffer != g_playerBoneBuffer && g_playerBoneIdentityLogs.load() < 20)
    {
        ++g_playerBoneIdentityLogs;
        MOHW_LOG(kLogFile, "player bone buffer identity: new=%p previous=%p distance=%.3f%s",
                  static_cast<void*>(buffer), static_cast<void*>(g_playerBoneBuffer), distance,
                  captureArmed ? " [MANUAL CAPTURE]" : "");
    }
    if (g_playerBoneBuffer && g_playerBoneBuffer != buffer)
        g_playerBoneBuffer->Release();
    g_playerBoneBuffer = buffer; // holds the QueryInterface's AddRef
    memcpy(g_playerBoneBytes, data, kPlayerBoneBufferByteWidth);
    g_playerBoneValid = true;

    if (captureArmed)
    {
        g_playerBoneManuallyCaptured.store(true, std::memory_order_relaxed);
        g_captureRequested.store(false, std::memory_order_relaxed);
        g_lockedEntityId.store(entityId, std::memory_order_relaxed);
        ClearKnownEntityBuffers();
        MOHW_LOG(kLogFile,
                  "player bone buffer CAPTURED and locked: buffer=%p entityId=%p (distance=%.3f) -- "
                  "ApplyBoneHideIfEnabled now gates on entityId, not buffer identity; will not change again "
                  "until the next capture request",
                  static_cast<void*>(buffer), entityId, distance);
        DumpEntityStructIfPossible(entityId);
    }
}

// Diagnostic for "some geometry doesn't follow the patched view" (task:
// patch per-eye view matrix with HMD pose + IPD offset -- weapon/character
// follow head tracking correctly, but static props/buildings/NPCs don't).
// Leading theory: the engine uses MORE than one live instance of this
// 352-byte buffer (e.g. ring-buffered per draw-batch, or separate
// camera-relative origins for different object categories for float
// precision), and g_viewBuffer -- which only ever caches the SINGLE most
// recently matched instance -- silently only patches whichever one last
// won the race, leaving any other simultaneously-active instance(s)
// completely untouched. Tracks every distinct buffer pointer ever seen
// matching the projection signature this session; if this grows past 1-2
// entries (beyond what a single level/scene reload would explain), that
// confirms multiple simultaneous instances exist and g_viewBuffer's
// single-instance design needs to become multi-instance.
std::unordered_set<void*> g_distinctViewBuffersSeen;
std::atomic<int> g_viewBufferIdentityChangeLogs{0};

// Every distinct MOHW.exe address that ever called Map on the identified
// view buffer -- ground-truth real code addresses (confirmed by having
// actually executed), for jumping straight to in Ghidra. Whichever address
// calls this the LEAST often is the best lead: the buffer is uploaded at
// most a few times per frame, so a low-count address is likely a
// higher-level, once-per-frame camera/view-update function -- a much
// cleaner hook target than the per-draw-call constant-buffer patching
// this project has needed a lot of iteration to get working (task: branch
// off DX11 patching, find a stable engine function to hook instead).
std::mutex g_viewMapCallersMutex;
std::unordered_map<void*, long long> g_viewMapCallers;

void UpdateKnownViewBuffer(ID3D11Resource* resource, const void* data, UINT byteWidth, void* mapReturnAddress)
{
    if (byteWidth != kCombinedViewProjByteWidth)
        return;

    ID3D11Buffer* buffer = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || !buffer)
        return;

    std::lock_guard<std::mutex> lock(g_viewMutex);
    if (buffer != g_viewBuffer)
    {
        bool isNew = g_distinctViewBuffersSeen.insert(buffer).second;
        if (g_viewBufferIdentityChangeLogs.load() < 40)
        {
            ++g_viewBufferIdentityChangeLogs;
            MOHW_LOG(kLogFile,
                      "view buffer identity change: new=%p previous=%p isNewPointerThisSession=%d "
                      "totalDistinctPointersThisSession=%zu",
                      static_cast<void*>(buffer), static_cast<void*>(g_viewBuffer), isNew,
                      g_distinctViewBuffersSeen.size());
        }
    }
    if (g_viewBuffer && g_viewBuffer != buffer)
        g_viewBuffer->Release();
    g_viewBuffer = buffer; // holds the QueryInterface's AddRef
    memcpy(g_viewFullBytes, data, kCombinedViewProjByteWidth);
    g_viewValid = true;

    if (mapReturnAddress)
    {
        std::lock_guard<std::mutex> callerLock(g_viewMapCallersMutex);
        bool firstSighting = (g_viewMapCallers.find(mapReturnAddress) == g_viewMapCallers.end());
        long long count = ++g_viewMapCallers[mapReturnAddress];
        if (firstSighting)
        {
            MOHW_LOG(kLogFile,
                      "NEW view-buffer Map call site: absolute address %p (module base 0x00400000, zero ASLR "
                      "delta confirmed) -- distinct call sites so far: %zu",
                      mapReturnAddress, g_viewMapCallers.size());
        }
        else if (count % 5000 == 0)
        {
            MOHW_LOG(kLogFile, "view-buffer Map call site %p now at %lld calls", mapReturnAddress, count);
        }
    }
}

// Once g_viewBuffer is identified (via UpdateKnownViewBuffer's projection-
// anchored match above), refreshes the cached full-buffer snapshot on
// EVERY subsequent write to that specific buffer OBJECT -- not just writes
// that happen to also contain the projection signature. This buffer holds
// far more than just view+projection (roughly 200 bytes past the
// projection region are still unidentified, quite possibly separate
// weapon/viewmodel transform data given what broke when this was missing),
// and WriteRawBytesToViewMatrix has to reconstruct-and-rewrite the WHOLE
// 352 bytes (dynamic constant buffers can't be safely partially updated --
// see WriteRawBytesToViewMatrix's own comment). A cache that's only
// refreshed when the projection scalar happens to match could be stale by
// the time we patch+restore it, silently overwriting whatever the game
// most recently wrote to the OTHER ~200 bytes with old data -- confirmed
// live: this exact staleness gap made the player's weapon/character mesh
// disappear (down to just a scope texture and the aim cursor) the first
// time view-matrix patching was tried, even though only the 64-byte view
// region was ever intentionally touched.
void RefreshKnownViewBufferIfCurrent(ID3D11Resource* resource, const void* data, UINT byteWidth)
{
    if (byteWidth != kCombinedViewProjByteWidth)
        return;

    std::lock_guard<std::mutex> lock(g_viewMutex);
    if (!g_viewValid)
        return; // not identified yet -- UpdateKnownViewBuffer does the first capture

    ID3D11Buffer* buffer = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || !buffer)
        return;
    bool isOurBuffer = (buffer == g_viewBuffer);
    buffer->Release(); // this QueryInterface's own ref; g_viewBuffer already holds the one that matters
    if (isOurBuffer)
        memcpy(g_viewFullBytes, data, kCombinedViewProjByteWidth);
}

void ScanMappedBufferForProjection(ID3D11Resource* resource, void* data, UINT byteWidth, void* mapReturnAddress)
{
    float target = ExpectedProjM11();
    if (target == 0.0f)
        return;

    size_t floatCount = byteWidth / sizeof(float);
    const float* floats = reinterpret_cast<const float*>(data);
    bool matched = false;
    size_t matchIndex = 0;
    for (size_t i = 0; i < floatCount; ++i)
    {
        if (fabsf(floats[i] - target) < 0.01f)
        {
            matched = true;
            matchIndex = i;
            break;
        }
    }
    if (!matched)
        return;

    {
        std::lock_guard<std::mutex> lock(g_knownMutex);
        g_knownProjectionBuffers.insert(resource);
    }
    UpdateKnownProjectionBuffer(resource, data, byteWidth);
    UpdateKnownViewBuffer(resource, data, byteWidth,
                            mapReturnAddress); // no-ops unless byteWidth is the combined buffer's size

    if (g_totalMatchesLogged.load() >= kMaxMatchesLogged)
        return;
    unsigned long long now = GetTickCount64();
    if (now < g_nextDumpAllowedMs.load())
        return;
    g_nextDumpAllowedMs.store(now + 2000); // spread dumps out over time to see values change with camera movement

    int n = ++g_totalMatchesLogged;

    std::string dump;
    char buf[32];
    for (size_t i = 0; i < floatCount; ++i)
    {
        snprintf(buf, sizeof(buf), "%.4f ", floats[i]);
        dump += buf;
        if (i % 4 == 3)
            dump += "| ";
    }

    MOHW_LOG(kLogFile,
              "dump #%d: resource=%p byteWidth=%u matchFloatIndex=%zu (expected proj[1][1]=%.6f) "
              "all floats (grouped by float4): %s",
              n, static_cast<void*>(resource), byteWidth, matchIndex, target, dump.c_str());
}

HRESULT __stdcall Hooked_Map(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource,
                              D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    // Must be captured directly in this function, not a helper -- it
    // returns the address that called THIS stack frame, i.e. the game's
    // own code that invoked Map through the patched vtable slot.
    void* callerAddress = _ReturnAddress();
    HRESULT hr = g_originalMap(self, resource, subresource, mapType, mapFlags, mapped);
    if (FAILED(hr) || !mapped || !mapped->pData)
        return hr;

    if (mapType != D3D11_MAP_WRITE_DISCARD && mapType != D3D11_MAP_WRITE_NO_OVERWRITE && mapType != D3D11_MAP_WRITE)
        return hr;

    ID3D11Buffer* buffer = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || !buffer)
        return hr;

    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    buffer->Release();

    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
        return hr;

    std::lock_guard<std::mutex> lock(g_mappedMutex);
    g_mapped[resource] = MappedCbInfo{mapped->pData, desc.ByteWidth, callerAddress};
    return hr;
}

void ApplyBoneHideIfEnabled(const void* srcData, UINT byteWidth); // defined below, used by Hooked_Unmap here and
                                                                    // Hooked_UpdateSubresource further down

void __stdcall Hooked_Unmap(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource)
{
    MappedCbInfo info{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mappedMutex);
        auto it = g_mapped.find(resource);
        if (it != g_mapped.end())
        {
            info = it->second;
            found = true;
            g_mapped.erase(it);
        }
    }

    if (found)
    {
        ScanMappedBufferForProjection(resource, info.data, info.byteWidth, info.mapReturnAddress);
        RefreshKnownViewBufferIfCurrent(resource, info.data, info.byteWidth);
        // Cache the ORIGINAL bytes first (same ordering as the
        // UpdateSubresource path), THEN patch -- BUG FIX (2026-08-25): this
        // was the only place UpdateKnownPlayerBoneBuffer got called without
        // a matching ApplyBoneHideIfEnabled call, so if the player bone
        // buffer's real per-frame writes go through Map/Unmap rather than
        // UpdateSubresource, the identity lock worked (explaining why it
        // kept re-confirming) but the hide patch never actually applied --
        // full "no difference" explained. Unlike UpdateSubresource's
        // const pSrcData, info.data here IS the live writable mapped
        // pointer -- patch it in place before Unmap commits it, no
        // alternate-buffer redirection needed.
        UpdateKnownPlayerBoneBuffer(resource, info.data, info.byteWidth, info.mapReturnAddress);
        ApplyBoneHideIfEnabled(info.data, info.byteWidth);
    }

    g_originalUnmap(self, resource, subresource);
}

// Unlike Map/Unmap, this hands us the CPU source data directly as a
// parameter (pSrcData), no Map-then-Unmap round trip or g_mapped tracking
// needed -- just check the destination buffer's size and, if it matches,
// feed pSrcData straight to the same identification function Unmap uses.
// Kept installed permanently, not just for discovery: the earlier
// diagnostic pass confirmed the player bone buffer is written via THIS
// API, not Map/Unmap (an exact-size Map/Unmap-only search came up
// completely empty), so this hook is load-bearing for
// GetKnownPlayerBoneBuffer() to ever populate at all, same as Map/Unmap
// already are for the view/projection buffers. Real per-call overhead
// (QueryInterface+GetDesc on every UpdateSubresource call, far more
// frequent than Map/Unmap since it also covers textures/vertex/index
// buffers) -- same category of cost this project already accepts for the
// Map hook, just paid more often here.
// TEMPORARY DIAGNOSTIC logging (throttled, 1/sec) confirming this fires
// rather than silently failing the entity/distance check every call.
std::atomic<unsigned long long> g_nextBoneHideLogAllowedMs{0};

// Draw-call-skip hide (2026-08-26), REPLACING bone-matrix corruption
// (zeroing and height-offset were both tried -- see git history/memory for
// the old version of this function). Both patching techniques only affect
// what the VERTEX SHADER computes for that bone; they can't stop the
// engine's own occlusion-culling/shadow/G-buffer submission systems from
// still expecting the mesh to be there, live-confirmed as a "stale
// rendering that follows the camera" artifact independent of alternating-
// eye stereo AND antialiasing, and independent of which patching technique
// was used. A live x32dbg search for a real engine-level per-mesh
// visibility flag (5+ call-chain levels: DrawIndexed -> 0076A4D0 ->
// 00775770 -> wrapper returns -> 0101457D/010130C0's giant dispatcher)
// found no such flag -- the most promising lead (a per-object field at
// esi+0x30) turned out to be either a shared debug-marker string constant
// or a pointer into nvwgf2um.dll (the NVIDIA driver), not real per-object
// identity.
//
// Skipping the DrawIndexed call itself (see Hooked_DrawIndexed) sidesteps
// the whole problem: no geometry is ever submitted to the GPU for a
// skipped draw, in whatever pass it belongs to, so there's nothing left
// over for any pass to render incorrectly. This function's only remaining
// job is identifying WHICH draw calls belong to the locked entity, via
// g_nextDrawIsLockedEntity -- it no longer touches the bone buffer's
// contents at all (data is passed through to the GPU completely
// untouched).
void ApplyBoneHideIfEnabled(const void* srcData, UINT byteWidth)
{
    if (byteWidth != kPlayerBoneBufferByteWidth || !GetBoneHideEnabled())
        return;

    unsigned long long now = GetTickCount64();
    unsigned long long allowed = g_nextBoneHideLogAllowedMs.load(std::memory_order_relaxed);
    bool dueToLog = now >= allowed &&
                     g_nextBoneHideLogAllowedMs.compare_exchange_strong(allowed, now + 1000, std::memory_order_relaxed);

    // Live finding (2026-08-25): buffer identity is a total no-op
    // discriminator for this buffer -- confirmed via live x32dbg tracing
    // that it's a single object rewritten in place immediately before EVERY
    // skinned draw call's own render (player, each visible NPC, props,
    // weapon), not a dedicated per-entity allocation. Distance alone is
    // also insufficient (an adjacent NPC's own true distance can fall
    // inside the player's own confirmed range). The real fix: gate on
    // entityId (g_lastEntityPoseArg1, captured by the
    // OFFSET_FUNCTIONCONSTANTBUFFERUPDATE_CALLER_ENTITY hook -- see
    // mohw_offsets.h), a genuine per-entity argument confirmed live to stay
    // stable across many consecutive frames for the same entity and to vary
    // between different ones, unlike buffer identity or distance. Once a
    // manual capture (End hotkey) has locked g_lockedEntityId, gate on THAT
    // alone -- distance/buffer-identity are dropped entirely post-capture
    // since they're proven unreliable. Before any capture, fall back to the
    // old distance-only behavior purely for diagnostic logging (no hide
    // effect pre-capture -- draw-skip is drastic enough that it should only
    // ever apply to a deliberately, manually locked entity).
    bool manuallyCaptured = g_playerBoneManuallyCaptured.load(std::memory_order_relaxed);
    if (manuallyCaptured)
    {
        void* entityId = g_lastEntityPoseArg1.load(std::memory_order_relaxed);
        void* lockedEntityId = g_lockedEntityId.load(std::memory_order_relaxed);
        if (entityId != lockedEntityId)
        {
            // BUG FIX (2026-08-26), live-diagnosed: draw-skip only hid ONE
            // draw call per bone-buffer write, but a live test showed the
            // hide still left a stale/occluding partial mesh visible --
            // RENDER ITEM CORRELATION logging earlier this session already
            // proved a single write is followed by MANY draw calls (one
            // per sub-mesh/material batch), not one. The old one-shot
            // .exchange(false,...) in Hooked_DrawIndexed consumed the flag
            // on the FIRST of those draws, leaving every subsequent one for
            // the same object rendering normally. Fix: this write's job is
            // now to explicitly END the "currently drawing the locked
            // entity" window (a different entity's write means our
            // entity's draw burst, if any, is over) rather than merely not
            // starting one -- paired with Hooked_DrawIndexed now PEEKING
            // (not consuming) the flag so it stays true across every draw
            // until a non-matching write like this one turns it back off.
            g_nextDrawIsLockedEntity.store(false, std::memory_order_relaxed);
            if (dueToLog)
                MOHW_LOG(kLogFile, "bone hide: SKIPPED this write, entityId=%p is not the locked entity (%p)",
                          entityId, lockedEntityId);
            return; // not the captured entity -- leave NPCs/weapon/world untouched
        }
        // This write is confirmed for our locked entity -- ALL DrawIndexed
        // calls from here until the next write for a DIFFERENT entity
        // (above) should be this entity's own draws (write-then-draw-burst,
        // confirmed live via RENDER ITEM CORRELATION: many draws share one
        // write). Hooked_DrawIndexed peeks this flag (doesn't consume it)
        // and skips every real draw while it's true, which is now the
        // whole hide mechanism.
        g_nextDrawIsLockedEntity.store(true, std::memory_order_relaxed);
        if (dueToLog)
            MOHW_LOG(kLogFile, "bone hide: entity=%p matches locked entity -- next DrawIndexed(s) will be SKIPPED",
                      entityId);
    }
    else
    {
        float boneHideThreshold = GetPlayerBoneDistanceThreshold();
        float distance = DistanceBone0ToKnownCamera(reinterpret_cast<const unsigned char*>(srcData));
        if (dueToLog)
            MOHW_LOG(kLogFile, "bone hide: SKIPPED this write, distance=%.3f (threshold %.2f) -- no manual "
                                "capture yet, press End near the player only",
                      distance, boneHideThreshold);
    }
}

void __stdcall Hooked_UpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* pDstResource, UINT dstSubresource,
                                          const D3D11_BOX* pDstBox, const void* pSrcData, UINT srcRowPitch,
                                          UINT srcDepthPitch)
{
    void* callerAddress = _ReturnAddress();

    if (pSrcData && pDstResource)
    {
        ID3D11Buffer* buffer = nullptr;
        if (SUCCEEDED(pDstResource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) &&
            buffer)
        {
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            buffer->Release();
            if (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)
            {
                // Scanning must use the ACTUAL valid extent of pSrcData, not
                // the full buffer size: when pDstBox is non-null (a partial
                // update), pSrcData only holds (right-left) bytes, per
                // D3D11's own docs for buffer resources (D3D11_BOX::left/
                // right are byte offsets for buffers). Treating it as the
                // full desc.ByteWidth here would read past pSrcData's real
                // extent -- an actual out-of-bounds read, not just a missed
                // detection. Only when pDstBox is null does the update
                // legitimately cover the whole resource. A partial update
                // smaller than kPlayerBoneBufferByteWidth is naturally
                // skipped by UpdateKnownPlayerBoneBuffer's own size check --
                // only a write of the FULL 4096 bytes is cached.
                UINT effectiveByteWidth = desc.ByteWidth;
                if (pDstBox)
                    effectiveByteWidth = pDstBox->right - pDstBox->left;

                // Cache the ORIGINAL (unpatched) bytes -- the cache should
                // reflect the game's own real data, not our test hide.
                UpdateKnownPlayerBoneBuffer(pDstResource, pSrcData, effectiveByteWidth, callerAddress);
                ApplyBoneHideIfEnabled(pSrcData, effectiveByteWidth);
            }
        }
    }

    g_originalUpdateSubresource(self, pDstResource, dstSubresource, pDstBox, pSrcData, srcRowPitch, srcDepthPitch);
}

// Per LOCKED ENTITY, not global (2026-08-26 fix, live-diagnosed): a
// single lock's writes produce many draws in one burst (one bone-buffer
// write is shared across every sub-mesh/material draw of that model, not
// just one), which exhausted a global cap on the very first entity
// tested, before ever reaching a later one (e.g. cycling from weapon to
// hands) -- see g_poseTableEntityState's own history for the same class
// of bug. Keyed by g_lockedEntityId itself (the flag is only ever set
// when entityId==locked, so whatever's locked AT LOG TIME is correct).
std::mutex g_drawTraceLogCountsMutex;
std::unordered_map<void*, int> g_drawTraceLogCounts;
constexpr int kMaxDrawTraceLogsPerEntity = 2000; // was 30 -- exhausted within seconds of a single long-lived lock (confirmed live: same entityId held locked for close to an hour straight), well before a live x32dbg correlation session could use it

// Vertex/index-buffer identity (2026-08-26) -- BUG FIX, live-diagnosed: the
// write-recency flag above (g_nextDrawIsLockedEntity) only knows "the
// shared bone CB was most recently written for this entity" -- true only
// for whichever draw(s) IMMEDIATELY follow a matching write. A live test
// showed draw-call-skip still left a stale/occluding remnant, identical to
// the old bone-corruption technique's own artifact, on a totally different
// mechanism -- strongly suggesting this entity's mesh gets drawn across
// MORE THAN ONE pass per frame (e.g. a depth pre-pass, then later a color
// pass), with OTHER entities' bone writes interleaving in between passes.
// Since the flag only covers the burst immediately after a fresh write, a
// later pass reusing already-uploaded data (no fresh write of its own,
// because something else wrote in between) would never get flagged.
//
// Fix: this entity's mesh has its own STABLE vertex/index buffer objects,
// confirmed different from every other entity's (see the DRAW TRACE log
// history this session) -- unlike the shared, constantly-rewritten bone
// CB, buffer identity doesn't depend on write timing at all. Learn the
// (vertexBuffer, indexBuffer) pairs actually used by the locked entity via
// the existing write-recency flag (reliable for whichever draw happens
// right after a confirmed-fresh write), then match on that pair identity
// for EVERY subsequent draw regardless of write timing -- covers every
// pass, every frame, independent of how often this entity's own bone data
// actually gets re-uploaded.
struct BufferPairHash
{
    size_t operator()(const std::pair<void*, void*>& p) const
    {
        return std::hash<void*>()(p.first) ^ (std::hash<void*>()(p.second) << 1);
    }
};
std::mutex g_knownEntityBuffersMutex;
std::unordered_set<std::pair<void*, void*>, BufferPairHash> g_knownEntityBufferPairs;

// Called whenever the lock moves to a DIFFERENT entity (End capture or
// Numpad0 cycle) -- stale pairs from a previously-locked entity must not
// keep suppressing draws for whatever gets locked next.
void ClearKnownEntityBuffers()
{
    std::lock_guard<std::mutex> lock(g_knownEntityBuffersMutex);
    g_knownEntityBufferPairs.clear();
}

bool IsKnownEntityBufferPair(void* vertexBuffer, void* indexBuffer)
{
    std::lock_guard<std::mutex> lock(g_knownEntityBuffersMutex);
    return g_knownEntityBufferPairs.count({vertexBuffer, indexBuffer}) != 0;
}

void RememberEntityBufferPair(void* vertexBuffer, void* indexBuffer)
{
    std::lock_guard<std::mutex> lock(g_knownEntityBuffersMutex);
    bool isNew = g_knownEntityBufferPairs.insert({vertexBuffer, indexBuffer}).second;
    if (isNew)
        MOHW_LOG(kLogFile,
                  "draw-call-skip: learned new vertexBuffer/indexBuffer pair for locked entity: vb=%p ib=%p "
                  "(total known pairs=%zu)",
                  vertexBuffer, indexBuffer, g_knownEntityBufferPairs.size());
}

// Draw-call-skip hide (2026-08-26) -- see ApplyBoneHideIfEnabled's comment
// for why this replaced bone-matrix corruption, and BufferPairHash's
// comment above for why the skip decision is buffer-identity-based, not
// purely write-recency-based. g_nextDrawIsLockedEntity is PEEKED (not
// consumed) so it stays true for the locked entity's whole draw burst, and
// is used here only to BOOTSTRAP which buffer pairs belong to that entity
// -- the actual skip covers every draw matching an already-learned pair,
// even ones with no fresh write immediately before them.
void __stdcall Hooked_DrawIndexed(ID3D11DeviceContext* self, UINT IndexCount, UINT StartIndexLocation,
                                    INT BaseVertexLocation)
{
    ID3D11Buffer* vertexBuffer = nullptr;
    UINT stride = 0, vbOffset = 0;
    self->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &vbOffset);
    ID3D11Buffer* indexBuffer = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    self->IAGetIndexBuffer(&indexBuffer, &indexFormat, &ibOffset);
    // IAGetVertexBuffers/IAGetIndexBuffer both AddRef -- release immediately,
    // only the raw pointer VALUE is used below (identity comparison, never
    // dereferenced), and the underlying objects stay alive regardless (bound
    // to the pipeline / owned by the game), same reasoning already applied
    // to g_lockedEntityId itself treating a raw address as stable identity
    // for the whole session.
    if (vertexBuffer)
        vertexBuffer->Release();
    if (indexBuffer)
        indexBuffer->Release();

    bool isLockedEntityDraw = g_nextDrawIsLockedEntity.load(std::memory_order_relaxed);
    bool knownBufferMatch = IsKnownEntityBufferPair(vertexBuffer, indexBuffer);
    if (isLockedEntityDraw && !knownBufferMatch)
    {
        RememberEntityBufferPair(vertexBuffer, indexBuffer);
        knownBufferMatch = true;
    }

    if (knownBufferMatch)
    {
        void* lockedEntityId = g_lockedEntityId.load(std::memory_order_relaxed);
        int thisEntityCount;
        {
            std::lock_guard<std::mutex> lock(g_drawTraceLogCountsMutex);
            thisEntityCount = ++g_drawTraceLogCounts[lockedEntityId];
        }
        if (thisEntityCount <= kMaxDrawTraceLogsPerEntity)
        {
            MOHW_LOG(kLogFile,
                      "DRAW SKIPPED #%d for entity=%p (draw-call-skip hide, buffer-match): vb=%p ib=%p "
                      "IndexCount=%u StartIndexLocation=%u BaseVertexLocation=%d",
                      thisEntityCount, lockedEntityId, static_cast<void*>(vertexBuffer),
                      static_cast<void*>(indexBuffer), IndexCount, StartIndexLocation, BaseVertexLocation);
        }
        return; // the hide itself -- never forward to the real draw call
    }

    g_originalDrawIndexed(self, IndexCount, StartIndexLocation, BaseVertexLocation);
    // Called AFTER forwarding (not before, as this used to) -- draw_trace_diag.cpp's UI-snapshot capture issues a
    // CopyResource from here when this is the Nth draw under a backbuffer bind, which must be recorded on the command
    // list AFTER this draw's own commands for correct GPU-side ordering. Also naturally skips the bone-hide early
    // return above (the real draw never happened there, so nothing to count).
    DrawTraceNoteDraw(self, IndexCount); // diagnostic frame trace + UI snapshot (hooks/draw_trace_diag.h) share this hook -- MinHook cannot hook one target twice
}

// Separate budget per stage string so one stage's early, frequent binding
// (e.g. classic "VS") can't starve out log visibility into a different,
// less-frequent stage (e.g. "VS1") sharing one global counter -- exactly
// what happened on the first test of the VS1 hook.
std::atomic<int>& BindingCounterFor(const char* stage)
{
    static std::atomic<int> counters[4]{};
    static std::mutex namesMutex;
    static const char* names[4] = {nullptr, nullptr, nullptr, nullptr};
    std::lock_guard<std::mutex> lock(namesMutex);
    for (int i = 0; i < 4; ++i)
    {
        if (names[i] == nullptr)
        {
            names[i] = stage;
            return counters[i];
        }
        if (strcmp(names[i], stage) == 0)
            return counters[i];
    }
    return counters[3]; // fallback, shouldn't happen with only 3 stages in use
}

void LogBindingIfKnown(const char* stage, UINT startSlot, UINT numBuffers, ID3D11Buffer* const* buffers)
{
    std::atomic<int>& counter = BindingCounterFor(stage);
    if (counter.load() >= kMaxBindingsLogged)
        return;
    for (UINT i = 0; i < numBuffers; ++i)
    {
        ID3D11Buffer* b = buffers[i];
        if (!b)
            continue;
        bool known;
        {
            std::lock_guard<std::mutex> lock(g_knownMutex);
            known = g_knownProjectionBuffers.count(b) != 0;
        }
        if (known)
        {
            int n = ++counter;
            MOHW_LOG(kLogFile, "binding #%d: known projection buffer=%p bound to %s slot %u", n,
                      static_cast<void*>(b), stage, startSlot + i);
            if (n >= kMaxBindingsLogged)
                return;
        }
    }
}

// Tracks whichever buffer is currently bound at EVERY VS constant-buffer
// slot (D3D11 has 14 per stage) -- not just slot 0. First pass only tracked
// slot 0 (assuming the camera CB stays there for every draw), but a live
// test showed 30,000 real draws all skipped despite the buffer being
// confirmed bound to slot 0 at some point -- suggesting slot 0 gets
// rebound to something else (e.g. per-object data) for actual geometry
// draws, with the camera data possibly living at a different slot by then.
// Tracking all slots lets draw_duplication_hook.cpp find wherever the known
// buffer actually is, or log what IS at slot 0 if not found anywhere.
// Immediate context is only ever used from one thread by D3D11's own rules,
// so plain (non-atomic-per-element) storage guarded by nothing extra is
// fine here, same reasoning as the single-slot version this replaces.
constexpr UINT kVsSlotCount = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; // 14
void* g_currentVsSlots[kVsSlotCount] = {};

void __stdcall Hooked_VSSetConstantBuffers(ID3D11DeviceContext* self, UINT startSlot, UINT numBuffers,
                                             ID3D11Buffer* const* buffers)
{
    LogBindingIfKnown("VS", startSlot, numBuffers, buffers);
    for (UINT i = 0; i < numBuffers; ++i)
    {
        UINT slot = startSlot + i;
        if (slot < kVsSlotCount)
            g_currentVsSlots[slot] = buffers[i];
    }
    g_originalVSSetConstantBuffers(self, startSlot, numBuffers, buffers);
}

void __stdcall Hooked_PSSetConstantBuffers(ID3D11DeviceContext* self, UINT startSlot, UINT numBuffers,
                                             ID3D11Buffer* const* buffers)
{
    LogBindingIfKnown("PS", startSlot, numBuffers, buffers);
    g_originalPSSetConstantBuffers(self, startSlot, numBuffers, buffers);
}

void __stdcall Hooked_VSSetConstantBuffers1(ID3D11DeviceContext1* self, UINT startSlot, UINT numBuffers,
                                              ID3D11Buffer* const* buffers, const UINT* firstConstant,
                                              const UINT* numConstants)
{
    LogBindingIfKnown("VS1", startSlot, numBuffers, buffers);
    for (UINT i = 0; i < numBuffers; ++i)
    {
        UINT slot = startSlot + i;
        if (slot < kVsSlotCount)
            g_currentVsSlots[slot] = buffers[i];
    }
    g_originalVSSetConstantBuffers1(self, startSlot, numBuffers, buffers, firstConstant, numConstants);
}

bool GetContextVtableSlots(void** outMap, void** outUnmap, void** outVsSet, void** outPsSet, void** outVsSet1,
                             void** outUpdateSubresource, void** outDrawIndexed)
{
    *outVsSet1 = nullptr; // stays null if this runtime doesn't support ID3D11DeviceContext1
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kDummyWindowClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kDummyWindowClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 2, 2, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    if (!hwnd)
    {
        MOHW_LOG(kLogFile, "GetContextVtableSlots: dummy window creation FAILED (err=%lu)", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 2;
    desc.BufferDesc.Height = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context);
    if (FAILED(hr) || !context)
    {
        MOHW_LOG(kLogFile, "GetContextVtableSlots: D3D11CreateDeviceAndSwapChain FAILED hr=0x%08lX", hr);
        DestroyWindow(hwnd);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(context);
    *outMap = vtable[14];    // ID3D11DeviceContext::Map
    *outUnmap = vtable[15];  // ID3D11DeviceContext::Unmap
    *outVsSet = vtable[7];   // ID3D11DeviceContext::VSSetConstantBuffers
    *outPsSet = vtable[16];  // ID3D11DeviceContext::PSSetConstantBuffers
    *outUpdateSubresource = vtable[48]; // ID3D11DeviceContext::UpdateSubresource, standard documented vtable order
    *outDrawIndexed = vtable[12]; // ID3D11DeviceContext::DrawIndexed -- same standard order already confirmed
                                    // correct for every other slot above

    ID3D11DeviceContext1* context1 = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1))) &&
        context1)
    {
        void** vtable1 = *reinterpret_cast<void***>(context1);
        *outVsSet1 = vtable1[118]; // ID3D11DeviceContext1::VSSetConstantBuffers1
        context1->Release();
        MOHW_LOG(kLogFile, "GetContextVtableSlots: ID3D11DeviceContext1 supported, VSSetConstantBuffers1 @ %p",
                  *outVsSet1);
    }
    else
    {
        MOHW_LOG(kLogFile, "GetContextVtableSlots: ID3D11DeviceContext1 NOT supported on this runtime");
    }

    swapChain->Release();
    context->Release();
    device->Release();
    DestroyWindow(hwnd);
    return true;
}

void InstallThreadProc()
{
    void* mapAddr = nullptr;
    void* unmapAddr = nullptr;
    void* vsSetAddr = nullptr;
    void* psSetAddr = nullptr;
    void* vsSet1Addr = nullptr;
    void* updateSubresourceAddr = nullptr;
    void* drawIndexedAddr = nullptr;
    if (!GetContextVtableSlots(&mapAddr, &unmapAddr, &vsSetAddr, &psSetAddr, &vsSet1Addr, &updateSubresourceAddr,
                                 &drawIndexedAddr))
        return;

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return;
    }

    g_mapAddress = mapAddr;
    g_unmapAddress = unmapAddr;
    g_vsSetAddress = vsSetAddr;
    g_psSetAddress = psSetAddr;
    g_vsSet1Address = vsSet1Addr;
    g_updateSubresourceAddress = updateSubresourceAddr;
    g_drawIndexedAddress = drawIndexedAddr;

    struct HookSpec
    {
        void* address;
        void* detour;
        void** original;
        const char* name;
    };
    HookSpec specs[] = {
        {g_mapAddress, reinterpret_cast<void*>(&Hooked_Map), reinterpret_cast<void**>(&g_originalMap), "Map"},
        {g_unmapAddress, reinterpret_cast<void*>(&Hooked_Unmap), reinterpret_cast<void**>(&g_originalUnmap),
         "Unmap"},
        {g_vsSetAddress, reinterpret_cast<void*>(&Hooked_VSSetConstantBuffers),
         reinterpret_cast<void**>(&g_originalVSSetConstantBuffers), "VSSetConstantBuffers"},
        {g_psSetAddress, reinterpret_cast<void*>(&Hooked_PSSetConstantBuffers),
         reinterpret_cast<void**>(&g_originalPSSetConstantBuffers), "PSSetConstantBuffers"},
        {g_updateSubresourceAddress, reinterpret_cast<void*>(&Hooked_UpdateSubresource),
         reinterpret_cast<void**>(&g_originalUpdateSubresource), "UpdateSubresource"},
        {g_drawIndexedAddress, reinterpret_cast<void*>(&Hooked_DrawIndexed),
         reinterpret_cast<void**>(&g_originalDrawIndexed), "DrawIndexed"},
    };

    for (auto& spec : specs)
    {
        MH_STATUS s = MH_CreateHook(spec.address, spec.detour, spec.original);
        if (s != MH_OK)
        {
            MOHW_LOG(kLogFile, "MH_CreateHook(%s @ %p) FAILED: %s", spec.name, spec.address, MH_StatusToString(s));
            return;
        }
        s = MH_EnableHook(spec.address);
        if (s != MH_OK)
        {
            MOHW_LOG(kLogFile, "MH_EnableHook(%s) FAILED: %s", spec.name, MH_StatusToString(s));
            return;
        }
    }

    MOHW_LOG(kLogFile,
              "hooks installed: Map=%p Unmap=%p VSSetConstantBuffers=%p PSSetConstantBuffers=%p "
              "UpdateSubresource=%p DrawIndexed=%p",
              g_mapAddress, g_unmapAddress, g_vsSetAddress, g_psSetAddress, g_updateSubresourceAddress,
              g_drawIndexedAddress);

    if (g_vsSet1Address)
    {
        MH_STATUS s = MH_CreateHook(g_vsSet1Address, reinterpret_cast<void*>(&Hooked_VSSetConstantBuffers1),
                                     reinterpret_cast<void**>(&g_originalVSSetConstantBuffers1));
        if (s != MH_OK)
        {
            MOHW_LOG(kLogFile, "MH_CreateHook(VSSetConstantBuffers1 @ %p) FAILED: %s", g_vsSet1Address,
                      MH_StatusToString(s));
            g_vsSet1Address = nullptr;
            return;
        }
        s = MH_EnableHook(g_vsSet1Address);
        if (s != MH_OK)
        {
            MOHW_LOG(kLogFile, "MH_EnableHook(VSSetConstantBuffers1) FAILED: %s", MH_StatusToString(s));
            g_vsSet1Address = nullptr;
            return;
        }
        MOHW_LOG(kLogFile, "VSSetConstantBuffers1 hook installed @ %p", g_vsSet1Address);
    }
}

} // namespace

bool InstallConstantBufferHook()
{
    // Installed synchronously (unlike the D3D11 vtable hooks below, this
    // needs no dummy device -- it's a raw internal function address,
    // resolvable as soon as the module is) so entity-id data is available
    // from the very first frame.
    //
    // TEMPORARILY DISABLED (2026-09-17): this hook's own MH_CreateHook JMP
    // is what was showing up at 00771670 in every static Ghidra dump we
    // took (both the process-dump program and, after checking, corrupted-
    // packed bytes in the original MOHW.exe) -- with the hook installed we
    // can never see FUN_00771670's real original bytes/callees to find the
    // real per-bone static hook point. Disabled so a live x32dbg session
    // can read the genuine unhooked function. Re-enable once that's done --
    // this is also what drives entity-id bone-hide gating (falls back to
    // distance-only without it) and the pose-table dumps.
    if (false && !InstallBoneEntityIdHook())
        MOHW_LOG(kLogFile, "InstallBoneEntityIdHook FAILED -- entity-id bone-hide gating will stay unavailable, "
                            "falls back to distance-only");
    if (!InstallDrawBatchDispatchHook())
        MOHW_LOG(kLogFile, "InstallDrawBatchDispatchHook FAILED -- render-item correlation logging unavailable");

    MOHW_LOG(kLogFile, "InstallConstantBufferHook: spawning background thread for dummy-context vtable grab");
    std::thread(&InstallThreadProc).detach();
    return true;
}

void RemoveConstantBufferHook()
{
    if (g_mapAddress)
    {
        MH_DisableHook(g_mapAddress);
        MH_RemoveHook(g_mapAddress);
        g_mapAddress = nullptr;
    }
    if (g_unmapAddress)
    {
        MH_DisableHook(g_unmapAddress);
        MH_RemoveHook(g_unmapAddress);
        g_unmapAddress = nullptr;
    }
    if (g_vsSetAddress)
    {
        MH_DisableHook(g_vsSetAddress);
        MH_RemoveHook(g_vsSetAddress);
        g_vsSetAddress = nullptr;
    }
    if (g_psSetAddress)
    {
        MH_DisableHook(g_psSetAddress);
        MH_RemoveHook(g_psSetAddress);
        g_psSetAddress = nullptr;
    }
    if (g_vsSet1Address)
    {
        MH_DisableHook(g_vsSet1Address);
        MH_RemoveHook(g_vsSet1Address);
        g_vsSet1Address = nullptr;
    }
    if (g_updateSubresourceAddress)
    {
        MH_DisableHook(g_updateSubresourceAddress);
        MH_RemoveHook(g_updateSubresourceAddress);
        g_updateSubresourceAddress = nullptr;
    }
    if (g_drawIndexedAddress)
    {
        MH_DisableHook(g_drawIndexedAddress);
        MH_RemoveHook(g_drawIndexedAddress);
        g_drawIndexedAddress = nullptr;
    }
    RemoveBoneEntityIdHook();
    RemoveDrawBatchDispatchHook();
    // MH_Uninitialize() is called once centrally from dllmain.cpp.
}

ID3D11Buffer* GetKnownProjectionBuffer()
{
    std::lock_guard<std::mutex> lock(g_projMutex);
    return g_projValid ? g_projBuffer : nullptr;
}

bool GetLastKnownProjectionBytes(void* outBytes80)
{
    std::lock_guard<std::mutex> lock(g_projMutex);
    if (!g_projValid)
        return false;
    memcpy(outBytes80, g_projBytes, 80);
    return true;
}

bool IsKnownProjectionBufferBoundToVS0()
{
    std::lock_guard<std::mutex> lock(g_projMutex);
    return g_projValid && g_currentVsSlots[0] == static_cast<void*>(g_projBuffer);
}

int FindKnownProjectionBufferVSSlot()
{
    std::lock_guard<std::mutex> lock(g_projMutex);
    if (!g_projValid)
        return -1;
    for (UINT slot = 0; slot < kVsSlotCount; ++slot)
    {
        if (g_currentVsSlots[slot] == static_cast<void*>(g_projBuffer))
            return static_cast<int>(slot);
    }
    return -1;
}

void* GetVsSlotBufferRaw(int slot)
{
    if (slot < 0 || static_cast<UINT>(slot) >= kVsSlotCount)
        return nullptr;
    return g_currentVsSlots[slot];
}

bool WriteRawBytesToProjectionBuffer(ID3D11DeviceContext* context, const void* bytes80)
{
    ID3D11Buffer* buffer;
    {
        std::lock_guard<std::mutex> lock(g_projMutex);
        if (!g_projValid)
            return false;
        buffer = g_projBuffer;
    }
    if (!g_originalMap || !g_originalUnmap)
        return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = g_originalMap(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData)
        return false;
    memcpy(mapped.pData, bytes80, 80);
    g_originalUnmap(context, buffer, 0);
    return true;
}

ID3D11Buffer* GetKnownViewBuffer()
{
    std::lock_guard<std::mutex> lock(g_viewMutex);
    return g_viewValid ? g_viewBuffer : nullptr;
}

bool GetLastKnownViewMatrixBytes(void* outBytes64)
{
    std::lock_guard<std::mutex> lock(g_viewMutex);
    if (!g_viewValid)
        return false;
    memcpy(outBytes64, g_viewFullBytes + kViewMatrixByteOffset, kViewMatrixByteSize);
    return true;
}

bool GetLastKnownViewBufferFullBytes(void* outBytes352)
{
    std::lock_guard<std::mutex> lock(g_viewMutex);
    if (!g_viewValid)
        return false;
    memcpy(outBytes352, g_viewFullBytes, kCombinedViewProjByteWidth);
    return true;
}

bool IsKnownViewBufferBoundToVS0()
{
    std::lock_guard<std::mutex> lock(g_viewMutex);
    return g_viewValid && g_currentVsSlots[0] == static_cast<void*>(g_viewBuffer);
}

int FindKnownViewBufferVSSlot()
{
    std::lock_guard<std::mutex> lock(g_viewMutex);
    if (!g_viewValid)
        return -1;
    for (UINT slot = 0; slot < kVsSlotCount; ++slot)
    {
        if (g_currentVsSlots[slot] == static_cast<void*>(g_viewBuffer))
            return static_cast<int>(slot);
    }
    return -1;
}

// Patches FIVE sub-regions within a copy of the buffer's last-known-good
// full 352 bytes, then writes the WHOLE buffer back via WRITE_DISCARD --
// can't just write any region in isolation without either destroying the
// rest of the buffer's content (also used by projection and other camera
// data in the same combined buffer) or relying on WRITE_NO_OVERWRITE,
// which isn't guaranteed valid for however this buffer was actually
// created. Same technique already proven for the projection buffer above.
//
// crViewProjBytes64/cameraPosBytes12 are the ones that actually matter --
// confirmed via disassembling the real shader bytecode (dumped through
// 3Dmigoto's hunting mode) that the vertex shader computes
// `worldPos - cameraPos` then multiplies by crViewProjMatrix, NOT the
// plain viewMatrix/viewProjMatrix fields this project spent a long
// investigation patching with zero visible effect (both confirmed
// [unused] by the compiler). viewBytes64/precombinedBytes64 are kept
// patched too, in sync, purely defensively in case some OTHER shader
// (not the one dumped) reads those fields instead -- costs nothing since
// the whole buffer gets rewritten regardless.
bool WriteRawBytesToViewMatrix(ID3D11DeviceContext* context, const void* viewBytes64,
                                const void* precombinedBytes64, const void* crViewProjBytes64,
                                const void* cameraPosBytes12)
{
    ID3D11Buffer* buffer;
    unsigned char fullBytes[kCombinedViewProjByteWidth];
    {
        std::lock_guard<std::mutex> lock(g_viewMutex);
        if (!g_viewValid)
            return false;
        buffer = g_viewBuffer;
        memcpy(fullBytes, g_viewFullBytes, kCombinedViewProjByteWidth);
    }
    memcpy(fullBytes + kViewMatrixByteOffset, viewBytes64, kViewMatrixByteSize);
    memcpy(fullBytes + kPrecombinedMatrixByteOffset, precombinedBytes64, kPrecombinedMatrixByteSize);
    memcpy(fullBytes + kCrViewProjMatrixByteOffset, crViewProjBytes64, kCrViewProjMatrixByteSize);
    memcpy(fullBytes + kCameraPosByteOffset, cameraPosBytes12, kCameraPosByteSize);

    if (!g_originalMap || !g_originalUnmap)
        return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = g_originalMap(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData)
        return false;
    memcpy(mapped.pData, fullBytes, kCombinedViewProjByteWidth);
    g_originalUnmap(context, buffer, 0);
    return true;
}

ID3D11Buffer* GetKnownPlayerBoneBuffer()
{
    std::lock_guard<std::mutex> lock(g_playerBoneMutex);
    return g_playerBoneValid ? g_playerBoneBuffer : nullptr;
}

bool GetLastKnownPlayerBoneBytes(void* outBytes4096)
{
    std::lock_guard<std::mutex> lock(g_playerBoneMutex);
    if (!g_playerBoneValid)
        return false;
    memcpy(outBytes4096, g_playerBoneBytes, kPlayerBoneBufferByteWidth);
    return true;
}

bool IsKnownPlayerBoneBufferBoundToVS0()
{
    std::lock_guard<std::mutex> lock(g_playerBoneMutex);
    return g_playerBoneValid && g_currentVsSlots[0] == static_cast<void*>(g_playerBoneBuffer);
}

int FindKnownPlayerBoneBufferVSSlot()
{
    std::lock_guard<std::mutex> lock(g_playerBoneMutex);
    if (!g_playerBoneValid)
        return -1;
    for (UINT slot = 0; slot < kVsSlotCount; ++slot)
    {
        if (g_currentVsSlots[slot] == static_cast<void*>(g_playerBoneBuffer))
            return static_cast<int>(slot);
    }
    return -1;
}

// Uses UpdateSubresource, NOT Map/Unmap, unlike every other write function
// in this file -- deliberately: this buffer is confirmed written via
// UpdateSubresource live (see UpdateKnownPlayerBoneBuffer's comment; an
// exact-size Map/Unmap-only search found zero hits), which strongly
// suggests D3D11_USAGE_DEFAULT rather than USAGE_DYNAMIC -- Map with
// D3D11_MAP_WRITE_DISCARD would simply fail on a DEFAULT-usage buffer.
// UpdateSubresource works on both usage types, so it's the safer choice
// here even though every other cached buffer above uses Map.
bool WriteRawBytesToPlayerBoneBuffer(ID3D11DeviceContext* context, const void* bytes4096)
{
    ID3D11Buffer* buffer;
    {
        std::lock_guard<std::mutex> lock(g_playerBoneMutex);
        if (!g_playerBoneValid)
            return false;
        buffer = g_playerBoneBuffer;
    }
    if (!g_originalUpdateSubresource)
        return false;

    g_originalUpdateSubresource(context, buffer, 0, nullptr, bytes4096, 0, 0);
    return true;
}

namespace {
// F1-F12/Insert/Home/PageUp/PageDown all already claimed elsewhere in this
// project -- Delete and the Numpad keys are unused and, for a first-person
// shooter, very unlikely to double as real gameplay binds.
constexpr int kBoneHideToggleHotkey = VK_DELETE;
constexpr int kBoneHideRangeStartDownHotkey = VK_SUBTRACT; // Numpad -
constexpr int kBoneHideRangeStartUpHotkey = VK_ADD;        // Numpad +
constexpr int kBoneHideRangeEndDownHotkey = VK_DIVIDE;      // Numpad /
constexpr int kBoneHideRangeEndUpHotkey = VK_MULTIPLY;      // Numpad *
constexpr int kPlayerBoneCaptureHotkey = VK_END;
constexpr int kPlayerBoneDistanceDownHotkey = VK_NUMPAD8;
constexpr int kPlayerBoneDistanceUpHotkey = VK_NUMPAD9;
constexpr float kPlayerBoneDistanceStep = 0.1f;
constexpr float kPlayerBoneDistanceMin = 0.1f;
constexpr int kEntityCandidateCycleHotkey = VK_NUMPAD0;
// Single-bone step mode (2026-08-26) -- sets BOTH range start and end to
// the same index, so exactly one bone hides at a time instead of a range.
// Reuses the existing range mechanism as-is (ApplyBoneHideIfEnabled
// already handles start==end correctly) -- just keeps the two in sync
// instead of needing two separate hotkey presses each step.
constexpr int kSingleBoneStepDownHotkey = VK_NUMPAD1;
constexpr int kSingleBoneStepUpHotkey = VK_NUMPAD2;
} // namespace

void CheckBoneHideHotkeys()
{
    static bool captureKeyWasDown = false;
    bool captureKeyDown = (GetAsyncKeyState(kPlayerBoneCaptureHotkey) & 0x8000) != 0;
    if (captureKeyDown && !captureKeyWasDown)
    {
        g_captureRequested.store(true, std::memory_order_relaxed);
        // Also clears the entity-candidate list (2026-08-25) -- a generous
        // distance threshold plus the fixed 8-slot cap meant unrelated
        // nearby geometry (confirmed live: multiple terrain chunks) could
        // fill every slot before the player's own body ever got recorded.
        // End is now the "start a fresh search" button: lower
        // PlayerBoneDistanceThreshold with Numpad8 first if terrain/props
        // keep winning the race, then press End to wipe the slate and
        // Numpad0 to cycle through whatever qualifies this time.
        int clearedCount;
        {
            std::lock_guard<std::mutex> lock(g_entityCandidatesMutex);
            clearedCount = g_entityCandidateCount;
            g_entityCandidateCount = 0;
            for (auto& candidate : g_entityCandidates)
                candidate = nullptr;
        }
        g_entityCandidateCycleIndex.store(-1, std::memory_order_relaxed);
        MOHW_LOG(kLogFile,
                  "End pressed -- capturing player bone buffer on the next qualifying write (locks permanently "
                  "until the next End press); cleared %d previous entity candidate(s)",
                  clearedCount);
    }
    captureKeyWasDown = captureKeyDown;

    static bool toggleKeyWasDown = false;
    bool toggleKeyDown = (GetAsyncKeyState(kBoneHideToggleHotkey) & 0x8000) != 0;
    if (toggleKeyDown && !toggleKeyWasDown)
    {
        bool newValue = !GetBoneHideEnabled();
        SetBoneHideEnabled(newValue);
        MOHW_LOG(kLogFile, "Delete pressed -- bone hide now %s (range %.0f-%.0f, saved to mohwvr_settings.ini)",
                  newValue ? "ENABLED" : "disabled", GetBoneHideRangeStart(), GetBoneHideRangeEnd());
    }
    toggleKeyWasDown = toggleKeyDown;

    static bool startDownKeyWasDown = false;
    bool startDownKeyDown = (GetAsyncKeyState(kBoneHideRangeStartDownHotkey) & 0x8000) != 0;
    if (startDownKeyDown && !startDownKeyWasDown)
    {
        float newValue = GetBoneHideRangeStart() - 1.0f;
        SetBoneHideRangeStart(newValue);
        MOHW_LOG(kLogFile, "Numpad- pressed -- bone hide range start now %.0f (saved to mohwvr_settings.ini)",
                  newValue);
    }
    startDownKeyWasDown = startDownKeyDown;

    static bool startUpKeyWasDown = false;
    bool startUpKeyDown = (GetAsyncKeyState(kBoneHideRangeStartUpHotkey) & 0x8000) != 0;
    if (startUpKeyDown && !startUpKeyWasDown)
    {
        float newValue = GetBoneHideRangeStart() + 1.0f;
        SetBoneHideRangeStart(newValue);
        MOHW_LOG(kLogFile, "Numpad+ pressed -- bone hide range start now %.0f (saved to mohwvr_settings.ini)",
                  newValue);
    }
    startUpKeyWasDown = startUpKeyDown;

    static bool endDownKeyWasDown = false;
    bool endDownKeyDown = (GetAsyncKeyState(kBoneHideRangeEndDownHotkey) & 0x8000) != 0;
    if (endDownKeyDown && !endDownKeyWasDown)
    {
        float newValue = GetBoneHideRangeEnd() - 1.0f;
        SetBoneHideRangeEnd(newValue);
        MOHW_LOG(kLogFile, "Numpad/ pressed -- bone hide range end now %.0f (saved to mohwvr_settings.ini)",
                  newValue);
    }
    endDownKeyWasDown = endDownKeyDown;

    static bool endUpKeyWasDown = false;
    bool endUpKeyDown = (GetAsyncKeyState(kBoneHideRangeEndUpHotkey) & 0x8000) != 0;
    if (endUpKeyDown && !endUpKeyWasDown)
    {
        float newValue = GetBoneHideRangeEnd() + 1.0f;
        SetBoneHideRangeEnd(newValue);
        MOHW_LOG(kLogFile, "Numpad* pressed -- bone hide range end now %.0f (saved to mohwvr_settings.ini)",
                  newValue);
    }
    endUpKeyWasDown = endUpKeyDown;

    static bool distanceDownKeyWasDown = false;
    bool distanceDownKeyDown = (GetAsyncKeyState(kPlayerBoneDistanceDownHotkey) & 0x8000) != 0;
    if (distanceDownKeyDown && !distanceDownKeyWasDown)
    {
        float newValue = GetPlayerBoneDistanceThreshold() - kPlayerBoneDistanceStep;
        if (newValue < kPlayerBoneDistanceMin)
            newValue = kPlayerBoneDistanceMin;
        SetPlayerBoneDistanceThreshold(newValue);
        MOHW_LOG(kLogFile, "Numpad8 pressed -- player bone distance threshold now %.2f (saved to mohwvr_settings.ini)",
                  newValue);
    }
    distanceDownKeyWasDown = distanceDownKeyDown;

    static bool distanceUpKeyWasDown = false;
    bool distanceUpKeyDown = (GetAsyncKeyState(kPlayerBoneDistanceUpHotkey) & 0x8000) != 0;
    if (distanceUpKeyDown && !distanceUpKeyWasDown)
    {
        float newValue = GetPlayerBoneDistanceThreshold() + kPlayerBoneDistanceStep;
        SetPlayerBoneDistanceThreshold(newValue);
        MOHW_LOG(kLogFile, "Numpad9 pressed -- player bone distance threshold now %.2f (saved to mohwvr_settings.ini)",
                  newValue);
    }
    distanceUpKeyWasDown = distanceUpKeyDown;

    static bool cycleKeyWasDown = false;
    bool cycleKeyDown = (GetAsyncKeyState(kEntityCandidateCycleHotkey) & 0x8000) != 0;
    if (cycleKeyDown && !cycleKeyWasDown)
    {
        std::lock_guard<std::mutex> lock(g_entityCandidatesMutex);
        if (g_entityCandidateCount == 0)
        {
            MOHW_LOG(kLogFile, "Numpad0 pressed -- no entity candidates seen yet (walk near yourself/the weapon "
                                "for a moment first)");
        }
        else
        {
            int next = (g_entityCandidateCycleIndex.load(std::memory_order_relaxed) + 1) % g_entityCandidateCount;
            g_entityCandidateCycleIndex.store(next, std::memory_order_relaxed);
            void* entityId = g_entityCandidates[next];
            g_lockedEntityId.store(entityId, std::memory_order_relaxed);
            g_playerBoneManuallyCaptured.store(true, std::memory_order_relaxed);
            ClearKnownEntityBuffers();
            MOHW_LOG(kLogFile, "Numpad0 pressed -- cycled bone-hide lock to candidate #%d/%d: entityId=%p", next,
                      g_entityCandidateCount, entityId);
            DumpEntityStructIfPossible(entityId);
        }
    }
    cycleKeyWasDown = cycleKeyDown;

    static bool boneStepDownKeyWasDown = false;
    bool boneStepDownKeyDown = (GetAsyncKeyState(kSingleBoneStepDownHotkey) & 0x8000) != 0;
    if (boneStepDownKeyDown && !boneStepDownKeyWasDown)
    {
        float newValue = GetBoneHideRangeStart() - 1.0f;
        SetBoneHideRangeStart(newValue);
        SetBoneHideRangeEnd(newValue);
        MOHW_LOG(kLogFile, "Numpad1 pressed -- single bone-hide index now %.0f (saved to mohwvr_settings.ini)",
                  newValue);
    }
    boneStepDownKeyWasDown = boneStepDownKeyDown;

    static bool boneStepUpKeyWasDown = false;
    bool boneStepUpKeyDown = (GetAsyncKeyState(kSingleBoneStepUpHotkey) & 0x8000) != 0;
    if (boneStepUpKeyDown && !boneStepUpKeyWasDown)
    {
        float newValue = GetBoneHideRangeStart() + 1.0f;
        SetBoneHideRangeStart(newValue);
        SetBoneHideRangeEnd(newValue);
        MOHW_LOG(kLogFile, "Numpad2 pressed -- single bone-hide index now %.0f (saved to mohwvr_settings.ini)",
                  newValue);
    }
    boneStepUpKeyWasDown = boneStepUpKeyDown;
}

} // namespace mohw
