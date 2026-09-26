#include "openvr_direct.h"
#include "openvr_types.h"
#include "vr_input.h"

#include "../sdk/logging.h"
#include "../sdk/vr_math.h"
#include "../hooks/companion_bridge.h"
#include "../hooks/render_pose_stamp.h"
#include "../shared/ipc_protocol.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

namespace mohw::openvr_direct {
namespace {

constexpr const char* kLogFile = "mohwvr_openvr_direct.log";

// Minimal OpenVR C-ABI surface, loaded dynamically -- see openvr_direct.h's top comment for why (DRM).
using PFN_VR_IsHmdPresent = bool(__cdecl*)();
using PFN_VR_InitInternal2 = uint32_t(__cdecl*)(int* peError, int eApplicationType, const char* pStartupInfo);
using PFN_VR_GetGenericInterface = void*(__cdecl*)(const char* pchInterfaceVersion, int* peError);
using PFN_VR_IsInterfaceVersionValid = bool(__cdecl*)(const char* pchInterfaceVersion);

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
// (g_ownDevice below) is created on the SAME adapter as the game's device. The game thread creates its eye
// textures with D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX and does CopyResource into them; a dedicated thread that
// owns g_ownDevice exclusively opens those same textures via OpenSharedResource (standard cross-device GPU
// resource sharing) and performs the paired WaitGetPoses+Submit sequence entirely on its own device/thread.
ID3D11Texture2D* g_leftTex = nullptr;
ID3D11Texture2D* g_rightTex = nullptr;
UINT g_leftWidth = 0, g_leftHeight = 0;
DXGI_FORMAT g_leftFormat = DXGI_FORMAT_UNKNOWN;
UINT g_rightWidth = 0, g_rightHeight = 0;
DXGI_FORMAT g_rightFormat = DXGI_FORMAT_UNKNOWN;

// Published by the game thread (EnsurePersistentEyeTexture) whenever it (re)creates a shared eye texture;
// consumed by SubmitThreadProc, which re-opens the texture on g_ownDevice whenever the generation changes.
// HANDLE from IDXGIResource::GetSharedHandle is a "weak" reference tied to the source texture's lifetime --
// no separate CloseHandle needed, unlike the newer NT-handle sharing API.
std::atomic<HANDLE> g_leftSharedHandle{nullptr};
std::atomic<HANDLE> g_rightSharedHandle{nullptr};
std::atomic<int> g_leftGeneration{0};
std::atomic<int> g_rightGeneration{0};

// IDXGIKeyedMutex gives real GPU-level mutual exclusion between the game device's CopyResource writes and
// g_ownDevice's Submit reads of the same shared texture -- D3D11_RESOURCE_MISC_SHARED alone allowed a genuine
// torn-frame race between the two independent devices. Used as a plain lock (both sides always
// Acquire(0)/Release(0), not a producer/consumer ping-pong key) since Submit must run for BOTH eyes every loop
// iteration regardless of whether that eye rendered anything new this time (the other eye resubmits its last
// content under alternating-eye). Game-thread-only pointers, created alongside g_leftTex/g_rightTex.
IDXGIKeyedMutex* g_leftKeyedMutexGame = nullptr;
IDXGIKeyedMutex* g_rightKeyedMutexGame = nullptr;
constexpr DWORD kKeyedMutexTimeoutMs = 5; // bounded on purpose -- never let either side risk a real stall over this

// Written by UpdateOpenVrDirectPair while it holds both keyed mutexes; read by SubmitThreadProc while it holds both.
std::mutex g_pairStampMutex;
PairStamp g_pairStamp;
std::atomic<bool> g_pairPublishEnabled{true};

// The second, independent D3D11 device (see the design comment near g_leftTex) -- created once, lazily, the first
// time UpdateOpenVrDirect runs with a real game device available (needed to find the matching adapter). Written
// once on the game thread strictly BEFORE SubmitThreadProc is started (std::thread's constructor establishes a
// happens-before relationship for everything written before it), then read-only for the rest of the process's
// life -- no atomics needed for g_ownDevice itself.
ID3D11Device* g_ownDevice = nullptr;
std::atomic<bool> g_ownDeviceStarted{false};

bool EnsurePersistentEyeTexture(ID3D11Device* device, ID3D11Texture2D*& tex, UINT& cachedWidth, UINT& cachedHeight,
                                  DXGI_FORMAT& cachedFormat, const D3D11_TEXTURE2D_DESC& sourceDesc,
                                  std::atomic<HANDLE>& outSharedHandle, std::atomic<int>& outGeneration,
                                  IDXGIKeyedMutex*& outKeyedMutex)
{
    if (tex && cachedWidth == sourceDesc.Width && cachedHeight == sourceDesc.Height && cachedFormat == sourceDesc.Format)
        return true;

    if (tex)
    {
        tex->Release();
        tex = nullptr;
    }
    if (outKeyedMutex)
    {
        outKeyedMutex->Release();
        outKeyedMutex = nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = sourceDesc.Width;
    desc.Height = sourceDesc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = sourceDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // KEYEDMUTEX instead of plain SHARED -- the two flags are mutually exclusive per the D3D11 docs. Gives real
    // GPU-level mutual exclusion between the game's device and g_ownDevice's access to this same resource.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
    if (FAILED(hr))
    {
        MOHW_LOG(kLogFile, "EnsurePersistentEyeTexture: CreateTexture2D(%ux%u fmt=%d) FAILED 0x%08lX", desc.Width, desc.Height,
                  static_cast<int>(desc.Format), hr);
        cachedWidth = cachedHeight = 0;
        cachedFormat = DXGI_FORMAT_UNKNOWN;
        return false;
    }

    cachedWidth = desc.Width;
    cachedHeight = desc.Height;
    cachedFormat = desc.Format;

    if (FAILED(tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&outKeyedMutex))))
    {
        MOHW_LOG(kLogFile, "EnsurePersistentEyeTexture: QueryInterface(IDXGIKeyedMutex) FAILED");
        outKeyedMutex = nullptr;
    }

    IDXGIResource* dxgiRes = nullptr;
    if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgiRes))) && dxgiRes)
    {
        HANDLE sharedHandle = nullptr;
        if (SUCCEEDED(dxgiRes->GetSharedHandle(&sharedHandle)) && sharedHandle)
        {
            outSharedHandle.store(sharedHandle, std::memory_order_relaxed);
            outGeneration.fetch_add(1, std::memory_order_release); // release: pairs with SubmitThreadProc's acquire-load
        }
        else
        {
            MOHW_LOG(kLogFile, "EnsurePersistentEyeTexture: GetSharedHandle FAILED");
        }
        dxgiRes->Release();
    }
    else
    {
        MOHW_LOG(kLogFile, "EnsurePersistentEyeTexture: QueryInterface(IDXGIResource) FAILED");
    }

    return true;
}

void ConnectThreadProc()
{
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\')
        --n;
    path[n] = 0;
    strcat_s(path, "openvr_api.dll"); // the game's own directory -- where the SteamVR-current copy was placed

    MOHW_LOG(kLogFile, "loading %s ...", path);
    HMODULE h = LoadLibraryA(path);
    if (!h)
    {
        MOHW_LOG(kLogFile, "LoadLibraryA FAILED, GetLastError=%lu", GetLastError());
        g_state.store(static_cast<int>(ConnectState::Failed), std::memory_order_release);
        return;
    }

    auto isHmdPresent = reinterpret_cast<PFN_VR_IsHmdPresent>(GetProcAddress(h, "VR_IsHmdPresent"));
    auto initInternal2 = reinterpret_cast<PFN_VR_InitInternal2>(GetProcAddress(h, "VR_InitInternal2"));
    auto getGenericInterface = reinterpret_cast<PFN_VR_GetGenericInterface>(GetProcAddress(h, "VR_GetGenericInterface"));
    auto isInterfaceVersionValid =
        reinterpret_cast<PFN_VR_IsInterfaceVersionValid>(GetProcAddress(h, "VR_IsInterfaceVersionValid"));
    if (!isHmdPresent || !initInternal2 || !getGenericInterface || !isInterfaceVersionValid)
    {
        MOHW_LOG(kLogFile, "GetProcAddress FAILED for one or more exports");
        g_state.store(static_cast<int>(ConnectState::Failed), std::memory_order_release);
        return;
    }

    bool hmdPresent = isHmdPresent();
    MOHW_LOG(kLogFile, "VR_IsHmdPresent() = %d", hmdPresent);
    if (!hmdPresent)
    {
        // Skip VR_InitInternal2 entirely when no headset present -- confirmed live it starts/talks to SteamVR
        // regardless (standard OpenVR behavior), which stalled the game well before the main menu with SteamVR
        // running headless. Every eye-matched/true-frustum code path already degrades gracefully when
        // GetHmdView() never returns real data, same as it does before this thread finishes either way.
        MOHW_LOG(kLogFile, "no HMD present -- skipping VR_InitInternal2 entirely, game renders normally");
        g_state.store(static_cast<int>(ConnectState::Failed), std::memory_order_release);
        return;
    }

    MOHW_LOG(kLogFile,
              "calling VR_InitInternal2() -- confirmed to sometimes hang indefinitely; this is a background thread "
              "specifically so that can never freeze the game...");
    int err = kInitError_None;
    uint32_t token = initInternal2(&err, kAppType_Scene, nullptr);
    MOHW_LOG(kLogFile, "VR_InitInternal2() returned token=%u err=%d", token, err);
    if (err != kInitError_None)
    {
        MOHW_LOG(kLogFile, "init FAILED, err=%d -- staying disconnected, game renders normally", err);
        g_state.store(static_cast<int>(ConnectState::Failed), std::memory_order_release);
        return;
    }

    bool ifaceOk = isInterfaceVersionValid("IVRSystem_026");
    MOHW_LOG(kLogFile, "IVRSystem_026 valid = %d", ifaceOk);

    // Real per-eye HMD frustum, published into the SAME HmdViewBlock hooks/eye_matched_fov.cpp already knows how
    // to consume (true per-eye frustum, no crop).
    int sysErr = kInitError_None;
    void* system = getGenericInterface("IVRSystem_026", &sysErr);
    MOHW_LOG(kLogFile, "IVRSystem_026 = %p, err=%d", system, sysErr);
    if (system)
    {
        void** sysVtable = *reinterpret_cast<void***>(system);
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
        g_state.store(static_cast<int>(ConnectState::Failed), std::memory_order_release);
        return;
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
        MOHW_LOG(kLogFile, "SteamVR Input setup FAILED -- continuing without controllers (see mohwvr_vrinput.log)");

    MOHW_LOG(kLogFile, "CONNECTED -- game thread will start the separate-device SubmitThreadProc on its first call");
    g_state.store(static_cast<int>(ConnectState::Connected), std::memory_order_release);
}

// Re-opens tex (and its keyed mutex) on g_ownDevice from sharedHandle whenever generationAtomic has moved
// past lastOpenedGen (a resize on the game thread bumps the generation -- see EnsurePersistentEyeTexture).
// Returns false if no shared handle has been published yet at all (startup grace period) or
// OpenSharedResource/QueryInterface fails.
bool EnsureOwnOpenedTexture(ID3D11Texture2D*& openedTex, IDXGIKeyedMutex*& openedMutex, int& lastOpenedGen,
                              std::atomic<HANDLE>& sharedHandleAtomic, std::atomic<int>& generationAtomic)
{
    int currentGen = generationAtomic.load(std::memory_order_acquire); // acquire: pairs with the publisher's release-store
    if (openedTex && currentGen == lastOpenedGen)
        return true;

    HANDLE handle = sharedHandleAtomic.load(std::memory_order_relaxed);
    if (!handle)
        return false;

    if (openedMutex)
    {
        openedMutex->Release();
        openedMutex = nullptr;
    }
    if (openedTex)
    {
        openedTex->Release();
        openedTex = nullptr;
    }

    HRESULT hr = g_ownDevice->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&openedTex));
    if (FAILED(hr))
    {
        MOHW_LOG(kLogFile, "EnsureOwnOpenedTexture: OpenSharedResource FAILED 0x%08lX", hr);
        lastOpenedGen = -1;
        return false;
    }
    if (FAILED(openedTex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&openedMutex))))
    {
        MOHW_LOG(kLogFile, "EnsureOwnOpenedTexture: QueryInterface(IDXGIKeyedMutex) FAILED");
        openedMutex = nullptr;
        openedTex->Release();
        openedTex = nullptr;
        lastOpenedGen = -1;
        return false;
    }
    lastOpenedGen = currentGen;
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

// Owns g_ownDevice exclusively for the rest of the process's life -- see the design comment near g_leftTex for
// why this exists. WaitGetPoses and Submit are called together, paired, right here, on this dedicated thread.
void SubmitThreadProc()
{
    ID3D11Texture2D* ownLeftTex = nullptr;
    ID3D11Texture2D* ownRightTex = nullptr;
    IDXGIKeyedMutex* ownLeftMutex = nullptr;
    IDXGIKeyedMutex* ownRightMutex = nullptr;
    int leftOpenedGen = -1;
    int rightOpenedGen = -1;

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
        bool submitBurstKeyDown = (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
        if (submitBurstKeyDown && !submitBurstKeyWasDown)
        {
            submitBurstRemaining = 300;
            MOHW_LOG(kLogFile, "===== SUBMIT BURST ARMED: logging next 300 loop iterations unconditionally =====");
        }
        submitBurstKeyWasDown = submitBurstKeyDown;

        bool perturbKeyDown = (GetAsyncKeyState(VK_NUMPAD7) & 0x8000) != 0;
        bool perturbModeChanged = perturbKeyDown && !perturbKeyWasDown;
        if (perturbModeChanged)
            perturbMode = (perturbMode + 1) % 3;
        perturbKeyWasDown = perturbKeyDown;

        bool stampFlipKeyDown = (GetAsyncKeyState(VK_NUMPAD6) & 0x8000) != 0;
        if (stampFlipKeyDown && !stampFlipKeyWasDown)
        {
            stampFromRightSlot = !stampFromRightSlot;
            MOHW_LOG(kLogFile, "NUMPAD6 stamp slot flip test -> shared stamp now taken from the %s eye's slot",
                      stampFromRightSlot ? "RIGHT" : "LEFT (normal)");
        }
        stampFlipKeyWasDown = stampFlipKeyDown;

        bool pairKeyDown = (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) != 0;
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

        bool haveLeft = EnsureOwnOpenedTexture(ownLeftTex, ownLeftMutex, leftOpenedGen, g_leftSharedHandle, g_leftGeneration);
        bool haveRight = EnsureOwnOpenedTexture(ownRightTex, ownRightMutex, rightOpenedGen, g_rightSharedHandle, g_rightGeneration);
        if (!haveLeft || !haveRight)
            continue; // startup grace period -- wait until both eyes have published a shared texture at least once

        // AcquireSync/ReleaseSync around each eye's Submit: real GPU-level mutual exclusion against the game
        // thread's CopyResource into the SAME shared texture -- see g_leftKeyedMutexGame's declaration comment.
        // Used as a plain lock rather than a producer/consumer ping-pong key, because Submit must still run for
        // BOTH eyes every single loop iteration regardless of whether that eye rendered anything new this time.
        // A short, bounded timeout means a rare failure to acquire degrades to "submit anyway, log it" rather
        // than ever risking a stall on this thread -- skipping Submit entirely would violate the
        // WaitGetPoses/Submit pairing this whole design exists to protect. Taken BEFORE reading the pair stamp, so
        // the stamp and both textures always come from the same published pair.
        static long long leftAcquireFails = 0, rightAcquireFails = 0;
        bool leftLocked = ownLeftMutex && SUCCEEDED(ownLeftMutex->AcquireSync(0, kKeyedMutexTimeoutMs));
        if (!leftLocked && (++leftAcquireFails <= 5 || leftAcquireFails % 1000 == 0))
            MOHW_LOG(kLogFile, "left eye AcquireSync timed out (count=%lld) -- submitting anyway", leftAcquireFails);
        bool rightLocked = ownRightMutex && SUCCEEDED(ownRightMutex->AcquireSync(0, kKeyedMutexTimeoutMs));
        if (!rightLocked && (++rightAcquireFails <= 5 || rightAcquireFails % 1000 == 0))
            MOHW_LOG(kLogFile, "right eye AcquireSync timed out (count=%lld) -- submitting anyway", rightAcquireFails);

        // One stamp for both eyes: SteamVR applies eye 0's pose to both and ignores eye 1's. With pair publish on
        // it's the stamp the current pair was rendered with; otherwise the old per-eye slot query.
        float sharedStampQuat[4], sharedStampPos[3];
        bool sharedStampPosValid = false;
        bool haveSharedStamp = false;
        if (g_pairPublishEnabled.load(std::memory_order_relaxed))
        {
            std::lock_guard<std::mutex> lock(g_pairStampMutex);
            haveSharedStamp = hmdPose.poseIsValid && g_pairStamp.valid;
            for (int i = 0; i < 4; ++i)
                sharedStampQuat[i] = g_pairStamp.quat[i];
            for (int i = 0; i < 3; ++i)
                sharedStampPos[i] = g_pairStamp.pos[i];
            sharedStampPosValid = g_pairStamp.posValid;
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
        int rightErr = g_submit(g_compositor, /*Eye_Right*/ 1, rightSubmitTex, nullptr, rightFlags);
        int leftErr = g_submit(g_compositor, /*Eye_Left*/ 0, leftSubmitTex, nullptr, leftFlags);

        if (submitBurstRemaining > 0)
        {
            long long nowUs = QpcMicros();
            long long deltaUs = lastSubmitUs != 0 ? (nowUs - lastSubmitUs) : 0;
            lastSubmitUs = nowUs;
            MOHW_LOG(kLogFile, "SUBMIT BURST %d: usSinceLastLoopIteration=%lld leftErr=%d rightErr=%d",
                      submitBurstRemaining, deltaUs, leftErr, rightErr);
            --submitBurstRemaining;
        }

        if (leftLocked)
            ownLeftMutex->ReleaseSync(0);
        if (rightLocked)
            ownRightMutex->ReleaseSync(0);

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
    int state = g_state.load(std::memory_order_acquire);
    if (state == static_cast<int>(ConnectState::NotStarted))
    {
        g_state.store(static_cast<int>(ConnectState::Connecting), std::memory_order_relaxed);
        MOHW_LOG(kLogFile, "starting background connect attempt");
        std::thread(ConnectThreadProc).detach();
        return false;
    }
    if (state != static_cast<int>(ConnectState::Connected))
        return false; // still connecting or failed -- game keeps rendering normally either way

    // One-time: create the separate device (see the design comment near g_leftTex) on the SAME adapter as the
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
        HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                          nullptr, 0, D3D11_SDK_VERSION, &g_ownDevice, nullptr, nullptr);
        if (adapter)
            adapter->Release();
        if (FAILED(hr) || !g_ownDevice)
        {
            MOHW_LOG(kLogFile, "D3D11CreateDevice (own device) FAILED 0x%08lX -- VR submission will not start", hr);
        }
        else
        {
            MOHW_LOG(kLogFile, "own D3D11 device created on the game's adapter -- starting SubmitThreadProc");
            std::thread(SubmitThreadProc).detach();
        }
    }
    return true;
}

} // namespace

void UpdateOpenVrDirect(ID3D11Device* device, ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye)
{
    if (!PrepareForUpdate(device))
        return;

    // leftEye/rightEye mirror UpdateCompanionEyes' contract: either may be nullptr meaning "this eye didn't render
    // this frame" (alternating-eye's temporal-stereo mode -- see present_hook.cpp's routing). Only CopyResource
    // into our own shared textures happens here -- WaitGetPoses/Submit are entirely on SubmitThreadProc (see its
    // own comment), which reads these via OpenSharedResource on its own separate device.
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context)
        return;

    // AcquireSync/ReleaseSync around each CopyResource: pairs with SubmitThreadProc's own Acquire/Release around
    // Submit -- see g_leftKeyedMutexGame's declaration comment. Short, bounded timeout: if the submit thread is
    // unusually slow to release (rare), skip this frame's copy rather than stall the game's own render thread --
    // the eye just keeps showing its last successfully-copied content.
    if (leftEye)
    {
        D3D11_TEXTURE2D_DESC desc{};
        leftEye->GetDesc(&desc);
        if (EnsurePersistentEyeTexture(device, g_leftTex, g_leftWidth, g_leftHeight, g_leftFormat, desc, g_leftSharedHandle,
                                          g_leftGeneration, g_leftKeyedMutexGame))
        {
            bool locked = g_leftKeyedMutexGame && SUCCEEDED(g_leftKeyedMutexGame->AcquireSync(0, kKeyedMutexTimeoutMs));
            if (locked || !g_leftKeyedMutexGame)
            {
                context->CopyResource(g_leftTex, leftEye);
                if (locked)
                    g_leftKeyedMutexGame->ReleaseSync(0);
            }
        }
    }
    if (rightEye)
    {
        D3D11_TEXTURE2D_DESC desc{};
        rightEye->GetDesc(&desc);
        if (EnsurePersistentEyeTexture(device, g_rightTex, g_rightWidth, g_rightHeight, g_rightFormat, desc,
                                          g_rightSharedHandle, g_rightGeneration, g_rightKeyedMutexGame))
        {
            bool locked = g_rightKeyedMutexGame && SUCCEEDED(g_rightKeyedMutexGame->AcquireSync(0, kKeyedMutexTimeoutMs));
            if (locked || !g_rightKeyedMutexGame)
            {
                context->CopyResource(g_rightTex, rightEye);
                if (locked)
                    g_rightKeyedMutexGame->ReleaseSync(0);
            }
        }
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
    if (!EnsurePersistentEyeTexture(device, g_leftTex, g_leftWidth, g_leftHeight, g_leftFormat, leftDesc,
                                    g_leftSharedHandle, g_leftGeneration, g_leftKeyedMutexGame) ||
        !EnsurePersistentEyeTexture(device, g_rightTex, g_rightWidth, g_rightHeight, g_rightFormat, rightDesc,
                                    g_rightSharedHandle, g_rightGeneration, g_rightKeyedMutexGame))
        return;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context)
        return;

    // Same lock order as SubmitThreadProc (left, then right). If either can't be taken, skip the whole pair: the
    // submitter keeps showing the previous, still-consistent pair rather than a half-updated one.
    bool leftAcquired = g_leftKeyedMutexGame && SUCCEEDED(g_leftKeyedMutexGame->AcquireSync(0, kKeyedMutexTimeoutMs));
    bool rightAcquired = (leftAcquired || !g_leftKeyedMutexGame) && g_rightKeyedMutexGame &&
                         SUCCEEDED(g_rightKeyedMutexGame->AcquireSync(0, kKeyedMutexTimeoutMs));
    bool leftOk = leftAcquired || !g_leftKeyedMutexGame;
    bool rightOk = rightAcquired || !g_rightKeyedMutexGame;
    if (leftOk && rightOk)
    {
        context->CopyResource(g_leftTex, leftEye);
        context->CopyResource(g_rightTex, rightEye);
        std::lock_guard<std::mutex> lock(g_pairStampMutex);
        g_pairStamp = stamp;
    }
    else
    {
        static long long skipped = 0;
        if (++skipped <= 5 || skipped % 1000 == 0)
            MOHW_LOG(kLogFile, "pair publish skipped, keyed mutex busy (count=%lld)", skipped);
    }
    if (rightAcquired)
        g_rightKeyedMutexGame->ReleaseSync(0);
    if (leftAcquired)
        g_leftKeyedMutexGame->ReleaseSync(0);
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

float GetDetectedDisplayFrequencyHz()
{
    return g_detectedDisplayFrequencyHz.load(std::memory_order_relaxed);
}

} // namespace mohw::openvr_direct
