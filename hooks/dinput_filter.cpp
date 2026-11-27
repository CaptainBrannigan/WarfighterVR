#include "dinput_filter.h"

#include "master_switch.h"
#include "startup_diag.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"
#include "../third_party/minhook/include/MinHook.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h> // types and interfaces only: nothing from dinput8 is linked, the DLL is loaded at run time

#include <atomic>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_dinput.log";
constexpr int kFullLogEnumerations = 4;           // the game's first enumerations are logged device by device
constexpr unsigned long long kVrCheckMs = 2000;   // how often whether SteamVR is running is checked again

// Not dxguid.lib's: the DLL links nothing beyond d3d11 / user32 / kernel32 / gdi32.
const GUID kIidDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kIidDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};

// IDirectInput8 vtable: 0 QueryInterface, 1 AddRef, 2 Release, 3 CreateDevice, 4 EnumDevices.
constexpr int kVtCreateDevice = 3, kVtEnumDevices = 4;

using EnumDevicesAFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
using EnumDevicesWFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8W*, DWORD, LPDIENUMDEVICESCALLBACKW, LPVOID, DWORD);
using CreateDeviceAFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using CreateDeviceWFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8W*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
EnumDevicesAFn g_enumA = nullptr;
EnumDevicesWFn g_enumW = nullptr;
CreateDeviceAFn g_createA = nullptr;
CreateDeviceWFn g_createW = nullptr;

std::atomic<int> g_enumerations{0};
// Inside an EnumDevices on this thread: DirectInput creates every device itself to describe it (live in the test, 2026-11-27:
// the CreateDevice hook refused them all mid-enumeration), so CreateDevice is left alone then; the callback filters.
thread_local int g_inEnumeration = 0;
std::atomic<int> g_refusals{0};

// A game controller: what DirectInput enumerates under DI8DEVCLASS_GAMECTRL, plus pedals and other supplemental
// controls. The keyboard, the mouse, DI8DEVTYPE_DEVICE and the rest stay.
bool IsGameController(DWORD devType)
{
    switch (GET_DIDEVICE_TYPE(devType))
    {
    case DI8DEVTYPE_JOYSTICK:
    case DI8DEVTYPE_GAMEPAD:
    case DI8DEVTYPE_DRIVING:
    case DI8DEVTYPE_FLIGHT:
    case DI8DEVTYPE_1STPERSON:
    case DI8DEVTYPE_SUPPLEMENTAL:
        return true;
    default:
        return false;
    }
}

const char* TypeName(DWORD devType)
{
    switch (GET_DIDEVICE_TYPE(devType))
    {
    case DI8DEVTYPE_DEVICE: return "device";
    case DI8DEVTYPE_MOUSE: return "mouse";
    case DI8DEVTYPE_KEYBOARD: return "keyboard";
    case DI8DEVTYPE_JOYSTICK: return "joystick";
    case DI8DEVTYPE_GAMEPAD: return "gamepad";
    case DI8DEVTYPE_DRIVING: return "wheel";
    case DI8DEVTYPE_FLIGHT: return "flight stick";
    case DI8DEVTYPE_1STPERSON: return "first-person controller";
    case DI8DEVTYPE_DEVICECTRL: return "device control";
    case DI8DEVTYPE_SCREENPOINTER: return "screen pointer";
    case DI8DEVTYPE_REMOTE: return "remote";
    case DI8DEVTYPE_SUPPLEMENTAL: return "supplemental (pedals etc.)";
    default: return "other";
    }
}

// Whether game controllers are hidden now: the setting, and SteamVR running or the mod on (otherwise the game runs
// unmodified, as everywhere else in the mod). SteamVR's process is looked for at most every kVrCheckMs.
bool Hiding()
{
    if (!GetHideOtherGameControllers())
        return false;
    if (IsModActive())
        return true;
    static std::atomic<unsigned long long> nextCheckMs{0};
    static std::atomic<bool> steamVr{false};
    const unsigned long long now = GetTickCount64();
    if (now >= nextCheckMs.load(std::memory_order_relaxed))
    {
        steamVr.store(IsProcessRunning(L"vrserver.exe"), std::memory_order_relaxed);
        nextCheckMs.store(now + kVrCheckMs, std::memory_order_relaxed);
    }
    return steamVr.load(std::memory_order_relaxed);
}

void ToUtf8(const wchar_t* in, char* out, int size)
{
    if (WideCharToMultiByte(CP_UTF8, 0, in, -1, out, size, nullptr, nullptr) == 0 && size > 0)
        out[0] = '\0';
}

// One enumeration's wrapping of the game's callback.
template <typename Callback>
struct EnumContext
{
    Callback callback;
    LPVOID ref;
    bool hide;
    bool log;
    int number;
    int shown, hidden;
};

void LogDevice(const EnumContext<LPDIENUMDEVICESCALLBACKW>& c, const char* name, DWORD devType, bool hidden)
{
    MOHW_LOG(kLogFile, "  enumeration %d: %s \"%s\" (type 0x%02X)%s", c.number, TypeName(devType), name,
             GET_DIDEVICE_TYPE(devType), hidden ? " -- HIDDEN from the game" : "");
}

BOOL CALLBACK FilterCallbackW(LPCDIDEVICEINSTANCEW device, LPVOID ref)
{
    auto* c = static_cast<EnumContext<LPDIENUMDEVICESCALLBACKW>*>(ref);
    const bool hide = c->hide && IsGameController(device->dwDevType);
    if (c->log)
    {
        char name[MAX_PATH];
        ToUtf8(device->tszInstanceName, name, sizeof(name));
        LogDevice(*c, name, device->dwDevType, hide);
    }
    if (hide)
    {
        ++c->hidden;
        return DIENUM_CONTINUE;
    }
    ++c->shown;
    return c->callback(device, c->ref);
}

BOOL CALLBACK FilterCallbackA(LPCDIDEVICEINSTANCEA device, LPVOID ref)
{
    auto* c = static_cast<EnumContext<LPDIENUMDEVICESCALLBACKA>*>(ref);
    const bool hide = c->hide && IsGameController(device->dwDevType);
    if (c->log)
        MOHW_LOG(kLogFile, "  enumeration %d: %s \"%s\" (type 0x%02X)%s", c->number, TypeName(device->dwDevType),
                 device->tszInstanceName, GET_DIDEVICE_TYPE(device->dwDevType), hide ? " -- HIDDEN from the game" : "");
    if (hide)
    {
        ++c->hidden;
        return DIENUM_CONTINUE;
    }
    ++c->shown;
    return c->callback(device, c->ref);
}

// Every enumeration is counted; the first few are logged in full, later ones only when something was hidden, at most
// once a minute.
bool BeginEnumeration(int* number, DWORD devType, DWORD flags, bool hide, bool wide)
{
    *number = g_enumerations.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool full = *number <= kFullLogEnumerations;
    if (full)
        MOHW_LOG(kLogFile, "game enumeration %d (%s): class/type 0x%X, flags 0x%X; game controllers %s", *number,
                 wide ? "W" : "A", devType, flags, hide ? "HIDDEN" : "shown");
    return full;
}

void EndEnumeration(int number, bool full, int shown, int hidden, HRESULT hr)
{
    static unsigned long long nextSummaryMs = 0;
    const unsigned long long now = GetTickCount64();
    if (full || (hidden > 0 && now >= nextSummaryMs))
    {
        MOHW_LOG(kLogFile, "game enumeration %d: %d device(s) given to the game, %d game controller(s) hidden (hr 0x%08lX)",
                 number, shown, hidden, static_cast<unsigned long>(hr));
        if (!full)
            nextSummaryMs = now + 60000;
    }
}

HRESULT STDMETHODCALLTYPE HookedEnumDevicesW(IDirectInput8W* self, DWORD devType, LPDIENUMDEVICESCALLBACKW callback,
                                             LPVOID ref, DWORD flags)
{
    if (!callback)
        return g_enumW(self, devType, callback, ref, flags);
    EnumContext<LPDIENUMDEVICESCALLBACKW> c{callback, ref, Hiding(), false, 0, 0, 0};
    c.log = BeginEnumeration(&c.number, devType, flags, c.hide, true);
    ++g_inEnumeration;
    const HRESULT hr = g_enumW(self, devType, &FilterCallbackW, &c, flags);
    --g_inEnumeration;
    EndEnumeration(c.number, c.log, c.shown, c.hidden, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedEnumDevicesA(IDirectInput8A* self, DWORD devType, LPDIENUMDEVICESCALLBACKA callback,
                                             LPVOID ref, DWORD flags)
{
    if (!callback)
        return g_enumA(self, devType, callback, ref, flags);
    EnumContext<LPDIENUMDEVICESCALLBACKA> c{callback, ref, Hiding(), false, 0, 0, 0};
    c.log = BeginEnumeration(&c.number, devType, flags, c.hide, false);
    ++g_inEnumeration;
    const HRESULT hr = g_enumA(self, devType, &FilterCallbackA, &c, flags);
    --g_inEnumeration;
    EndEnumeration(c.number, c.log, c.shown, c.hidden, hr);
    return hr;
}

// A device created by GUID: refused when it's a game controller (one the game remembered from before, or found on its
// own), so it can't be used as the gamepad either way.
template <typename Device, typename Info>
bool RefuseController(Device* device)
{
    Info info{};
    info.dwSize = sizeof(info);
    return SUCCEEDED(device->GetDeviceInfo(&info)) && IsGameController(info.dwDevType);
}

HRESULT STDMETHODCALLTYPE HookedCreateDeviceW(IDirectInput8W* self, REFGUID guid, LPDIRECTINPUTDEVICE8W* out,
                                              LPUNKNOWN outer)
{
    const HRESULT hr = g_createW(self, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out && g_inEnumeration == 0 && Hiding() &&
        RefuseController<IDirectInputDevice8W, DIDEVICEINSTANCEW>(*out))
    {
        (*out)->Release();
        *out = nullptr;
        if (g_refusals.fetch_add(1, std::memory_order_relaxed) < 10)
            MOHW_LOG(kLogFile, "game asked to create a game controller (W): refused, hidden from the game");
        return DIERR_DEVICENOTREG;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateDeviceA(IDirectInput8A* self, REFGUID guid, LPDIRECTINPUTDEVICE8A* out,
                                              LPUNKNOWN outer)
{
    const HRESULT hr = g_createA(self, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out && g_inEnumeration == 0 && Hiding() &&
        RefuseController<IDirectInputDevice8A, DIDEVICEINSTANCEA>(*out))
    {
        (*out)->Release();
        *out = nullptr;
        if (g_refusals.fetch_add(1, std::memory_order_relaxed) < 10)
            MOHW_LOG(kLogFile, "game asked to create a game controller (A): refused, hidden from the game");
        return DIERR_DEVICENOTREG;
    }
    return hr;
}

bool HookSlot(void* object, int slot, void* detour, void** original, const char* name)
{
    void* target = (*reinterpret_cast<void***>(object))[slot];
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK)
        s = MH_EnableHook(target);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "hooking %s @ %p FAILED: %s", name, target, MH_StatusToString(s));
        return false;
    }
    MOHW_LOG(kLogFile, "%s hooked @ %p", name, target);
    return true;
}

// The devices connected now, logged once from the mod's own enumeration (the original functions: nothing hidden).
BOOL CALLBACK ListCallback(LPCDIDEVICEINSTANCEW device, LPVOID)
{
    char name[MAX_PATH], product[MAX_PATH];
    ToUtf8(device->tszInstanceName, name, sizeof(name));
    ToUtf8(device->tszProductName, product, sizeof(product));
    MOHW_LOG(kLogFile, "  connected: %s \"%s\" (product \"%s\", type 0x%02X)%s", TypeName(device->dwDevType), name,
             product, GET_DIDEVICE_TYPE(device->dwDevType),
             IsGameController(device->dwDevType) ? " -- a game controller: hidden from the game while it's on" : "");
    return DIENUM_CONTINUE;
}

} // namespace

bool InstallDirectInputFilter()
{
    const bool alreadyLoaded = GetModuleHandleA("dinput8.dll") != nullptr;
    HMODULE dinput = LoadLibraryA("dinput8.dll");
    if (!dinput)
    {
        MOHW_LOG(kLogFile, "LoadLibrary(dinput8.dll) FAILED (GetLastError=%lu) -- other game controllers not hidden",
                 GetLastError());
        return false;
    }
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(dinput, "DirectInput8Create"));
    if (!create)
    {
        MOHW_LOG(kLogFile, "GetProcAddress(DirectInput8Create) FAILED");
        return false;
    }
    MOHW_LOG(kLogFile, "dinput8.dll %s; hiding other game controllers: setting %s (applies while SteamVR is running or "
             "the mod is on)", alreadyLoaded ? "was already loaded by the game (it may have looked for controllers "
             "before this)" : "not loaded by the game yet", GetHideOtherGameControllers() ? "ON" : "off");

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    const HINSTANCE self = GetModuleHandleW(nullptr);
    IDirectInput8W* wide = nullptr;
    IDirectInput8A* ansi = nullptr;
    bool ok = false;
    if (SUCCEEDED(create(self, DIRECTINPUT_VERSION, kIidDirectInput8W, reinterpret_cast<LPVOID*>(&wide), nullptr)) && wide)
    {
        MOHW_LOG(kLogFile, "DirectInput devices connected now:");
        wide->EnumDevices(DI8DEVCLASS_ALL, &ListCallback, nullptr, DIEDFL_ATTACHEDONLY);
        ok |= HookSlot(wide, kVtEnumDevices, reinterpret_cast<void*>(&HookedEnumDevicesW),
                       reinterpret_cast<void**>(&g_enumW), "IDirectInput8W::EnumDevices");
        HookSlot(wide, kVtCreateDevice, reinterpret_cast<void*>(&HookedCreateDeviceW), reinterpret_cast<void**>(&g_createW),
                 "IDirectInput8W::CreateDevice");
    }
    else
        MOHW_LOG(kLogFile, "DirectInput8Create (W) FAILED");
    if (SUCCEEDED(create(self, DIRECTINPUT_VERSION, kIidDirectInput8A, reinterpret_cast<LPVOID*>(&ansi), nullptr)) && ansi)
    {
        // The A and W functions are separate in dinput8.dll; if they ever shared one, its hook is already in.
        if ((*reinterpret_cast<void***>(ansi))[kVtEnumDevices] != (wide ? (*reinterpret_cast<void***>(wide))[kVtEnumDevices] : nullptr))
            ok |= HookSlot(ansi, kVtEnumDevices, reinterpret_cast<void*>(&HookedEnumDevicesA),
                           reinterpret_cast<void**>(&g_enumA), "IDirectInput8A::EnumDevices");
        if ((*reinterpret_cast<void***>(ansi))[kVtCreateDevice] != (wide ? (*reinterpret_cast<void***>(wide))[kVtCreateDevice] : nullptr))
            HookSlot(ansi, kVtCreateDevice, reinterpret_cast<void*>(&HookedCreateDeviceA),
                     reinterpret_cast<void**>(&g_createA), "IDirectInput8A::CreateDevice");
    }
    else
        MOHW_LOG(kLogFile, "DirectInput8Create (A) FAILED");
    // The objects are only for finding the functions: released, the hooks stay (dinput8.dll is kept loaded by the
    // LoadLibrary above).
    if (wide)
        wide->Release();
    if (ansi)
        ansi->Release();
    return ok;
}

} // namespace mohw
