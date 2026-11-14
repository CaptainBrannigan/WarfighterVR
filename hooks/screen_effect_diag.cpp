#include "screen_effect_diag.h"

#include "native_stereo.h"
#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_screeneffect.log";

// FUN_00FD2FA0: __thiscall(renderer, view, viewport, visual environment), RET 0xC.
using ScreenEffectFn = void(__fastcall*)(void* self, void* edx, unsigned char* view, const uint16_t* viewport,
                                         unsigned char* environment);
ScreenEffectFn g_original = nullptr;
void* g_address = nullptr;

// The render interface's "get target", vtable +0x04: __thiscall, no arguments.
using GetTargetFn = void*(__fastcall*)(void* self, void* edx);

struct Sample
{
    uint32_t effect[2];
    uint32_t frame;
    uint16_t viewport[4];
    void* target;
    bool ok;
};

void* CurrentTarget()
{
    __try
    {
        void* iface = *reinterpret_cast<void* const*>(g_baseDelta + OFFSET_UI_RENDER_INTERFACE);
        if (!iface)
            return nullptr;
        void* const* vtable = *reinterpret_cast<void* const* const*>(iface);
        return reinterpret_cast<GetTargetFn>(vtable[1])(iface, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

Sample Read(const uint16_t* viewport, const unsigned char* environment)
{
    Sample s{};
    __try
    {
        s.effect[0] = *reinterpret_cast<const uint32_t*>(environment + 0xA20);
        s.effect[1] = *reinterpret_cast<const uint32_t*>(environment + 0xA24);
        s.frame = *reinterpret_cast<const uint32_t*>(environment + 0xAA0);
        for (int i = 0; i < 4; ++i)
            s.viewport[i] = viewport[i];
        s.ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s.ok = false;
    }
    return s;
}

std::atomic<long> g_calls[2] = {{0}, {0}}, g_active[2] = {{0}, {0}};
std::atomic<int> g_lines{0};

// THE LEFT VIEW'S SCREEN EFFECT, DRAWN LAST (2026-11-09, ScreenEffectsBothEyes; "the dust effect was still only in the
// right eye"): this log showed both views drawing it every frame (971 / 971 calls in 10 s), into the same bound target,
// each with its own half's viewport, so the left half's is lost later in the frame: something of the right view's own
// render, run after the left view's effect, rewrites the left half too (a whole-target copy, clear or post pass). So the
// left view's call is held and made right after the right view's, on the same thread (FUN_0101F5D0 renders the views in
// turn on one), with the left view's own arguments: its view, viewport and visual environment, which stay valid all
// frame (the world renderer's per-view blocks). A held call whose right view never comes (only one view drew an effect,
// or the frame ended) is dropped rather than made in a later frame, and counted.
struct HeldCall
{
    bool pending = false;
    void* self = nullptr;
    void* edx = nullptr;
    unsigned char* view = nullptr;
    const uint16_t* viewport = nullptr;
    unsigned char* environment = nullptr;
    DWORD thread = 0;
    unsigned long long atMs = 0;
};
std::mutex g_heldMutex;
HeldCall g_held; // guarded by g_heldMutex
std::atomic<long> g_heldCount{0}, g_madeLate{0}, g_dropped{0};

void __fastcall Hooked(void* self, void* edx, unsigned char* view, const uint16_t* viewport, unsigned char* environment)
{
    const Sample s = (viewport && environment) ? Read(viewport, environment) : Sample{};
    // Which half: a viewport starting past the frame's left edge is the right view.
    const int half = s.ok && s.viewport[0] > 0 ? 1 : 0;
    g_calls[half].fetch_add(1, std::memory_order_relaxed);
    const bool active = s.ok && (s.effect[0] != 0 || s.effect[1] != 0);
    const bool holdLeft = active && half == 0 && IsNativeStereoActive() && GetScreenEffectsBothEyes();
    if (active)
    {
        g_active[half].fetch_add(1, std::memory_order_relaxed);
        static std::atomic<unsigned long long> nextLineMs{0};
        const unsigned long long now = GetTickCount64();
        unsigned long long due = nextLineMs.load(std::memory_order_relaxed);
        if (g_lines.load(std::memory_order_relaxed) < 160 && now >= due &&
            nextLineMs.compare_exchange_strong(due, now + 100, std::memory_order_relaxed))
        {
            g_lines.fetch_add(1, std::memory_order_relaxed);
            MOHW_LOG(kLogFile,
                     "screen effect for the %s view (viewport %u,%u %ux%u, two-view %s): resources +0xA20 %08X +0xA24 "
                     "%08X, frame %u, bound target %p, renderer %p, thread %lu%s",
                     half ? "right" : "left", s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3],
                     IsNativeStereoActive() ? "on" : "off", s.effect[0], s.effect[1], s.frame, CurrentTarget(), self,
                     GetCurrentThreadId(), holdLeft ? " -- held, made after the right view's" : "");
        }
    }
    const unsigned long long now = GetTickCount64();
    if (holdLeft)
    {
        std::lock_guard<std::mutex> lock(g_heldMutex);
        if (g_held.pending)
            g_dropped.fetch_add(1, std::memory_order_relaxed); // the last one's right view never came
        g_held = HeldCall{true, self, edx, view, viewport, environment, GetCurrentThreadId(), now};
        g_heldCount.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        g_original(self, edx, view, viewport, environment);
        if (half == 1)
        {
            HeldCall held;
            {
                std::lock_guard<std::mutex> lock(g_heldMutex);
                held = g_held;
                g_held.pending = false;
            }
            if (held.pending && held.thread == GetCurrentThreadId() && held.self == self && now - held.atMs < 100)
            {
                g_original(held.self, held.edx, held.view, held.viewport, held.environment);
                g_madeLate.fetch_add(1, std::memory_order_relaxed);
            }
            else if (held.pending)
            {
                g_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    static std::atomic<unsigned long long> nextSummaryMs{0};
    unsigned long long due = nextSummaryMs.load(std::memory_order_relaxed);
    if (now >= due && nextSummaryMs.compare_exchange_strong(due, now + 10000, std::memory_order_relaxed))
    {
        const long active0 = g_active[0].exchange(0), active1 = g_active[1].exchange(0);
        const long calls0 = g_calls[0].exchange(0), calls1 = g_calls[1].exchange(0);
        const long held = g_heldCount.exchange(0), late = g_madeLate.exchange(0), dropped = g_dropped.exchange(0);
        if (active0 || active1 || held || dropped)
            MOHW_LOG(kLogFile,
                     "last 10 s: screen effect calls left %ld (%ld with an effect), right %ld (%ld with an effect); the left "
                     "view's held %ld, made after the right view's %ld, dropped %ld",
                     calls0, active0, calls1, active1, held, late, dropped);
    }
}

} // namespace

bool InstallScreenEffectDiag()
{
    if (g_moduleBase == 0 || !g_buildVerified || g_drivingExe)
        return false;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    void* address = Offset<void*>(OFFSET_SCREEN_EFFECT_RENDER);
    s = MH_CreateHook(address, reinterpret_cast<void*>(&Hooked), reinterpret_cast<void**>(&g_original));
    if (s == MH_OK)
        s = MH_EnableHook(address);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "screen effect diagnostic @ %p FAILED: %s", address, MH_StatusToString(s));
        return false;
    }
    g_address = address;
    MOHW_LOG(kLogFile, "screen effect diagnostic installed @ %p (FUN_00FD2FA0)", address);
    return true;
}

void RemoveScreenEffectDiag()
{
    if (!g_address)
        return;
    MH_DisableHook(g_address);
    MH_RemoveHook(g_address);
    g_address = nullptr;
}

} // namespace mohw
