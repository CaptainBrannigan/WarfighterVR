#include "present_hook.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "companion_bridge.h"
#include "memory_dump.h"
#include "aiming_controller_hook.h"
#include "fov_scale_hook.h"
#include "alternating_eye.h"
#include "camera_matrix_test_hook.h"
#include "constantbuffer_hook.h"
#include "controller_trigger_hook.h"

#include <windows.h>
#include <d3d11.h>
#include "render_pose_stamp.h"
#include "draw_trace_diag.h"
#include "projection_aspect_hook.h"
#include "eye_matched_fov.h"
#include "../openvr_direct/openvr_direct.h"
#include <atomic>
#include <thread>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_present.log";
constexpr wchar_t kDummyWindowClass[] = L"MohwVrDummyWindow";

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_originalPresent = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
void* g_presentAddress = nullptr;
void* g_resizeBuffersAddress = nullptr;

// FIX (2026-08-23): the left eye's cross-process copy source used to be
// the REAL swap-chain backbuffer directly (self->GetBuffer(0)). Confirmed
// live via paired instrumentation: writes into a shared texture sourced directly from the real swap-chain
// backbuffer skipped far more often than one sourced from an independent render target, since the backbuffer is
// a resource the swap chain machinery is ALSO actively using for the real Present(). Fix: copy the backbuffer
// into our OWN independent, persistent texture first (a plain in-process GPU copy, same device/context), and
// hand THAT onward instead of the backbuffer itself.
ID3D11Texture2D* g_leftCaptureTexture = nullptr;
UINT g_leftCaptureWidth = 0;
UINT g_leftCaptureHeight = 0;
DXGI_FORMAT g_leftCaptureFormat = DXGI_FORMAT_UNKNOWN;

// Recreates g_leftCaptureTexture to match the backbuffer's current size/format if it doesn't already.
bool EnsureLeftCaptureTexture(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& backbufferDesc)
{
    if (g_leftCaptureTexture && g_leftCaptureWidth == backbufferDesc.Width &&
        g_leftCaptureHeight == backbufferDesc.Height && g_leftCaptureFormat == backbufferDesc.Format)
        return true;

    if (g_leftCaptureTexture)
    {
        g_leftCaptureTexture->Release();
        g_leftCaptureTexture = nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = backbufferDesc.Width;
    desc.Height = backbufferDesc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = backbufferDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_leftCaptureTexture);
    if (FAILED(hr))
    {
        MOHW_LOG(kLogFile, "EnsureLeftCaptureTexture: CreateTexture2D(%ux%u fmt=%d) FAILED 0x%08lX", desc.Width,
                  desc.Height, static_cast<int>(desc.Format), hr);
        g_leftCaptureWidth = g_leftCaptureHeight = 0;
        g_leftCaptureFormat = DXGI_FORMAT_UNKNOWN;
        return false;
    }

    g_leftCaptureWidth = desc.Width;
    g_leftCaptureHeight = desc.Height;
    g_leftCaptureFormat = desc.Format;
    MOHW_LOG(kLogFile, "EnsureLeftCaptureTexture: created %ux%u fmt=%d", desc.Width, desc.Height,
              static_cast<int>(desc.Format));
    return true;
}

// PAIR PUBLISH (2026-09-25): holds a left-eye frame until its right-eye partner renders, so both eyes are handed
// over together (see openvr_direct.h's UpdateOpenVrDirectPair). Game thread only.
ID3D11Texture2D* g_pairLeftTexture = nullptr;
bool g_pairLeftStaged = false;

bool EnsurePairLeftTexture(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& sourceDesc)
{
    if (g_pairLeftTexture)
    {
        D3D11_TEXTURE2D_DESC existing{};
        g_pairLeftTexture->GetDesc(&existing);
        if (existing.Width == sourceDesc.Width && existing.Height == sourceDesc.Height &&
            existing.Format == sourceDesc.Format)
            return true;
        g_pairLeftTexture->Release();
        g_pairLeftTexture = nullptr;
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
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_pairLeftTexture);
    if (FAILED(hr))
    {
        MOHW_LOG(kLogFile, "EnsurePairLeftTexture: CreateTexture2D(%ux%u fmt=%d) FAILED 0x%08lX", desc.Width,
                  desc.Height, static_cast<int>(desc.Format), hr);
        g_pairLeftTexture = nullptr;
        return false;
    }
    MOHW_LOG(kLogFile, "EnsurePairLeftTexture: created %ux%u fmt=%d", desc.Width, desc.Height,
              static_cast<int>(desc.Format));
    return true;
}

std::atomic<long long> g_presentCalls{0};
std::atomic<long long> g_resizeBuffersCalls{0};

// LOG MARKER HOTKEY (2026-09-25): F11 (unused by any other hook, confirmed via a full grep of hooks/ -- the old
// draw-duplication stereo debug toggle that used to own this key is retired/uncompiled). Lets the user bound a
// time window in the logs during live testing ("press F11, do the thing, press F11 again, tell Claude what
// happened between marker N and N+1") instead of guessing which stretch of a log capture is relevant. Deliberately
// wired OUTSIDE the disabled polling-table block below so it works regardless of that test's on/off state. Logs
// to its own small file so it's easy to find regardless of which other log a given test is watching.
void CheckLogMarkerHotkey()
{
    static bool wasDown = false;
    static std::atomic<int> counter{0};
    bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (down && !wasDown)
    {
        int n = ++counter;
        MOHW_LOG("mohwvr_marker.log", "===== MARKER #%d =====", n);
    }
    wasDown = down;
}

// DYNAMIC FRAME PACE (2026-09-25): confirmed live this session that Present was running at an uncapped ~200Hz
// against the real 120Hz Steam Link submit target (openvr_direct.cpp's SubmitThreadProc, itself confirmed
// rock-solid and correctly paced) -- a 5:3 ratio with no clean integer relationship, the kind of mismatch that
// produces beat-frequency judder visible only when the underlying pose is actually changing (invisible when
// still, visible during rotation -- matches everything observed chasing this tonight). Paces to 2x the REAL,
// LIVE-DETECTED display frequency (GetDetectedDisplayFrequencyHz, queried via
// IVRSystem::GetFloatTrackedDeviceProperty -- never hardcoded, per explicit request) rather than a fixed assumed
// number: alternating-eye means each eye only gets a fresh render every OTHER Present call, so pacing the
// eye-PAIR rate to 2x the display rate gives each INDIVIDUAL eye a fresh render exactly once per real submit
// interval -- a clean 1:1 relationship instead of an uncontrolled beat pattern. Deliberately a no-op (runs
// uncapped, original behavior) until a real display frequency has actually been detected -- never falls back to
// a hardcoded guess. Single-threaded (game thread only, same thread Present always runs on), so plain statics
// are safe with no atomics needed.
void PaceToDisplayFrequency()
{
    float displayHz = mohw::openvr_direct::GetDetectedDisplayFrequencyHz();
    if (displayHz <= 1.0f)
        return; // not yet detected -- run uncapped rather than guess at a number

    static LARGE_INTEGER freq{};
    static bool haveFreq = false;
    if (!haveFreq)
    {
        QueryPerformanceFrequency(&freq);
        haveFreq = true;
    }

    static LARGE_INTEGER lastPresent{};
    static bool haveLast = false;

    long long targetPeriodTicks = static_cast<long long>(static_cast<double>(freq.QuadPart) / (displayHz * 2.0));

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!haveLast)
    {
        lastPresent = now;
        haveLast = true;
        return;
    }

    long long remainingTicks = targetPeriodTicks - (now.QuadPart - lastPresent.QuadPart);
    if (remainingTicks > 0)
    {
        // Hybrid sleep+spin: Sleep() for the bulk (coarse ~1-15ms granularity, yields the CPU), then spin-wait the
        // final ~2ms for real precision -- Sleep() alone isn't accurate enough at these sub-millisecond targets,
        // spinning the whole remaining time would waste a full CPU core for no reason.
        double remainingMs = static_cast<double>(remainingTicks) * 1000.0 / static_cast<double>(freq.QuadPart);
        if (remainingMs > 2.0)
            Sleep(static_cast<DWORD>(remainingMs - 2.0));
        do
        {
            QueryPerformanceCounter(&now);
        } while (now.QuadPart - lastPresent.QuadPart < targetPeriodTicks);
    }

    lastPresent = now;
}

HRESULT __stdcall Hooked_Present(IDXGISwapChain* self, UINT syncInterval, UINT flags)
{
    PaceToDisplayFrequency();

    long long n = ++g_presentCalls;
    if (n == 1 || n % 300 == 0)
        MOHW_LOG(kLogFile, "Present #%lld self=%p syncInterval=%u flags=%u", n, static_cast<void*>(self),
                  syncInterval, flags);

    // Set below, inside the eye-routing block, to whether THIS frame (the one just captured/rendered) was the
    // left eye -- captured BEFORE AdvanceEyeToNextFrame() flips parity for the NEXT frame, since
    // IsRightEyeActive() itself no longer reflects this frame's eye once that's happened.
    bool presentedFrameWasLeftEye = true;

    // Trigger-as-left-mouse-click (task: "make the controller trigger act
    // as a left mouse input for the time being") -- cheap poll, no-ops if
    // the companion isn't running/publishing yet. See controller_trigger_hook.h.
    UpdateControllerTriggerMouseInput();

    // Always active regardless of the polling-table test below -- see its own declaration comment.
    CheckLogMarkerHotkey();
    // NUMPAD5: arms an every-call burst capture of GetHeadPose staleness -- see companion_bridge.h.
    CheckHeadPoseStalenessBurstHotkey();
    // Numpad .: weapon-to-controller calibrate / off -- see camera_matrix_test_hook.h.
    CheckWeaponDriveHotkey();

    // POLLING TABLE RE-ENABLED (2026-09-25): needed live so the user can toggle head-aim (F12) at will to test
    // mouse-driven rotation with smoothing left on, decoupled from HMD/head-tracking entirely.
    // F9 one-shot: dumps the live (already-decrypted) MOHW.exe module to
    // disk for Ghidra -- see memory_dump.h.
    CheckMemoryDumpHotkey();
    // F10 one-shot: walks the live per-module dispatch array to find the
    // sibling module (camera/player/input) alongside the render module --
    // see memory_dump.h.
    CheckModuleRegistryDumpHotkey();
    // NUMPAD4 toggle: live A/B for render-pose stamping (Submit_TextureWithPose) -- see render_pose_stamp.h.
    CheckRenderPoseStampDebugHotkey();
    // F5,F6 step: live control for the game's internal FOV scale -- see fov_scale_hook.h.
    CheckFovScaleHotkeys();
    // F12 toggle / F1,F2 step / F3 recenter / F4 invert-yaw toggle: live
    // controls for head-driven aim/facing (AimingController+0xC/+0x10) --
    // see aiming_controller_hook.h.
    CheckAimingControllerHotkeys();
    // F7,F8 step: live control for IPD scale -- see alternating_eye.h.
    CheckAlternatingEyeHotkeys();
    // (CheckBoneHideHotkeys retired 2026-09-28 with the constant-buffer hook -- see dllmain.cpp.)

    // Every frame: hand this frame's real backbuffer to the companion (and openvr_direct) via the shared D3D11
    // textures. Called here specifically because by the time Present fires, all of this frame's draws have
    // already happened.
    ID3D11Device* device = nullptr;
    self->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
    if (device)
    {
        ID3D11Texture2D* backbuffer = nullptr;
        self->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
        if (backbuffer)
        {
            DrawTraceOnPresent(device, backbuffer); // diagnostic, see hooks/draw_trace_diag.h

            D3D11_TEXTURE2D_DESC backbufferDesc{};
            backbuffer->GetDesc(&backbufferDesc);

            // Copy into our own independent texture BEFORE anything below touches it as a cross-process source --
            // see EnsureLeftCaptureTexture's comment for why. leftEye (not backbuffer) is what actually goes to
            // UpdateOpenVrDirect.
            ID3D11Texture2D* leftEye = backbuffer;
            ID3D11DeviceContext* immediateContext = nullptr;
            device->GetImmediateContext(&immediateContext);
            if (immediateContext)
            {
                if (EnsureLeftCaptureTexture(device, backbufferDesc))
                {
                    immediateContext->CopyResource(g_leftCaptureTexture, backbuffer);
                    leftEye = g_leftCaptureTexture;
                }
            }

            // Alternating-eye routing (hooks/alternating_eye.h): this frame's single, plain render already
            // carries the correct per-eye position offset (fov_scale_hook.cpp's CommitViewTransform hook called
            // ApplyEyeOffset using whatever parity was current when THIS frame's transform was committed, before
            // this Present call ever ran). Route it to the matching slot and leave the OTHER slot's shared
            // texture untouched (nullptr) -- UpdateOpenVrDirect treats a null source that way, which is exactly
            // temporal stereo's "other eye still shows its last frame" semantics.
            bool rightEye = IsRightEyeActive();
            // PublishRenderPoseForEye must run BEFORE UpdateOpenVrDirect, not after -- UpdateOpenVrDirect's
            // in-process render-pose-stamp lookup (GetPendingRenderPoseStamp) reads whatever this function last
            // published for the ACTIVE eye; the reverse order fed Submit_TextureWithPose a stale pose every frame.
            PublishRenderPoseForEye(rightEye); // EXPERIMENTAL, see hooks/render_pose_stamp.h
            // STALENESS ISOLATION TEST (2026-09-24), RIGHT ARROW: every content/order/index/parity swap tried so
            // far always left the physical right eye (OpenVR index 1) receiving SOME alternating stale/fresh
            // stream -- none of them ever tested "what if that slot is never held stale at all". When armed, the
            // right eye's shared texture gets a fresh CopyResource every single frame (this frame's captured
            // content, whichever real eye it actually belongs to) instead of being left untouched on off-parity
            // frames. This makes the right eye's visual CONTENT wrong about half the time (a real diagnostic
            // cost, expected) but isolates staleness-duration from content-identity as the variable under test.
            // Left eye, game-side rendering (IPD/frustum/pose-stamp), and AdvanceEyeToNextFrame are all untouched.
            bool forceRightAlwaysFresh = IsStalenessIsolationTestActive();
            if (mohw::openvr_direct::IsPairPublishEnabled() && !forceRightAlwaysFresh)
            {
                // PAIR PUBLISH: pair-freeze renders right N+1 with left N's rotation, so (left N, right N+1) is one
                // consistent pair and right N+1's stamp fits both. Hold the left frame, publish both on the right.
                if (!rightEye)
                {
                    g_pairLeftStaged = immediateContext && EnsurePairLeftTexture(device, backbufferDesc);
                    if (g_pairLeftStaged)
                        immediateContext->CopyResource(g_pairLeftTexture, leftEye);
                }
                else if (g_pairLeftStaged)
                {
                    mohw::openvr_direct::PairStamp stamp{};
                    stamp.valid = GetPendingRenderPoseStamp(true, stamp.quat, stamp.pos, &stamp.posValid);
                    mohw::openvr_direct::UpdateOpenVrDirectPair(device, g_pairLeftTexture, leftEye, stamp);
                    g_pairLeftStaged = false;
                }
            }
            else
            {
                ID3D11Texture2D* rightArg = (rightEye || forceRightAlwaysFresh) ? leftEye : nullptr;
                mohw::openvr_direct::UpdateOpenVrDirect(device, rightEye ? nullptr : leftEye, rightArg);
            }
            if (immediateContext)
                immediateContext->Release();
            presentedFrameWasLeftEye = !rightEye; // captured before the flip below -- see its declaration comment
            // Advance parity now, after this frame's capture/routing is done, so it's ready before the NEXT
            // frame's CommitViewTransform fires.
            AdvanceEyeToNextFrame();

            backbuffer->Release();
        }
        device->Release();
    }

    // Skip the REAL desktop swap-chain present on right-eye frames, same technique BF2VR-Alpha's
    // DirectXService.cpp uses ("Only render the left eye on screen because of stereo shake") -- see
    // presentedFrameWasLeftEye's declaration comment for why that captured bool is used here instead of
    // re-querying IsRightEyeActive() (already flipped for next frame by AdvanceEyeToNextFrame() above). The full
    // scene render for this eye has ALREADY happened by the time this hook fires -- skipping the desktop present
    // doesn't skip any of that draw-call/shading cost, only the swap-chain flip/blit and whatever DWM compositor
    // work rides on it.
    if (presentedFrameWasLeftEye)
        return g_originalPresent(self, syncInterval, flags);
    return S_OK;
}

HRESULT __stdcall Hooked_ResizeBuffers(IDXGISwapChain* self, UINT bufferCount, UINT width, UINT height,
                                        DXGI_FORMAT newFormat, UINT swapChainFlags)
{
    long long n = ++g_resizeBuffersCalls;
    MOHW_LOG(kLogFile, "ResizeBuffers #%lld self=%p %ux%u format=%d bufferCount=%u", n, static_cast<void*>(self),
              width, height, static_cast<int>(newFormat), bufferCount);

    return g_originalResizeBuffers(self, bufferCount, width, height, newFormat, swapChainFlags);
}

// Creates a throwaway device+swapchain purely to read the real Present/
// ResizeBuffers vtable slots (indices 8 and 13 in IDXGISwapChain's vtable),
// then tears everything down. The vtable is shared per-driver across every
// swapchain instance of the same kind, so patching it here affects the
// game's own real swapchain too -- we never need to intercept its creation.
bool GetRealSwapChainVtableSlots(void** outPresent, void** outResizeBuffers)
{
    MOHW_LOG(kLogFile, "GetRealSwapChainVtableSlots: creating dummy window");
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
        MOHW_LOG(kLogFile, "GetRealSwapChainVtableSlots: dummy window creation FAILED (err=%lu)", GetLastError());
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

    MOHW_LOG(kLogFile, "GetRealSwapChainVtableSlots: calling D3D11CreateDeviceAndSwapChain");
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context);
    if (FAILED(hr) || !swapChain)
    {
        MOHW_LOG(kLogFile, "GetRealSwapChainVtableSlots: D3D11CreateDeviceAndSwapChain FAILED hr=0x%08lX", hr);
        DestroyWindow(hwnd);
        return false;
    }
    MOHW_LOG(kLogFile, "GetRealSwapChainVtableSlots: dummy device+swapchain created OK, reading vtable");

    void** vtable = *reinterpret_cast<void***>(swapChain);
    *outPresent = vtable[8];        // IDXGISwapChain::Present
    *outResizeBuffers = vtable[13]; // IDXGISwapChain::ResizeBuffers

    swapChain->Release();
    context->Release();
    device->Release();
    DestroyWindow(hwnd);
    return true;
}

// Runs entirely on its own thread, off whatever thread called our proxied
// CreateDXGIFactory/1/2 (almost certainly the game's own thread, mid-way
// through its own DXGI/D3D11 initialization). A first attempt built this
// synchronously into that call and it deadlocked the game -- no log output
// at all appeared, even from failure paths, meaning it hung inside
// D3D11CreateDeviceAndSwapChain itself: creating a second, independent
// device+swapchain reentrantly from inside the game's own in-flight
// DXGI factory call risks contending for an internal DXGI/driver lock the
// outer call already holds. Running on a fresh thread avoids that entirely;
// the real Present hook isn't needed until the game's render loop starts,
// well after its own device/swapchain setup finishes, so there's no rush.
void InstallThreadProc()
{
    void* presentAddr = nullptr;
    void* resizeBuffersAddr = nullptr;
    if (!GetRealSwapChainVtableSlots(&presentAddr, &resizeBuffersAddr))
        return;

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return;
    }

    g_presentAddress = presentAddr;
    g_resizeBuffersAddress = resizeBuffersAddr;

    MH_STATUS s = MH_CreateHook(g_presentAddress, &Hooked_Present, reinterpret_cast<void**>(&g_originalPresent));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(Present @ %p) FAILED: %s", g_presentAddress, MH_StatusToString(s));
        return;
    }

    s = MH_CreateHook(g_resizeBuffersAddress, &Hooked_ResizeBuffers,
                       reinterpret_cast<void**>(&g_originalResizeBuffers));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(ResizeBuffers @ %p) FAILED: %s", g_resizeBuffersAddress,
                  MH_StatusToString(s));
        return;
    }

    s = MH_EnableHook(g_presentAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook(Present) FAILED: %s", MH_StatusToString(s));
        return;
    }

    s = MH_EnableHook(g_resizeBuffersAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook(ResizeBuffers) FAILED: %s", MH_StatusToString(s));
        return;
    }

    MOHW_LOG(kLogFile, "Present hook installed @ %p, ResizeBuffers hook installed @ %p", g_presentAddress,
              g_resizeBuffersAddress);
}

} // namespace

bool InstallPresentHook()
{
    MOHW_LOG(kLogFile, "InstallPresentHook: spawning background thread for dummy-device vtable grab");
    std::thread(&InstallThreadProc).detach();
    return true; // install outcome is logged asynchronously; this just confirms the thread was spawned
}

void RemovePresentHook()
{
    if (g_presentAddress)
    {
        MH_DisableHook(g_presentAddress);
        MH_RemoveHook(g_presentAddress);
        g_presentAddress = nullptr;
    }
    if (g_resizeBuffersAddress)
    {
        MH_DisableHook(g_resizeBuffersAddress);
        MH_RemoveHook(g_resizeBuffersAddress);
        g_resizeBuffersAddress = nullptr;
    }
    // MH_Uninitialize() is called once centrally from dllmain.cpp, not here --
    // multiple hook modules share one MinHook instance.
}

} // namespace mohw
