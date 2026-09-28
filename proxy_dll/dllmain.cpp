// Phase 0: injection proof.
//
// Correction vs. the original plan: MSVC's linker does NOT support the
// "EntryName=Module.Function" forwarder syntax in a .def file the way
// MinGW/binutils does -- it was tried and confirmed to fail with LNK2001 on
// both x86 and x64 (the linker demands a real matching symbol, not a forward
// string). The standard, actually-working technique real DXGI proxy mods use
// (ReShade, Special K, ENB-style wrappers) is instead followed here: write
// real typed trampoline functions only for the small set of exports a game
// actually calls (CreateDXGIFactory/1/2 -- confirmed via `dumpbin /exports`
// against the live SysWOW64\dxgi.dll, see docs/dxgi_exports.txt), lazy-load
// the real system DLL by full path, and call straight through. The other 17
// undocumented DXGI exports (DXGID3D10*, PIX*, etc.) are used internally by
// other Microsoft system DLLs loading dxgi.dll from System32/SysWOW64
// directly -- never through a game-directory proxy -- so they don't need
// forwarding here at all. This also removes the earlier plan to ship a
// renamed copy of the system DLL: since we resolve the real dxgi.dll
// ourselves at runtime by full path, there's nothing to rename or install.

#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"
#include "../hooks/present_hook.h"
#include "../hooks/constantbuffer_hook.h"
#include "../hooks/companion_bridge.h"
#include "../hooks/engine_function_hook.h"
#include "../hooks/aiming_controller_hook.h"
#include "../hooks/xinput_hook.h"
#include "../hooks/fov_scale_hook.h"
#include "../hooks/projection_aspect_hook.h"
#include "../hooks/alternating_eye.h"
#include "../hooks/bullet_raycast_redirect_hook.h"
#include "../hooks/fire_candidate_redirect_hook.h"
#include "../hooks/camera_matrix_test_hook.h"
#include "../third_party/minhook/include/MinHook.h"

#include <dxgi.h>
#include <mutex>
#include <thread>

namespace {

constexpr const char* kLogFile = "mohwvr_proxy.log";

using PFN_CreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

HMODULE LoadRealDxgi()
{
    // GetSystemDirectory returns "...\System32" even from a 32-bit process;
    // the OS's WOW64 file-system redirector transparently maps the actual
    // file open to SysWOW64, so no manual path substitution is needed here.
    char sysDir[MAX_PATH]{};
    if (GetSystemDirectoryA(sysDir, MAX_PATH) == 0)
        return nullptr;

    std::string path = std::string(sysDir) + "\\dxgi.dll";
    return LoadLibraryA(path.c_str());
}

HMODULE RealDxgiModule()
{
    static HMODULE hReal = []() {
        HMODULE h = LoadRealDxgi();
        MOHW_LOG(kLogFile, h ? "loaded real dxgi.dll" : "FAILED to load real dxgi.dll");
        return h;
    }();
    return hReal;
}

// Deliberately NOT done from DllMain: MinHook allocates/patches executable
// memory near the target address, and doing that under loader lock (i.e.
// still inside DLL_PROCESS_ATTACH) is a known deadlock risk. Instead this
// runs lazily on first call to one of our exported functions, by which point
// DllMain has long since returned and the loader lock is released -- the
// same reasoning RealDxgiModule() below already follows for lazy-loading
// the real DLL.
void EnsureInitialized()
{
    static std::once_flag once;
    std::call_once(once, [] {
        MOHW_LOG(kLogFile, "proxy dxgi.dll attached to process");

        if (!mohw::ResolveModule())
        {
            MOHW_LOG(kLogFile, "ResolveModule FAILED -- could not find %s module in this process", MOHW_MP);
            return;
        }

        MOHW_LOG(kLogFile, "resolved %s base = 0x%p (delta from capture base 0x%X = 0x%IX)",
                  MOHW_MP, reinterpret_cast<void*>(mohw::g_moduleBase), MOHW_MP_BASE, mohw::g_baseDelta);

        if (!mohw::VerifyBuild())
        {
            MOHW_LOG(kLogFile,
                      "VerifyBuild FAILED -- loaded image size does not match MOHW_MP_SIZE (0x%X). "
                      "Offsets in sdk/ were captured from a different build; do NOT proceed to install hooks.",
                      MOHW_MP_SIZE);
            return;
        }

        MOHW_LOG(kLogFile, "VerifyBuild OK -- image size matches expected build, offsets should be valid");

        // Loads mohwvr_settings.ini (FOV/IPD scale today) before any hook
        // that reads them installs -- see sdk/settings.h.
        mohw::LoadSettings();

        if (!mohw::InstallPresentHook())
            MOHW_LOG(kLogFile, "InstallPresentHook FAILED -- see mohwvr_present.log");

        // RETIRED 2026-09-28: the constant-buffer hook (bone-buffer detection + the bone-hide test) and the
        // engine-function diagnostic hook are no longer installed. The bone-hide test was superseded by the viewmodel
        // instance hide (camera_matrix_test_hook.cpp); what was left was pure overhead -- six hooks on hot D3D calls
        // (Map/Unmap/UpdateSubresource/DrawIndexed/VS+PSSetConstantBuffers), the same kind of whole-frame hook cost
        // that caused the 2026-09-22 ghosting regression -- plus hotkeys colliding with gameplay (End = the holsters'
        // melee key, Delete, Numpad 0/1/2/8/9/+/-/*//). Code kept for reference; the Remove* calls below are no-ops.
        constexpr bool kLegacyBoneHooksEnabled = false;
        if (kLegacyBoneHooksEnabled)
        {
            if (!mohw::InstallConstantBufferHook())
                MOHW_LOG(kLogFile, "InstallConstantBufferHook FAILED -- see mohwvr_cbscan.log");
            if (!mohw::InstallEngineFunctionHook())
                MOHW_LOG(kLogFile, "InstallEngineFunctionHook FAILED -- see mohwvr_enginefn.log");
        }

        if (!mohw::InstallFovScaleHook())
            MOHW_LOG(kLogFile, "InstallFovScaleHook FAILED -- see mohwvr_fovscale.log");

        // Vertical-only FOV for the inner-edge view mode: changes the projection aspect inside the engine's matrix
        // rebuild (see hooks/projection_aspect_hook.h). Inert unless eye_matched_fov.cpp activates it.
        if (!mohw::InstallProjectionAspectHook())
            MOHW_LOG(kLogFile, "InstallProjectionAspectHook FAILED -- see mohwvr_fovscale.log");

        // Head-aim itself defaults OFF (sdk/settings.h, F12 to enable).
        if (!mohw::InstallAimingControllerHook())
            MOHW_LOG(kLogFile, "InstallAimingControllerHook FAILED -- see mohwvr_aimingcontroller.log");

        // VR thumbsticks presented to the game as XInput controller 0 (see hooks/xinput_hook.h).
        if (!mohw::InstallXInputHook())
            MOHW_LOG(kLogFile, "InstallXInputHook FAILED -- see mohwvr_xinput.log");

        // Cosmetic bullet-travel/impact-VFX redirect to the right motion controller's live aim direction --
        // the visual feedback channel alongside InstallFireCandidateRedirectHook's actual hit-scan redirect.
        if (!mohw::InstallBulletRaycastRedirectHook())
            MOHW_LOG(kLogFile, "InstallBulletRaycastRedirectHook FAILED -- see mohwvr_bulletraycast.log");

        // Redirects the real hit-scan/damage raycast to the right motion controller's live aim direction --
        // see hooks/fire_candidate_redirect_hook.cpp and sdk/mohw_offsets.h's OFFSET_FIRECANDIDATE comment.
        if (!mohw::InstallFireCandidateRedirectHook())
            MOHW_LOG(kLogFile, "InstallFireCandidateRedirectHook FAILED -- see mohwvr_firecandidate.log");

        // IsPlayerSkeletonLoaded() (consumed via camera_matrix_test_hook.h) needs this hook installed.
        if (!mohw::InstallCameraMatrixTestHook())
            MOHW_LOG(kLogFile, "InstallCameraMatrixTestHook FAILED -- see mohwvr_cameramatrix.log");

        // Companion-bridge pose/view overrides and openvr_direct's own connection are driven from
        // present_hook.cpp's Present hook instead of here -- they need the game's real D3D11 device and
        // swapchain, which aren't available yet at this point in initialization.
    });
}

} // namespace

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    EnsureInitialized();
    MOHW_LOG(kLogFile, "CreateDXGIFactory called");
    auto real = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(RealDxgiModule(), "CreateDXGIFactory"));
    return real ? real(riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    EnsureInitialized();
    MOHW_LOG(kLogFile, "CreateDXGIFactory1 called");
    auto real = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(RealDxgiModule(), "CreateDXGIFactory1"));
    return real ? real(riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    EnsureInitialized();
    MOHW_LOG(kLogFile, "CreateDXGIFactory2 called");
    auto real = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(RealDxgiModule(), "CreateDXGIFactory2"));
    return real ? real(flags, riid, ppFactory) : E_FAIL;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        mohw::RemovePresentHook();
        mohw::RemoveConstantBufferHook();
        mohw::RemoveEngineFunctionHook();
        mohw::RemoveAimingControllerHook();
        mohw::RemoveXInputHook();
        mohw::RemoveBulletRaycastRedirectHook();
        mohw::RemoveFireCandidateRedirectHook();
        mohw::RemoveCameraMatrixTestHook();
        mohw::RemoveFovScaleHook();
        mohw::RemoveProjectionAspectHook();
        MH_Uninitialize(); // centralized here -- all hook modules share one MinHook instance
        MOHW_LOG(kLogFile, "proxy dxgi.dll detached from process");
        break;
    }
    return TRUE;
}
