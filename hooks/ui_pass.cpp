#include "ui_pass.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"

#include <windows.h>
#include <atomic>
#include <cstdint>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_uipass.log";

using UiPassFn = void(__fastcall*)(void* self, void* edx, uint32_t* params);
UiPassFn g_original = nullptr;
void* g_address = nullptr;

thread_local int t_pass = -1;
std::atomic<long> g_calls[2] = {{0}, {0}};
std::atomic<long> g_draws[2] = {{0}, {0}};
std::atomic<DWORD> g_thread[2] = {{0}, {0}};

int ReadPass(const uint32_t* params)
{
    __try
    {
        return params[3] != 0 ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

void __fastcall Hooked(void* self, void* edx, uint32_t* params)
{
    const int pass = params ? ReadPass(params) : 0;
    const int previous = t_pass;
    t_pass = pass;
    g_thread[pass].store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_original(self, edx, params);
    t_pass = previous;
    g_calls[pass].fetch_add(1, std::memory_order_relaxed);

    static std::atomic<unsigned long long> nextLogMs{0};
    const unsigned long long now = GetTickCount64();
    unsigned long long due = nextLogMs.load(std::memory_order_relaxed);
    if (now >= due && nextLogMs.compare_exchange_strong(due, now + 5000, std::memory_order_relaxed))
    {
        MOHW_LOG(kLogFile,
                 "UI passes in the last 5 s: before the world %ld call(s), %ld D3D draw(s) inside, thread %lu; HUD %ld "
                 "call(s), %ld draw(s) inside, thread %lu (draws inside = the D3D calls are made in the pass itself)",
                 g_calls[0].exchange(0), g_draws[0].exchange(0), g_thread[0].load(), g_calls[1].exchange(0),
                 g_draws[1].exchange(0), g_thread[1].load());
    }
}

} // namespace

bool InstallUiPassHook()
{
    if (g_moduleBase == 0 || !g_buildVerified || g_drivingExe)
        return false;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    g_address = Offset<void*>(OFFSET_UI_PASS);
    s = MH_CreateHook(g_address, reinterpret_cast<void*>(&Hooked), reinterpret_cast<void**>(&g_original));
    if (s == MH_OK)
        s = MH_EnableHook(g_address);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "UI pass hook (FUN_00CBE3D0 @ %p) FAILED: %s", g_address, MH_StatusToString(s));
        g_address = nullptr;
        return false;
    }
    MOHW_LOG(kLogFile, "UI pass hook installed (FUN_00CBE3D0 @ %p)", g_address);
    return true;
}

void RemoveUiPassHook()
{
    if (g_address)
    {
        MH_DisableHook(g_address);
        MH_RemoveHook(g_address);
        g_address = nullptr;
    }
}

int CurrentUiPass()
{
    return t_pass;
}

void UiPassNoteDraw()
{
    if (t_pass >= 0)
        g_draws[t_pass].fetch_add(1, std::memory_order_relaxed);
}

} // namespace mohw
