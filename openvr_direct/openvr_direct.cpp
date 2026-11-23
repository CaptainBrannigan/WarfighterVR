#include "openvr_direct.h"
#include "../sdk/hotkeys.h"
#include "openvr_types.h"
#include "vr_input.h"
#include "vr_overlay.h"

#include "../sdk/logging.h"
#include "../sdk/vr_math.h"
#include "../hooks/aiming_controller_hook.h"
#include "../hooks/companion_bridge.h"
#include "../hooks/perf_probe.h"
#include "../hooks/render_pose_stamp.h"
#include "../hooks/startup_diag.h"
#include "../shared/ipc_protocol.h"
#include "../sdk/d3d_util.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h> // ID3D11Device5 / ID3D11Fence / ID3D11DeviceContext4: the hand-off slots' fence (COM only, no imports)
#include <dxgi.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace mohw::openvr_direct {
namespace {

constexpr const char* kLogFile = "mohwvr_openvr_direct.log";

// Minimal OpenVR C-ABI surface, loaded dynamically -- see openvr_direct.h's top comment for why (DRM).
using PFN_VR_InitInternal2 = uint32_t(__cdecl*)(int* peError, int eApplicationType, const char* pStartupInfo);
using PFN_VR_GetGenericInterface = void*(__cdecl*)(const char* pchInterfaceVersion, int* peError);
using PFN_VR_IsInterfaceVersionValid = bool(__cdecl*)(const char* pchInterfaceVersion);
using PFN_VR_GetVRInitErrorText = const char*(__cdecl*)(int error);

constexpr int kInitError_None = 0;
constexpr int kAppType_Scene = 1; // EVRApplicationType::VRApplication_Scene

// Minimal stand-in for vr::Texture_t -- plain data, no vtable, no ABI risk reproducing it by hand.
struct DirectTexture
{
    void* handle;
    int eType;       // ETextureType::TextureType_DirectX = 0
    int eColorSpace; // EColorSpace::ColorSpace_Auto = 0
};

// Minimal stand-in for vr::VRTextureWithPose_t (third_party/openvr/headers/openvr.h): a DirectTexture
// (Texture_t) followed by mDeviceToAbsoluteTracking, "the actual pose used to render scene textures" --
// used with the Submit_TextureWithPose flag for render-pose stamping (see render_pose_stamp.h's
// GetPendingRenderPoseStamp comment). Field order/layout must match Texture_t exactly followed by the
// matrix, same "plain data, no vtable" reasoning as DirectTexture above.
struct DirectTextureWithPose
{
    void* handle;
    int eType;
    int eColorSpace;
    float mDeviceToAbsoluteTracking[3][4];
};

constexpr int kSubmitFlag_TextureWithPose = 0x08; // vr::Submit_TextureWithPose

// IVRCompositor methods called through the raw vtable (IVRCompositor_029): SetTrackingSpace(0), GetTrackingSpace(1),
// WaitGetPoses(2), GetLastPoses(3), GetLastPoseForTrackedDeviceIndex(4), GetSubmitTexture(5), Submit(6). C++ virtual
// calls on Windows x86 use __thiscall (this in ECX).
constexpr int kGetTrackingSpaceVtableIndex = 1;
constexpr int kWaitGetPosesVtableIndex = 2;
constexpr int kSubmitVtableIndex = 6;
using PFN_GetTrackingSpace = int(__thiscall*)(void* self);
using PFN_WaitGetPoses = int(__thiscall*)(void* self, void* pRenderPoseArray, uint32_t unRenderPoseArrayCount, void* pGamePoseArray,
                                            uint32_t unGamePoseArrayCount);
using PFN_Submit = int(__thiscall*)(void* self, int eEye, const DirectTexture* pTexture, const void* pBounds, int nSubmitFlags);

// IVRSystem methods, also called through the raw vtable (IVRSystem_026): GetRecommendedRenderTargetSize(0),
// GetProjectionMatrix(1), GetProjectionRaw(2), ..., GetFloatTrackedDeviceProperty(23) -- see
// third_party/openvr/headers/openvr.h's IVRSystem class for the declaration order this indexing depends on
// (counted directly from that header, 0-based, through every virtual method in between).
constexpr int kGetProjectionRawVtableIndex = 2;
using PFN_GetProjectionRaw = void(__thiscall*)(void* self, int eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom);
constexpr int kGetFloatTrackedDevicePropertyVtableIndex = 23;
using PFN_GetFloatTrackedDeviceProperty = float(__thiscall*)(void* self, uint32_t unDeviceIndex, int prop, int* pError);
constexpr uint32_t kTrackedDeviceIndexHmd = 0; // vr::k_unTrackedDeviceIndex_Hmd
constexpr int kProp_DisplayFrequency_Float = 2002; // vr::Prop_DisplayFrequency_Float

// HEADSET RECENTER (2026-09-28): the headset's own recenter (on a Quest 3, holding the Meta button) resets SteamVR's
// zero pose, which SteamVR broadcasts as an event; on one of those the mod recenters head look too (same as F3 /
// the dashboard button). PollNextEvent(30) / GetEventTypeNameFromEnum(33) on IVRSystem_026. Every other event type is
// logged the first few times it arrives, so the exact event a given headset/streamer sends can be read off the log.
constexpr int kPollNextEventVtableIndex = 30;
constexpr int kGetEventTypeNameVtableIndex = 33;
using PFN_PollNextEvent = bool(__thiscall*)(void* self, void* event, uint32_t size);
using PFN_GetEventTypeName = const char*(__thiscall*)(void* self, int eventType);
constexpr uint32_t kEventChaperoneUniverseHasChanged = 801;
constexpr uint32_t kEventSeatedZeroPoseReset = 804;
constexpr uint32_t kEventStandingZeroPoseReset = 808;
// vr::VREvent_t: 12 bytes + 4 padding + a 48-byte data union (pack(8) on Windows) = 64.
struct RawVrEvent
{
    uint32_t eventType;
    uint32_t trackedDeviceIndex;
    float eventAgeSeconds;
    uint32_t padding;
    uint64_t data[6];
};
static_assert(sizeof(RawVrEvent) == 64, "must match vr::VREvent_t");
// Written once on the connect thread before SubmitThreadProc starts, then only read there.
void* g_system = nullptr;
PFN_PollNextEvent g_pollNextEvent = nullptr;
PFN_GetEventTypeName g_getEventTypeName = nullptr;

// SteamVR's recommended render size per eye (2026-11-12): IVRSystem::GetRecommendedRenderTargetSize (vtable 0), which
// carries SteamVR's resolution setting (the global slider and the per-app multiplier). Read at connect and every 2 s
// (the slider can move while the game runs). 0 until read.
using PFN_GetRecommendedRenderTargetSize = void(__thiscall*)(void* self, uint32_t* width, uint32_t* height);
constexpr int kGetRecommendedRenderTargetSizeVtableIndex = 0;
std::atomic<uint32_t> g_recommendedWidth{0}, g_recommendedHeight{0};

void RefreshRecommendedSize(void* system)
{
    if (!system)
        return;
    uint32_t w = 0, h = 0;
    reinterpret_cast<PFN_GetRecommendedRenderTargetSize>(
        (*reinterpret_cast<void***>(system))[kGetRecommendedRenderTargetSizeVtableIndex])(system, &w, &h);
    if (w == 0 || h == 0 || w > 16384 || h > 16384)
        return;
    const uint32_t oldW = g_recommendedWidth.exchange(w, std::memory_order_relaxed);
    const uint32_t oldH = g_recommendedHeight.exchange(h, std::memory_order_relaxed);
    if (oldW != w || oldH != h)
        MOHW_LOG(kLogFile, "SteamVR recommended render size per eye: %ux%u (with its resolution setting)", w, h);
}

void PollSystemEvents()
{
    if (!g_system || !g_pollNextEvent)
        return;
    static unsigned long long nextSizeMs = 0;
    const unsigned long long nowMs = GetTickCount64();
    if (nowMs >= nextSizeMs)
    {
        nextSizeMs = nowMs + 2000;
        RefreshRecommendedSize(g_system);
    }
    RawVrEvent ev{};
    while (g_pollNextEvent(g_system, &ev, sizeof(ev)))
    {
        static std::map<uint32_t, int> seen;
        int& count = seen[ev.eventType];
        if (count < 5)
        {
            const char* name = g_getEventTypeName ? g_getEventTypeName(g_system, static_cast<int>(ev.eventType)) : "?";
            MOHW_LOG(kLogFile, "SteamVR event %u (%s) device %u", ev.eventType, name ? name : "?",
                      ev.trackedDeviceIndex);
        }
        ++count;
        if (ev.eventType == kEventSeatedZeroPoseReset || ev.eventType == kEventStandingZeroPoseReset ||
            ev.eventType == kEventChaperoneUniverseHasChanged)
            RequestRecenter("headset recenter (SteamVR zero-pose event)");
        ev = RawVrEvent{};
    }
}

// Inverse of MatrixToQuat above (render-pose stamping port -- see render_pose_stamp.h's GetPendingRenderPoseStamp
// comment). Places the quaternion's rotated identity basis vectors into the SAME row/column slots MatrixToQuat
// reads them from (column 0/1/2 = the rotated X/Y/Z axes), so this is a true round-trip of that function
// regardless of what those axes physically mean in OpenVR's space -- MatrixToQuat(QuatToDeviceMatrix(q)) == q for
// any q built from a real MatrixToQuat output (which is exactly how render_pose_stamp.cpp's stamped quaternion is
// derived: rotating a MatrixToQuat-sourced HeadPoseBlock orientation by a small arc). posOverride supplies
// m[row][3]; rotation-only otherwise.
void QuatToDeviceMatrix(const mohw::Quat& q, const float pos[3], float outM[3][4])
{
    mohw::Vec3 axisX{1.0f, 0.0f, 0.0f};
    mohw::Vec3 axisY{0.0f, 1.0f, 0.0f};
    mohw::Vec3 axisZ{0.0f, 0.0f, 1.0f};
    mohw::Vec3 col0 = mohw::QuatRotateVector(q, axisX);
    mohw::Vec3 col1 = mohw::QuatRotateVector(q, axisY);
    mohw::Vec3 col2 = mohw::QuatRotateVector(q, axisZ);
    outM[0][0] = col0.x; outM[1][0] = col0.y; outM[2][0] = col0.z;
    outM[0][1] = col1.x; outM[1][1] = col1.y; outM[2][1] = col1.z;
    outM[0][2] = col2.x; outM[1][2] = col2.y; outM[2][2] = col2.z;
    outM[0][3] = pos[0]; outM[1][3] = pos[1]; outM[2][3] = pos[2];
}

enum class ConnectState : int
{
    NotStarted = 0,
    Connecting = 1,
    Connected = 2,
    Failed = 3,
};
std::atomic<int> g_state{static_cast<int>(ConnectState::NotStarted)};

// DYNAMIC FRAME-PACE TARGET (2026-09-25): populated once via IVRSystem::GetFloatTrackedDeviceProperty
// (Prop_DisplayFrequency_Float) during connect, instead of hardcoding an assumed display rate -- present_hook.cpp
// paces Present() off this so it tracks whatever the real headset/Steam Link target actually is (confirmed live
// this session: Present was running at an uncapped ~200Hz against a 120Hz real submit target, a 5:3 mismatch
// with no clean integer relationship -- the kind of thing that produces beat-frequency judder, visible only when
// the underlying value is actually changing, i.e. during rotation). Atomic since it's set on the connect thread
// and read on the game's own render thread, with no other ordering guarantee tying the two together the way the
// g_compositor/g_waitGetPoses/g_submit one-time handoff below has. 0.0f means "not yet known".
std::atomic<float> g_detectedDisplayFrequencyHz{0.0f};

// Only meaningful once g_state == Connected. Plain (non-atomic) globals are safe here: they're fully populated on
// the connect thread BEFORE the release-store to g_state below, and only ever read on the render thread AFTER an
// acquire-load observes Connected -- standard one-time background-init handoff.
void* g_compositor = nullptr;
PFN_WaitGetPoses g_waitGetPoses = nullptr;
PFN_Submit g_submit = nullptr;

// Separate-device design (see openvr_direct.h's top comment for why): a second, fully independent ID3D11Device
// (g_ownDevice below) is created on the SAME adapter as the game's device. The game thread creates the eye textures
// with D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX and does CopyResource into them; a dedicated thread that owns g_ownDevice
// exclusively opens those same textures via OpenSharedResource (standard cross-device GPU resource sharing) and
// performs the paired WaitGetPoses+Submit sequence entirely on its own device/thread. (D3D11_RESOURCE_MISC_SHARED alone,
// without the keyed mutex, allowed a genuine torn-frame race between the two devices.)
//
// HAND-OFF SLOTS (2026-11-17): the game thread and SubmitThreadProc never wait on each other. Before, one shared texture
// per eye was a plain lock between them (keyed mutex, 5 ms timeouts on each side), held by SubmitThreadProc through its
// two Submit calls; Submit can only copy a pair once the GPU has finished writing it, behind the whole game frame. So
// whenever the GPU ran behind (a level's first minute, mohwvr_perf.log), the game waited up to 5 ms per eye at every
// Present (6-9 ms a frame) and the submit thread fell to 30-40 loops a second against the headset's 120. Worse, a timed-
// out AcquireSync returns WAIT_TIMEOUT (0x102), a success code to SUCCEEDED(), so a timed-out wait went ahead as if it
// held the lock: the game's copies went to textures it didn't own (dropped or torn) while the pair's stamp moved on --
// the old image reprojected with the new pose. Every AcquireSync result is now compared with S_OK.
//
// Now: kSlotCount pairs of shared eye textures, each with its keyed mutexes. The game copies each frame into a slot
// nobody else is using, signals g_gameFence after the copies, and publishes the slot with a sequence number.
// SubmitThreadProc keeps the slot it shows (its keyed mutexes stay acquired across loops; re-submitted every loop with
// that slot's stamp) and moves to the NEWEST pair THE GPU HAS FINISHED -- not simply the newest published: while the
// game is GPU-bound the CPU runs up to the swap chain's frame latency (3 by default) ahead of the GPU, so the newest
// published pair is never finished when the submit thread looks (the first try took only the newest: the headset froze
// for the first ~48 s of a level, 2026-11-17). Hence six slots: the one shown, the newest finished one, up to three
// in flight, and the one being written. The game never overwrites the newest finished pair; with no free slot it
// reuses the oldest other published one (that frame is lost, counted as dropped). Neither side ever blocks: AcquireSync
// with no timeout, on slots the other side has let go of. Slot bookkeeping is under g_slotMutex, held for a few
// instructions on either thread, never across a GPU wait. Without fences (no ID3D11Device5 / ID3D11DeviceContext4) every
// published pair counts as finished: the newest is taken at once, as before, but the game still never waits on the
// submit thread.
constexpr int kSlotCount = 6;
enum SlotState : int
{
    kSlotFree = 0,  // nobody's: the game may write it
    kSlotWriting,   // the game is copying into it
    kSlotPublished, // written, waiting for SubmitThreadProc
    kSlotHeld,      // SubmitThreadProc shows it
};

// Game thread only.
struct GameSlot
{
    ID3D11Texture2D* tex[2] = {}; // left, right
    IDXGIKeyedMutex* mutex[2] = {};
};
GameSlot g_gameSlots[kSlotCount];
D3D11_TEXTURE2D_DESC g_slotDesc[2] = {};
ID3D11Fence* g_gameFence = nullptr;
UINT64 g_fenceValue = 0;

// Shared by both threads, under g_slotMutex.
std::mutex g_slotMutex;
int g_slotSetGeneration = 0;             // bumped whenever the game remakes the slots (a new device, a new size)
HANDLE g_slotHandle[kSlotCount][2] = {}; // GetSharedHandle: a weak handle, valid while the game's texture lives
HANDLE g_fenceShareHandle = nullptr;     // NT handle (CreateSharedHandle), closed when the set is remade
int g_slotState[kSlotCount] = {};
UINT64 g_slotFence[kSlotCount] = {};     // g_gameFence's value once the slot's copies are done (0: no fence)
UINT64 g_slotSeq[kSlotCount] = {};       // the publish order (never reset, also across sets)
PairStamp g_slotStamp[kSlotCount];
UINT64 g_publishSeq = 0;

// Under g_slotMutex: the published pair with the highest sequence whose copies the GPU has finished (completed = the
// fence's completed value; a pair published without a fence counts as finished), newer than afterSeq. -1 if none.
int NewestFinishedLocked(UINT64 completed, UINT64 afterSeq)
{
    int best = -1;
    for (int i = 0; i < kSlotCount; ++i)
        if (g_slotState[i] == kSlotPublished && g_slotSeq[i] > afterSeq && g_slotFence[i] <= completed &&
            (best < 0 || g_slotSeq[i] > g_slotSeq[best]))
            best = i;
    return best;
}

std::atomic<long long> g_droppedPairs{0}; // game frames that never reached the headset's queue (see PublishPair)

std::atomic<bool> g_pairPublishEnabled{true};

// The second, independent D3D11 device (see the design comment near g_gameSlots) -- created once, lazily, the first
// time UpdateOpenVrDirect runs with a real game device available (needed to find the matching adapter). Written
// once on the game thread strictly BEFORE SubmitThreadProc is started (std::thread's constructor establishes a
// happens-before relationship for everything written before it), then read-only for the rest of the process's
// life -- no atomics needed for g_ownDevice itself.
ID3D11Device* g_ownDevice = nullptr;
std::atomic<bool> g_ownDeviceStarted{false};

// Game thread, under g_slotMutex: lets go of the game's side of the slots.
void ReleaseGameSlotsLocked()
{
    for (GameSlot& slot : g_gameSlots)
        for (int e = 0; e < 2; ++e)
        {
            if (slot.mutex[e])
                slot.mutex[e]->Release();
            if (slot.tex[e])
                slot.tex[e]->Release();
            slot.mutex[e] = nullptr;
            slot.tex[e] = nullptr;
        }
    if (g_gameFence)
        g_gameFence->Release();
    g_gameFence = nullptr;
    if (g_fenceShareHandle)
        CloseHandle(g_fenceShareHandle);
    g_fenceShareHandle = nullptr;
    for (int s = 0; s < kSlotCount; ++s)
        g_slotHandle[s][0] = g_slotHandle[s][1] = nullptr;
}

bool SameEyeTexture(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b)
{
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format;
}

// Game thread: the hand-off slots exist on this device at these eyes' sizes and formats, (re)made when not -- a level
// change brings a new game device, a resolution change new sizes. SubmitThreadProc re-opens them by the generation.
bool EnsureSlots(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& leftDesc, const D3D11_TEXTURE2D_DESC& rightDesc)
{
    if (g_gameSlots[0].tex[0] && SameEyeTexture(g_slotDesc[0], leftDesc) && SameEyeTexture(g_slotDesc[1], rightDesc) &&
        TextureOnDevice(g_gameSlots[0].tex[0], device))
        return true;

    std::lock_guard<std::mutex> lock(g_slotMutex);
    ReleaseGameSlotsLocked();
    g_slotDesc[0] = g_slotDesc[1] = D3D11_TEXTURE2D_DESC{};
    ++g_slotSetGeneration;
    for (int s = 0; s < kSlotCount; ++s)
    {
        g_slotState[s] = kSlotFree;
        g_slotFence[s] = 0;
        g_slotSeq[s] = 0;
        g_slotStamp[s] = PairStamp{};
    }

    const D3D11_TEXTURE2D_DESC* sources[2] = {&leftDesc, &rightDesc};
    for (int s = 0; s < kSlotCount; ++s)
        for (int e = 0; e < 2; ++e)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = sources[e]->Width;
            desc.Height = sources[e]->Height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = sources[e]->Format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX; // exclusive with plain SHARED, per the D3D11 docs
            GameSlot& slot = g_gameSlots[s];
            IDXGIResource* resource = nullptr;
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, &slot.tex[e]);
            if (SUCCEEDED(hr))
                hr = slot.tex[e]->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&slot.mutex[e]));
            if (SUCCEEDED(hr))
                hr = slot.tex[e]->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
            if (SUCCEEDED(hr))
                hr = resource->GetSharedHandle(&g_slotHandle[s][e]);
            if (resource)
                resource->Release();
            if (FAILED(hr) || !g_slotHandle[s][e])
            {
                MOHW_LOG(kLogFile, "hand-off slot %d eye %d (%ux%u fmt %d) FAILED 0x%08lX", s, e, desc.Width, desc.Height,
                          static_cast<int>(desc.Format), hr);
                ReleaseGameSlotsLocked();
                return false;
            }
        }
    g_slotDesc[0] = leftDesc;
    g_slotDesc[1] = rightDesc;

    // The fence that tells SubmitThreadProc a slot's copies are done on the GPU (optional, see the slots' comment).
    g_fenceValue = 0;
    ID3D11Device5* device5 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5))) && device5)
    {
        if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
                                        reinterpret_cast<void**>(&g_gameFence))))
            g_gameFence = nullptr;
        else if (FAILED(g_gameFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g_fenceShareHandle)))
        {
            g_gameFence->Release();
            g_gameFence = nullptr;
            g_fenceShareHandle = nullptr;
        }
        device5->Release();
    }
    MOHW_LOG(kLogFile, "hand-off slots (set %d): %d pairs, eyes %ux%u / %ux%u fmt %d, %s", g_slotSetGeneration, kSlotCount,
              leftDesc.Width, leftDesc.Height, rightDesc.Width, rightDesc.Height, static_cast<int>(leftDesc.Format),
              g_gameFence ? "GPU fence" : "no fence: taken at once");
    return true;
}

// Game thread: copies one pair into a slot and publishes it with its stamp. Never waits. The slot: a free one; else the
// oldest published pair other than the newest one the GPU has finished (what the submit thread takes next) -- that
// frame never reaches the headset, counted as dropped; else (or a slot's lock not free at once) this frame isn't handed
// over, the headset keeps showing what it has.
void PublishPair(ID3D11DeviceContext* context, ID3D11Texture2D* left, ID3D11Texture2D* right, const PairStamp& stamp)
{
    int s = -1;
    bool reused = false;
    {
        const UINT64 completed = g_gameFence ? g_gameFence->GetCompletedValue() : ~0ull;
        std::lock_guard<std::mutex> lock(g_slotMutex);
        for (int i = 0; i < kSlotCount && s < 0; ++i)
            if (g_slotState[i] == kSlotFree)
                s = i;
        if (s < 0)
        {
            const int keep = NewestFinishedLocked(completed, 0);
            for (int i = 0; i < kSlotCount; ++i)
                if (g_slotState[i] == kSlotPublished && i != keep && (s < 0 || g_slotSeq[i] < g_slotSeq[s]))
                    s = i;
            reused = s >= 0;
        }
        if (s >= 0)
            g_slotState[s] = kSlotWriting;
    }
    if (reused)
        PerfAdd(kPerfDropped, 0);
    GameSlot* slot = s >= 0 ? &g_gameSlots[s] : nullptr;
    // S_OK only: WAIT_TIMEOUT (0x102) and WAIT_ABANDONED (0x80) are success codes to SUCCEEDED(). No timeout: a free
    // slot's locks were let go of by the submit thread before it freed the slot.
    const bool leftLocked = slot && slot->mutex[0]->AcquireSync(0, 0) == S_OK;
    const bool rightLocked = leftLocked && slot->mutex[1]->AcquireSync(0, 0) == S_OK;
    if (!rightLocked)
    {
        if (leftLocked)
            slot->mutex[0]->ReleaseSync(0);
        if (s >= 0)
        {
            std::lock_guard<std::mutex> lock(g_slotMutex);
            g_slotState[s] = kSlotFree;
        }
        PerfAdd(kPerfDropped, 0);
        const long long dropped = ++g_droppedPairs;
        if (dropped <= 5 || dropped % 1000 == 0)
            MOHW_LOG(kLogFile, "pair not handed over: %s (count %lld)", s < 0 ? "no free slot" : "a slot's lock wasn't free",
                      dropped);
        return;
    }
    context->CopyResource(slot->tex[0], left);
    context->CopyResource(slot->tex[1], right);
    slot->mutex[1]->ReleaseSync(0);
    slot->mutex[0]->ReleaseSync(0);
    // Queued after the copies on the same context: reached on the GPU once they're done. Goes out with Present's flush.
    UINT64 fence = 0;
    ID3D11DeviceContext4* context4 = nullptr;
    if (g_gameFence &&
        SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&context4))) && context4)
    {
        if (SUCCEEDED(context4->Signal(g_gameFence, g_fenceValue + 1)))
            fence = ++g_fenceValue;
        context4->Release();
    }

    std::lock_guard<std::mutex> lock(g_slotMutex);
    g_slotFence[s] = fence;
    g_slotStamp[s] = stamp;
    g_slotSeq[s] = ++g_publishSeq;
    g_slotState[s] = kSlotPublished;
}

// The runtime and every tracked device, to mohwvr_startup.log: which driver a tester's headset comes through (Steam
// Link's vrlink, Virtual Desktop's, Oculus...) and which controller type, since default bindings only cover Touch.
void ReportVrDevices(void* system)
{
    constexpr int kGetTrackedDeviceClassVtableIndex = 20, kGetStringPropertyVtableIndex = 28,
                  kGetRuntimeVersionVtableIndex = 49;
    constexpr int kPropTrackingSystemName = 1000, kPropModelNumber = 1001, kPropManufacturerName = 1005,
                  kPropDriverVersion = 1031, kPropControllerType = 7000;
    using GetClassFn = int(__thiscall*)(void*, uint32_t);
    using GetStringFn = uint32_t(__thiscall*)(void*, uint32_t, int, char*, uint32_t, int*);
    using GetRuntimeVersionFn = const char*(__thiscall*)(void*);
    void** vtable = *reinterpret_cast<void***>(system);
    auto getClass = reinterpret_cast<GetClassFn>(vtable[kGetTrackedDeviceClassVtableIndex]);
    auto getString = reinterpret_cast<GetStringFn>(vtable[kGetStringPropertyVtableIndex]);
    const char* runtime = reinterpret_cast<GetRuntimeVersionFn>(vtable[kGetRuntimeVersionVtableIndex])(system);
    const char* report = StartupLogFile();
    MOHW_LOG(report, "runtime version: %s", runtime ? runtime : "?");
    for (uint32_t device = 0; device < 16; ++device)
    {
        int deviceClass = getClass(system, device);
        if (deviceClass == 0)
            continue;
        char tracking[64] = "", model[64] = "", maker[64] = "", driver[64] = "", type[64] = "";
        int error = 0;
        getString(system, device, kPropTrackingSystemName, tracking, sizeof(tracking), &error);
        getString(system, device, kPropModelNumber, model, sizeof(model), &error);
        getString(system, device, kPropManufacturerName, maker, sizeof(maker), &error);
        getString(system, device, kPropDriverVersion, driver, sizeof(driver), &error);
        getString(system, device, kPropControllerType, type, sizeof(type), &error);
        MOHW_LOG(report, "VR device %u: class %d, tracking system \"%s\", model \"%s\", by \"%s\", driver \"%s\", "
                         "controller type \"%s\"", device, deviceClass, tracking, model, maker, driver, type);
    }
}

// The GPU the game renders on, and whether it's the one SteamVR wants: on a laptop or a multi-GPU PC they can differ.
void ReportGameAdapter(IDXGIAdapter* adapter)
{
    constexpr int kGetDXGIOutputInfoVtableIndex = 8;
    using GetOutputInfoFn = void(__thiscall*)(void*, int32_t*);
    const char* report = StartupLogFile();
    DXGI_ADAPTER_DESC mine{};
    if (!adapter || FAILED(adapter->GetDesc(&mine)))
    {
        MOHW_LOG(report, "game GPU: unknown");
        return;
    }
    MOHW_LOG(report, "game GPU: %ls, %llu MB", mine.Description,
             static_cast<unsigned long long>(mine.DedicatedVideoMemory) / (1024 * 1024));
    if (!g_system)
        return;
    int32_t wanted = -1;
    reinterpret_cast<GetOutputInfoFn>((*reinterpret_cast<void***>(g_system))[kGetDXGIOutputInfoVtableIndex])(g_system,
                                                                                                             &wanted);
    IDXGIFactory* factory = nullptr;
    IDXGIAdapter* steamVrAdapter = nullptr;
    DXGI_ADAPTER_DESC theirs{};
    if (wanted >= 0 && SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) &&
        SUCCEEDED(factory->EnumAdapters(static_cast<UINT>(wanted), &steamVrAdapter)) &&
        SUCCEEDED(steamVrAdapter->GetDesc(&theirs)))
    {
        bool same = theirs.AdapterLuid.LowPart == mine.AdapterLuid.LowPart &&
                    theirs.AdapterLuid.HighPart == mine.AdapterLuid.HighPart;
        MOHW_LOG(report, "SteamVR's GPU: %ls -- %s", theirs.Description,
                 same ? "the same as the game's" : "DIFFERENT from the game's");
    }
    else
    {
        MOHW_LOG(report, "SteamVR's GPU: adapter index %d, couldn't be compared", wanted);
    }
    if (steamVrAdapter)
        steamVrAdapter->Release();
    if (factory)
        factory->Release();
}

// One connection attempt (TryConnectOpenVrDirect): NotNow = no headset this time, try again later (the state goes back
// to NotStarted); Failed = connected to SteamVR but something it needs is missing, don't retry.
ConnectResult ConnectAttempt()
{
    // The main game folder, where the SteamVR-current copy was placed -- also from the driving missions'
    // game\MOHWD.exe (ModuleFolderPath resolves its game\ folder to the main one), so one copy serves both. Loaded once.
    const char* report = StartupLogFile();
    static HMODULE h = nullptr;
    if (!h)
    {
        char path[MAX_PATH] = {};
        strcpy_s(path, ModuleFolderPath("openvr_api.dll").c_str());
        MOHW_LOG(kLogFile, "loading %s ...", path);
        BeginStartupStep("loading openvr_api.dll");
        h = LoadLibraryA(path);
        DWORD loadError = GetLastError();
        EndStartupStep();
        if (!h)
        {
            MOHW_LOG(kLogFile, "LoadLibraryA FAILED, GetLastError=%lu", loadError);
            MOHW_LOG(report, "openvr_api.dll failed to load, error %lu%s", loadError,
                     loadError == ERROR_MOD_NOT_FOUND     ? " (file missing)"
                     : loadError == ERROR_BAD_EXE_FORMAT ? " (wrong bitness: needs SteamVR's bin\\win32 copy)"
                                                         : "");
            return ConnectResult::Failed;
        }
    }

    auto initInternal2 = reinterpret_cast<PFN_VR_InitInternal2>(GetProcAddress(h, "VR_InitInternal2"));
    auto getGenericInterface = reinterpret_cast<PFN_VR_GetGenericInterface>(GetProcAddress(h, "VR_GetGenericInterface"));
    auto isInterfaceVersionValid =
        reinterpret_cast<PFN_VR_IsInterfaceVersionValid>(GetProcAddress(h, "VR_IsInterfaceVersionValid"));
    if (!initInternal2 || !getGenericInterface || !isInterfaceVersionValid)
    {
        MOHW_LOG(kLogFile, "GetProcAddress FAILED for one or more exports");
        MOHW_LOG(report, "openvr_api.dll loaded but lacks the functions the mod needs (an unexpected build of it)");
        return ConnectResult::Failed;
    }

    // No VR_IsHmdPresent first (removed 2026-10-16): it loads SteamVR's client and the headset drivers INTO this
    // process to ask them directly. Live, the game hung at startup with no window, every thread waiting, and the log
    // stopped right here, while SteamVR's Steam Link driver was failing ("NVENC: Invalid Level", fatal, 3 s later).
    // It was no filter anyway (it said yes with no headset: the wireless driver counts as present). The init below
    // goes to the running vrserver over IPC and is the real test.
    // Only ever called with SteamVR already running (hooks/master_switch.cpp), so this never launches it.
    int err = kInitError_None;
    static bool firstInit = true;
    BeginStartupStep("VR_Init (connecting to SteamVR)", !firstInit);
    firstInit = false;
    uint32_t token = initInternal2(&err, kAppType_Scene, nullptr);
    EndStartupStep();
    if (err != kInitError_None)
    {
        // Every failure lands here, not only "no headset" (215): SteamVR's own words for which.
        static int lastErr = 0;
        if (err != lastErr)
        {
            auto symbol = reinterpret_cast<PFN_VR_GetVRInitErrorText>(GetProcAddress(h, "VR_GetVRInitErrorAsSymbol"));
            auto english =
                reinterpret_cast<PFN_VR_GetVRInitErrorText>(GetProcAddress(h, "VR_GetVRInitErrorAsEnglishDescription"));
            const char* name = symbol ? symbol(err) : nullptr;
            const char* description = english ? english(err) : nullptr;
            MOHW_LOG(kLogFile, "VR_InitInternal2() err=%d %s: %s", err, name ? name : "?", description ? description : "?");
            MOHW_LOG(report, "VR_Init failed, error %d %s: %s", err, name ? name : "?", description ? description : "?");
        }
        lastErr = err;
        return ConnectResult::NotNow;
    }
    MOHW_LOG(kLogFile, "VR_InitInternal2() returned token=%u -- connected", token);
    MOHW_LOG(report, "VR_Init succeeded");
    g_state.store(static_cast<int>(ConnectState::Connecting), std::memory_order_relaxed);

    bool ifaceOk = isInterfaceVersionValid("IVRSystem_026");
    MOHW_LOG(kLogFile, "IVRSystem_026 valid = %d", ifaceOk);
    for (const char* name : {"IVRSystem_026", "IVRCompositor_029", "IVRInput_011", "IVROverlay_028"})
        if (!isInterfaceVersionValid(name))
            MOHW_LOG(report, "this runtime doesn't offer %s", name);

    // Real per-eye HMD frustum, published into the SAME HmdViewBlock hooks/eye_matched_fov.cpp already knows how
    // to consume (true per-eye frustum, no crop).
    int sysErr = kInitError_None;
    void* system = getGenericInterface("IVRSystem_026", &sysErr);
    MOHW_LOG(kLogFile, "IVRSystem_026 = %p, err=%d", system, sysErr);
    if (system)
    {
        ReportVrDevices(system);
        void** sysVtable = *reinterpret_cast<void***>(system);
        g_pollNextEvent = reinterpret_cast<PFN_PollNextEvent>(sysVtable[kPollNextEventVtableIndex]);
        g_getEventTypeName = reinterpret_cast<PFN_GetEventTypeName>(sysVtable[kGetEventTypeNameVtableIndex]);
        g_system = system;
        RefreshRecommendedSize(system);
        auto getProjectionRaw = reinterpret_cast<PFN_GetProjectionRaw>(sysVtable[kGetProjectionRawVtableIndex]);
        auto getFloatProp =
            reinterpret_cast<PFN_GetFloatTrackedDeviceProperty>(sysVtable[kGetFloatTrackedDevicePropertyVtableIndex]);

        int freqErr = 0;
        float displayHz = getFloatProp(system, kTrackedDeviceIndexHmd, kProp_DisplayFrequency_Float, &freqErr);
        MOHW_LOG(kLogFile, "GetFloatTrackedDeviceProperty(DisplayFrequency) = %.3f Hz, err=%d", displayHz, freqErr);
        if (freqErr == 0 && displayHz > 1.0f && displayHz < 1000.0f) // sanity bound, same philosophy as other hooks' transform checks
            g_detectedDisplayFrequencyHz.store(displayHz, std::memory_order_relaxed);

        mohwvr::ipc::HmdViewBlock hmd{};
        constexpr float kRadToDeg = 57.29577951f;
        for (int eye = 0; eye < 2; ++eye)
        {
            float rawLeft = 0, rawRight = 0, rawTop = 0, rawBottom = 0;
            getProjectionRaw(system, eye, &rawLeft, &rawRight, &rawTop, &rawBottom);
            hmd.angleLeft[eye] = atanf(rawLeft);
            hmd.angleRight[eye] = atanf(rawRight);
            // VERIFIED LIVE: horizontal (Left/Right) matched shared/eye_frustum.h's documented Quest 3 ground
            // truth (-54/+40 deg) directly, but vertical came out SWAPPED, not just sign-flipped -- raw pfTop's
            // magnitude matched the expected DOWN angle and pfBottom's matched the expected UP angle. On this
            // runtime, OpenVR's Top/Bottom raw params are transposed relative to HmdViewBlock's
            // up-positive/down-negative convention, not merely negated.
            hmd.angleUp[eye] = atanf(rawBottom);
            hmd.angleDown[eye] = atanf(rawTop);
            MOHW_LOG(kLogFile,
                      "eye%d GetProjectionRaw: raw L/R/T/B = %.4f/%.4f/%.4f/%.4f -> angles L/R/U/D = %.1f/%.1f/%.1f/%.1f deg",
                      eye, rawLeft, rawRight, rawTop, rawBottom, hmd.angleLeft[eye] * kRadToDeg, hmd.angleRight[eye] * kRadToDeg,
                      hmd.angleUp[eye] * kRadToDeg, hmd.angleDown[eye] * kRadToDeg);
            MOHW_LOG(report, "eye %d raw projection L/R/T/B: %.4f / %.4f / %.4f / %.4f", eye, rawLeft, rawRight, rawTop,
                     rawBottom);
        }
        hmd.ready = 1;
        hmd.viewMode = mohwvr::ipc::kViewModeTrueFrustum;
        hmd.frustumCandidateIndex = -1; // write to every match
        SetHmdViewOverride(hmd);
        MOHW_LOG(kLogFile, "published HmdView override (true-frustum mode) from OpenVR's real per-eye geometry");
    }

    int compErr = kInitError_None;
    void* compositor = getGenericInterface("IVRCompositor_029", &compErr);
    MOHW_LOG(kLogFile, "IVRCompositor_029 = %p, err=%d", compositor, compErr);
    if (!compositor)
    {
        MOHW_LOG(report, "no compositor from this runtime (IVRCompositor_029, error %d)", compErr);
        g_state.store(static_cast<int>(ConnectState::Failed), std::memory_order_release);
        return ConnectResult::Failed;
    }

    void** vtable = *reinterpret_cast<void***>(compositor);
    g_compositor = compositor;
    g_waitGetPoses = reinterpret_cast<PFN_WaitGetPoses>(vtable[kWaitGetPosesVtableIndex]);
    g_submit = reinterpret_cast<PFN_Submit>(vtable[kSubmitVtableIndex]);

    // Controllers: SteamVR Input, in the compositor's own tracking space so hand poses match the head pose.
    int trackingUniverse = reinterpret_cast<PFN_GetTrackingSpace>(vtable[kGetTrackingSpaceVtableIndex])(compositor);
    int inputErr = kInitError_None;
    void* input = getGenericInterface("IVRInput_011", &inputErr);
    MOHW_LOG(kLogFile, "IVRInput_011 = %p, err=%d", input, inputErr);
    if (!InitVrInput(input, trackingUniverse))
    {
        MOHW_LOG(kLogFile, "SteamVR Input setup FAILED -- continuing without controllers (see mohwvr_vrinput.log)");
        MOHW_LOG(report, "SteamVR Input setup failed: no controllers, see mohwvr_vrinput.log");
    }

    // Settings menu: a "MOHW VR" tab in the SteamVR dashboard.
    int overlayErr = kInitError_None;
    void* overlay = getGenericInterface("IVROverlay_028", &overlayErr);
    MOHW_LOG(kLogFile, "IVROverlay_028 = %p, err=%d", overlay, overlayErr);
    if (!InitVrOverlay(overlay, trackingUniverse))
    {
        MOHW_LOG(kLogFile, "dashboard settings tab setup FAILED -- continuing without it (see mohwvr_overlay.log)");
        MOHW_LOG(report, "dashboard settings tab setup failed, see mohwvr_overlay.log");
    }

    MOHW_LOG(kLogFile, "CONNECTED -- game thread will start the separate-device SubmitThreadProc on its first call");
    g_state.store(static_cast<int>(ConnectState::Connected), std::memory_order_release);
    return ConnectResult::Connected;
}

// SubmitThreadProc's side of the hand-off slots: the game's slot textures opened on g_ownDevice, the shared fence, and
// the slot shown. SubmitThreadProc only.
struct OwnSlots
{
    ID3D11Texture2D* tex[kSlotCount][2] = {};
    IDXGIKeyedMutex* mutex[kSlotCount][2] = {};
    ID3D11Fence* fence = nullptr;
    int generation = -1;       // the game's set these were opened from
    int failedGeneration = -1; // a set that wouldn't open: not retried every loop
    int held = -1;             // the slot shown: its keyed mutexes stay acquired on g_ownDevice
    UINT64 heldSeq = 0;        // its publish sequence: only newer pairs are taken (sequences never reset)
    PairStamp heldStamp;
    unsigned long long lastTakeMs = 0; // GetTickCount64 of the last pair taken (the stale-take safety valve)
};

void ReleaseOwnSlots(OwnSlots& own)
{
    if (own.held >= 0)
        for (int e = 1; e >= 0; --e)
            if (own.mutex[own.held][e])
                own.mutex[own.held][e]->ReleaseSync(0);
    own.held = -1;
    for (int s = 0; s < kSlotCount; ++s)
        for (int e = 0; e < 2; ++e)
        {
            if (own.mutex[s][e])
                own.mutex[s][e]->Release();
            if (own.tex[s][e])
                own.tex[s][e]->Release();
            own.mutex[s][e] = nullptr;
            own.tex[s][e] = nullptr;
        }
    if (own.fence)
        own.fence->Release();
    own.fence = nullptr;
    own.generation = -1;
}

// Under g_slotMutex (the game's handles stay valid while it's held): opens the game's current set on g_ownDevice when
// it's a new one. False while there is none, or it wouldn't open.
bool OpenSlotsLocked(OwnSlots& own)
{
    if (own.generation == g_slotSetGeneration)
        return true;
    ReleaseOwnSlots(own);
    if (!g_slotHandle[0][0] || own.failedGeneration == g_slotSetGeneration)
        return false;
    for (int s = 0; s < kSlotCount; ++s)
        for (int e = 0; e < 2; ++e)
        {
            HRESULT hr = g_ownDevice->OpenSharedResource(g_slotHandle[s][e], __uuidof(ID3D11Texture2D),
                                                         reinterpret_cast<void**>(&own.tex[s][e]));
            if (SUCCEEDED(hr))
                hr = own.tex[s][e]->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&own.mutex[s][e]));
            if (FAILED(hr))
            {
                MOHW_LOG(kLogFile, "submit thread: opening hand-off slot %d eye %d FAILED 0x%08lX", s, e, hr);
                ReleaseOwnSlots(own);
                own.failedGeneration = g_slotSetGeneration;
                return false;
            }
        }
    ID3D11Device5* own5 = nullptr;
    if (g_fenceShareHandle &&
        SUCCEEDED(g_ownDevice->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&own5))) && own5)
    {
        if (FAILED(own5->OpenSharedFence(g_fenceShareHandle, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&own.fence))))
            own.fence = nullptr;
        own5->Release();
    }
    own.generation = g_slotSetGeneration;
    MOHW_LOG(kLogFile, "submit thread: hand-off slots (set %d) opened, %s", own.generation,
              own.fence ? "GPU fence" : "no fence: taken at once");
    return true;
}

// DIRECT SUBMIT-RATE MEASUREMENT (2026-09-25): the ~200Hz submission loop rate was previously inferred indirectly
// via GetHeadPose's producer frameCounter (companion_bridge.cpp) -- structurally the same rate, since
// WaitGetPoses/Submit are paired in this one loop, but the user asked for it measured directly rather than
// inferred. Self-contained NUMPAD5 burst here (own edge-detection, own counter, own high-res QPC timing) --
// deliberately independent of companion_bridge.cpp's identical-looking burst so this thread doesn't need any
// cross-module coupling; both fire off the same keypress since present_hook.cpp's poll and this thread's own
// poll run in parallel.
long long QpcMicros()
{
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (now.QuadPart * 1000000LL) / freq.QuadPart;
}

// Owns g_ownDevice exclusively for the rest of the process's life -- see the design comment near g_gameSlots for
// why this exists. WaitGetPoses and Submit are called together, paired, right here, on this dedicated thread.
void SubmitThreadProc()
{
    OwnSlots own;

    bool submitBurstKeyWasDown = false;
    int submitBurstRemaining = 0;
    long long lastSubmitUs = 0;

    // POSE PERTURBATION TEST (2026-09-25), NUMPAD7 cycles: 0 = off, 1 = right eye's submitted pose yawed,
    // 2 = left eye's submitted pose yawed. Tests whether the compositor honours eye 1's pose at all: if mode 1
    // doesn't visibly shift the right eye's image, SteamVR is using something other than eye 1's own pose.
    constexpr float kPosePerturbYawDeg = 5.0f;
    bool perturbKeyWasDown = false;
    int perturbMode = 0;

    // STAMP SLOT FLIP TEST (2026-09-25), NUMPAD6 toggles: the compositor applies eye 0's pose to both eyes (NUMPAD7
    // result), so the shared stamp decides which eye's texture matches it. Taking it from the right eye's slot
    // instead of the left should move the every-other-Present mismatch, and so the ghost, onto the left eye.
    bool stampFlipKeyWasDown = false;
    bool stampFromRightSlot = false;

    bool pairKeyWasDown = false;

    for (;;)
    {
        bool submitBurstKeyDown = ModKeyDown(VK_NUMPAD5);
        if (submitBurstKeyDown && !submitBurstKeyWasDown)
        {
            submitBurstRemaining = 300;
            MOHW_LOG(kLogFile, "===== SUBMIT BURST ARMED: logging next 300 loop iterations unconditionally =====");
        }
        submitBurstKeyWasDown = submitBurstKeyDown;

        bool perturbKeyDown = ModKeyDown(VK_NUMPAD7);
        bool perturbModeChanged = perturbKeyDown && !perturbKeyWasDown;
        if (perturbModeChanged)
            perturbMode = (perturbMode + 1) % 3;
        perturbKeyWasDown = perturbKeyDown;

        bool stampFlipKeyDown = ModKeyDown(VK_NUMPAD6);
        if (stampFlipKeyDown && !stampFlipKeyWasDown)
        {
            stampFromRightSlot = !stampFromRightSlot;
            MOHW_LOG(kLogFile, "NUMPAD6 stamp slot flip test -> shared stamp now taken from the %s eye's slot",
                      stampFromRightSlot ? "RIGHT" : "LEFT (normal)");
        }
        stampFlipKeyWasDown = stampFlipKeyDown;

        bool pairKeyDown = ModKeyDown(VK_NUMPAD3);
        if (pairKeyDown && !pairKeyWasDown)
        {
            bool enabled = !g_pairPublishEnabled.load(std::memory_order_relaxed);
            g_pairPublishEnabled.store(enabled, std::memory_order_relaxed);
            MOHW_LOG(kLogFile, "NUMPAD3 pair publish -> %s", enabled ? "ON (fix active; NUMPAD6 flip ignored)" : "OFF (old per-eye publishing)");
        }
        pairKeyWasDown = pairKeyDown;

        RawTrackedDevicePose hmdPose{};
        g_waitGetPoses(g_compositor, &hmdPose, 1, nullptr, 0);
        UpdateVrInput();
        PollSystemEvents();

        if (hmdPose.poseIsValid)
        {
            mohw::Quat q = MatrixToQuat(hmdPose.deviceToAbsoluteTracking);
            mohwvr::ipc::HeadPoseBlock headPose{};
            headPose.ready = 1;
            static UINT64 frameCounter = 0;
            headPose.frameCounter = ++frameCounter;
            headPose.orientationX = q.x;
            headPose.orientationY = q.y;
            headPose.orientationZ = q.z;
            headPose.orientationW = q.w;
            headPose.positionX = hmdPose.deviceToAbsoluteTracking[0][3];
            headPose.positionY = hmdPose.deviceToAbsoluteTracking[1][3];
            headPose.positionZ = hmdPose.deviceToAbsoluteTracking[2][3];
            mohw::SetHeadPoseOverride(headPose);
        }

        // The newest pair the GPU has finished writing (the hand-off slots, see g_gameSlots), if it's newer than the one
        // shown; older published pairs it supersedes are freed. Otherwise the pair shown keeps going out with its own
        // stamp. Nothing here waits.
        int take = -1;
        UINT64 takeSeq = 0;
        PairStamp takeStamp{};
        {
            std::lock_guard<std::mutex> lock(g_slotMutex);
            if (OpenSlotsLocked(own))
            {
                const UINT64 completed = own.fence ? own.fence->GetCompletedValue() : ~0ull;
                take = NewestFinishedLocked(completed, own.heldSeq);
                if (take >= 0)
                {
                    g_slotState[take] = kSlotHeld;
                    takeSeq = g_slotSeq[take];
                    takeStamp = g_slotStamp[take];
                    for (int i = 0; i < kSlotCount; ++i) // superseded before they were shown: free again
                        if (g_slotState[i] == kSlotPublished && g_slotSeq[i] < takeSeq)
                            g_slotState[i] = kSlotFree;
                }
                else
                {
                    // Newer pairs exist, none finished yet: the shown one again. Safety valve: if that has gone on for
                    // kStaleTakeMs (a fence that stopped advancing, or a GPU far behind), the newest published pair is
                    // taken anyway -- its Submit then waits on the GPU, as the first design always did.
                    constexpr unsigned long long kStaleTakeMs = 250;
                    int newest = -1;
                    for (int i = 0; i < kSlotCount; ++i)
                        if (g_slotState[i] == kSlotPublished && g_slotSeq[i] > own.heldSeq &&
                            (newest < 0 || g_slotSeq[i] > g_slotSeq[newest]))
                            newest = i;
                    if (newest >= 0)
                    {
                        PerfAdd(kPerfLate, 0);
                        if (GetTickCount64() - own.lastTakeMs > kStaleTakeMs)
                        {
                            take = newest;
                            g_slotState[take] = kSlotHeld;
                            takeSeq = g_slotSeq[take];
                            takeStamp = g_slotStamp[take];
                            for (int i = 0; i < kSlotCount; ++i)
                                if (g_slotState[i] == kSlotPublished && g_slotSeq[i] < takeSeq)
                                    g_slotState[i] = kSlotFree;
                            static long long staleTakes = 0;
                            if (++staleTakes <= 5 || staleTakes % 100 == 0)
                                MOHW_LOG(kLogFile, "submit thread: no finished pair for %llu ms, took the newest anyway "
                                                   "(count %lld)", kStaleTakeMs, staleTakes);
                        }
                    }
                }
            }
        }
        if (take >= 0)
        {
            // S_OK only (WAIT_TIMEOUT is a success code). Free at once: the game let go of it after its copies.
            const bool leftLocked = own.mutex[take][0]->AcquireSync(0, 0) == S_OK;
            const bool rightLocked = leftLocked && own.mutex[take][1]->AcquireSync(0, 0) == S_OK;
            if (leftLocked && !rightLocked)
                own.mutex[take][0]->ReleaseSync(0);
            const int previous = own.held;
            if (rightLocked && previous >= 0)
            {
                own.mutex[previous][1]->ReleaseSync(0);
                own.mutex[previous][0]->ReleaseSync(0);
            }
            std::lock_guard<std::mutex> lock(g_slotMutex);
            const bool sameSet = own.generation == g_slotSetGeneration; // the game may have remade the slots meanwhile
            if (rightLocked)
            {
                if (previous >= 0 && sameSet)
                    g_slotState[previous] = kSlotFree;
                own.held = take;
                own.heldSeq = takeSeq;
                own.heldStamp = takeStamp;
                own.lastTakeMs = GetTickCount64();
                PerfAdd(kPerfShown, 0);
            }
            else
            {
                if (sameSet)
                    g_slotState[take] = kSlotPublished; // still a candidate next loop
                static long long takeFails = 0;
                if (++takeFails <= 5 || takeFails % 1000 == 0)
                    MOHW_LOG(kLogFile, "submit thread: a published pair's lock wasn't free (count %lld)", takeFails);
            }
        }
        if (own.held < 0)
            continue; // nothing published yet: startup, or the game has just remade the slots
        ID3D11Texture2D* ownLeftTex = own.tex[own.held][0];
        ID3D11Texture2D* ownRightTex = own.tex[own.held][1];

        // One stamp for both eyes: SteamVR applies eye 0's pose to both and ignores eye 1's. With pair publish on
        // it's the stamp the pair shown was rendered with; otherwise the old per-eye slot query.
        float sharedStampQuat[4], sharedStampPos[3];
        bool sharedStampPosValid = false;
        bool haveSharedStamp = false;
        if (g_pairPublishEnabled.load(std::memory_order_relaxed))
        {
            haveSharedStamp = hmdPose.poseIsValid && own.heldStamp.valid;
            for (int i = 0; i < 4; ++i)
                sharedStampQuat[i] = own.heldStamp.quat[i];
            for (int i = 0; i < 3; ++i)
                sharedStampPos[i] = own.heldStamp.pos[i];
            sharedStampPosValid = own.heldStamp.posValid;
        }
        else
        {
            haveSharedStamp = hmdPose.poseIsValid && GetPendingRenderPoseStamp(stampFromRightSlot, sharedStampQuat,
                                                                                 sharedStampPos, &sharedStampPosValid);
        }

        if (perturbModeChanged)
        {
            static const char* kPerturbModeNames[3] = {"OFF", "RIGHT eye pose yawed", "LEFT eye pose yawed"};
            MOHW_LOG(kLogFile, "NUMPAD7 pose perturbation test -> mode %d (%s, %.1f deg), pose stamp currently %s",
                      perturbMode, kPerturbModeNames[perturbMode], kPosePerturbYawDeg,
                      haveSharedStamp ? "ACTIVE" : "inactive (perturbed eye falls back to the live pose)");
        }

        auto buildSubmitArgs = [&](ID3D11Texture2D* tex, DirectTextureWithPose* outTexWithPose, int* outFlags,
                                     float yawOffsetDeg) -> const void*
        {
            bool perturb = yawOffsetDeg != 0.0f && hmdPose.poseIsValid;
            if (!haveSharedStamp && !perturb)
            {
                *outFlags = 0; // Submit_Default -- no fresh stamp, use the live pose (WaitGetPoses' own default)
                return nullptr;
            }
            mohw::Quat q = haveSharedStamp
                               ? mohw::Quat{sharedStampQuat[0], sharedStampQuat[1], sharedStampQuat[2], sharedStampQuat[3]}
                               : MatrixToQuat(hmdPose.deviceToAbsoluteTracking);
            if (perturb)
            {
                float halfRad = yawOffsetDeg * (mohw::kPi / 180.0f) * 0.5f;
                mohw::Quat yawQ{0.0f, sinf(halfRad), 0.0f, cosf(halfRad)}; // world-up (tracking space +Y)
                q = mohw::QuatMultiply(yawQ, q);
            }
            float pos[3] = {hmdPose.deviceToAbsoluteTracking[0][3], hmdPose.deviceToAbsoluteTracking[1][3],
                              hmdPose.deviceToAbsoluteTracking[2][3]};
            if (haveSharedStamp && sharedStampPosValid)
            {
                float dx = sharedStampPos[0] - pos[0], dy = sharedStampPos[1] - pos[1], dz = sharedStampPos[2] - pos[2];
                if (sqrtf(dx * dx + dy * dy + dz * dz) < 0.5f) // sanity, same 50cm bound companion used
                {
                    pos[0] += dx;
                    pos[1] += dy;
                    pos[2] += dz;
                }
            }
            outTexWithPose->handle = tex;
            outTexWithPose->eType = 0;
            outTexWithPose->eColorSpace = 0;
            QuatToDeviceMatrix(q, pos, outTexWithPose->mDeviceToAbsoluteTracking);
            *outFlags = kSubmitFlag_TextureWithPose;
            return outTexWithPose;
        };

        static int lastLeftErr = -999, lastRightErr = -999; // only log on change
        DirectTexture leftTex{ownLeftTex, /*TextureType_DirectX*/ 0, /*ColorSpace_Auto*/ 0};
        DirectTexture rightTex{ownRightTex, /*TextureType_DirectX*/ 0, /*ColorSpace_Auto*/ 0};
        DirectTextureWithPose leftTexPosed{}, rightTexPosed{};
        int leftFlags = 0, rightFlags = 0;
        const void* leftPosedPtr =
            buildSubmitArgs(ownLeftTex, &leftTexPosed, &leftFlags, perturbMode == 2 ? kPosePerturbYawDeg : 0.0f);
        const void* rightPosedPtr =
            buildSubmitArgs(ownRightTex, &rightTexPosed, &rightFlags, perturbMode == 1 ? kPosePerturbYawDeg : 0.0f);
        const DirectTexture* leftSubmitTex = leftPosedPtr ? reinterpret_cast<const DirectTexture*>(leftPosedPtr) : &leftTex;
        const DirectTexture* rightSubmitTex = rightPosedPtr ? reinterpret_cast<const DirectTexture*>(rightPosedPtr) : &rightTex;

        // SUBMIT ORDER SWAP TEST (2026-09-25): ghost survived identical L/R pose data (shared-pose test above),
        // ruling out per-eye pose divergence. Testing whether the ghost is actually tied to "whichever eye is
        // submitted SECOND" rather than "the right eye specifically" -- if it jumps to the left eye now, that's
        // a genuine SteamVR submission-order quirk; if it stays on the right eye, order is ruled out too.
        int rightErr = 0, leftErr = 0;
        {
            PerfScope perf(kPerfSubmit); // hooks/perf_probe.h
            rightErr = g_submit(g_compositor, /*Eye_Right*/ 1, rightSubmitTex, nullptr, rightFlags);
            leftErr = g_submit(g_compositor, /*Eye_Left*/ 0, leftSubmitTex, nullptr, leftFlags);
        }
        static bool reportedFirstSubmit = false;
        if (!reportedFirstSubmit)
        {
            reportedFirstSubmit = true;
            MOHW_LOG(StartupLogFile(), "first frame sent to the headset: Submit left %d, right %d (0 = accepted)", leftErr,
                     rightErr);
        }

        if (submitBurstRemaining > 0)
        {
            long long nowUs = QpcMicros();
            long long deltaUs = lastSubmitUs != 0 ? (nowUs - lastSubmitUs) : 0;
            lastSubmitUs = nowUs;
            MOHW_LOG(kLogFile, "SUBMIT BURST %d: usSinceLastLoopIteration=%lld leftErr=%d rightErr=%d",
                      submitBurstRemaining, deltaUs, leftErr, rightErr);
            --submitBurstRemaining;
        }

        if (leftErr != lastLeftErr)
        {
            MOHW_LOG(kLogFile, "Submit(Eye_Left) -> %d", leftErr);
            lastLeftErr = leftErr;
        }
        if (rightErr != lastRightErr)
        {
            MOHW_LOG(kLogFile, "Submit(Eye_Right) -> %d", rightErr);
            lastRightErr = rightErr;
        }
    }
}

// Kicks off the connect attempt, and once connected creates g_ownDevice and starts SubmitThreadProc. Returns true
// only when connected, i.e. when it's worth copying eye textures at all.
bool PrepareForUpdate(ID3D11Device* device)
{
    // The connection is made by the master switch (hooks/master_switch.h) before any hook is installed, so it's
    // normally up by the first call here.
    int state = g_state.load(std::memory_order_acquire);
    if (state != static_cast<int>(ConnectState::Connected))
        return false; // failed -- game keeps rendering normally

    // One-time: create the separate device (see the design comment near g_gameSlots) on the SAME adapter as the
    // game's device, then start SubmitThreadProc. Done here (not in ConnectThreadProc) because finding the
    // matching adapter needs a real game ID3D11Device*, which ConnectThreadProc never has. compare_exchange_strong
    // ensures exactly one caller wins this even though UpdateOpenVrDirect runs every frame.
    bool expected = false;
    if (g_ownDeviceStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice)
        {
            dxgiDevice->GetAdapter(&adapter);
            dxgiDevice->Release();
        }
        ReportGameAdapter(adapter);
        HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                          nullptr, 0, D3D11_SDK_VERSION, &g_ownDevice, nullptr, nullptr);
        if (adapter)
            adapter->Release();
        if (FAILED(hr) || !g_ownDevice)
        {
            MOHW_LOG(kLogFile, "D3D11CreateDevice (own device) FAILED 0x%08lX -- VR submission will not start", hr);
            MOHW_LOG(StartupLogFile(), "the mod's own D3D11 device failed, 0x%08lX: no frames can reach the headset", hr);
        }
        else
        {
            MOHW_LOG(kLogFile, "own D3D11 device created on the game's adapter -- starting SubmitThreadProc");
            std::thread(SubmitThreadProc).detach();
        }
    }
    return true;
}

// The old per-eye publishing (pair publish off, NUMPAD3; the staleness test): each eye's newest frame kept in a staging
// texture on the game's device, and the two handed over through the slots like any pair. Game thread only.
ID3D11Texture2D* g_stagingTex[2] = {};
bool g_stagingFilled[2] = {};

} // namespace

void UpdateOpenVrDirect(ID3D11Device* device, ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye)
{
    if (!PrepareForUpdate(device))
        return;

    // leftEye/rightEye mirror UpdateCompanionEyes' contract: either may be nullptr meaning "this eye didn't render
    // this frame" (alternating-eye's temporal-stereo mode -- see present_hook.cpp's routing): that eye's last frame
    // goes out again with the other eye's new one.
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context)
        return;
    ID3D11Texture2D* eyes[2] = {leftEye, rightEye};
    for (int e = 0; e < 2; ++e)
    {
        if (!eyes[e])
            continue;
        D3D11_TEXTURE2D_DESC desc{}, have{};
        eyes[e]->GetDesc(&desc);
        if (g_stagingTex[e])
            g_stagingTex[e]->GetDesc(&have);
        if (!g_stagingTex[e] || !SameEyeTexture(have, desc) || !TextureOnDevice(g_stagingTex[e], device))
        {
            if (g_stagingTex[e])
                g_stagingTex[e]->Release();
            g_stagingTex[e] = nullptr;
            g_stagingFilled[e] = false;
            D3D11_TEXTURE2D_DESC staging{};
            staging.Width = desc.Width;
            staging.Height = desc.Height;
            staging.MipLevels = 1;
            staging.ArraySize = 1;
            staging.Format = desc.Format;
            staging.SampleDesc.Count = 1;
            staging.Usage = D3D11_USAGE_DEFAULT;
            staging.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&staging, nullptr, &g_stagingTex[e])))
            {
                g_stagingTex[e] = nullptr;
                continue;
            }
        }
        context->CopyResource(g_stagingTex[e], eyes[e]);
        g_stagingFilled[e] = true;
    }
    if (g_stagingFilled[0] && g_stagingFilled[1])
    {
        D3D11_TEXTURE2D_DESC leftDesc{}, rightDesc{};
        g_stagingTex[0]->GetDesc(&leftDesc);
        g_stagingTex[1]->GetDesc(&rightDesc);
        if (EnsureSlots(device, leftDesc, rightDesc))
            PublishPair(context, g_stagingTex[0], g_stagingTex[1], PairStamp{}); // the submit thread queries the stamp
    }
    context->Release();
}

void UpdateOpenVrDirectPair(ID3D11Device* device, ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye,
                            const PairStamp& stamp)
{
    if (!leftEye || !rightEye || !PrepareForUpdate(device))
        return;

    D3D11_TEXTURE2D_DESC leftDesc{}, rightDesc{};
    leftEye->GetDesc(&leftDesc);
    rightEye->GetDesc(&rightDesc);
    if (!EnsureSlots(device, leftDesc, rightDesc))
        return;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context)
        return;
    // One slot holds both eyes and their stamp: SubmitThreadProc takes all of it or none, never half a pair.
    PublishPair(context, leftEye, rightEye, stamp);
    context->Release();
}

bool IsPairPublishEnabled()
{
    return g_pairPublishEnabled.load(std::memory_order_relaxed);
}

bool IsOpenVrDirectConnected()
{
    return g_state.load(std::memory_order_acquire) == static_cast<int>(ConnectState::Connected);
}

ConnectResult TryConnectOpenVrDirect()
{
    if (g_state.load(std::memory_order_acquire) == static_cast<int>(ConnectState::Connected))
        return ConnectResult::Connected;
    return ConnectAttempt();
}

bool GetHandControllerType(bool left, char* out, unsigned size)
{
    constexpr int kGetDeviceForRoleVtableIndex = 18, kGetStringPropertyVtableIndex = 28, kPropControllerType = 7000;
    constexpr int kRoleLeftHand = 1, kRoleRightHand = 2;
    constexpr uint32_t kInvalidDevice = 0xFFFFFFFFu;
    using GetDeviceForRoleFn = uint32_t(__thiscall*)(void*, int);
    using GetStringFn = uint32_t(__thiscall*)(void*, uint32_t, int, char*, uint32_t, int*);
    if (!g_system)
        return false;
    void** vtable = *reinterpret_cast<void***>(g_system);
    uint32_t device = reinterpret_cast<GetDeviceForRoleFn>(vtable[kGetDeviceForRoleVtableIndex])(
        g_system, left ? kRoleLeftHand : kRoleRightHand);
    if (device == kInvalidDevice)
        return false;
    int error = 0;
    uint32_t length = reinterpret_cast<GetStringFn>(vtable[kGetStringPropertyVtableIndex])(
        g_system, device, kPropControllerType, out, size, &error);
    return error == 0 && length > 1;
}

float GetDetectedDisplayFrequencyHz()
{
    return g_detectedDisplayFrequencyHz.load(std::memory_order_relaxed);
}

bool GetRecommendedEyeSize(unsigned* width, unsigned* height)
{
    *width = g_recommendedWidth.load(std::memory_order_relaxed);
    *height = g_recommendedHeight.load(std::memory_order_relaxed);
    return *width != 0 && *height != 0;
}

} // namespace mohw::openvr_direct
