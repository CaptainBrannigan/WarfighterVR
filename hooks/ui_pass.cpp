#include "ui_pass.h"

#include "marker_projection_hook.h"
#include "native_stereo.h"
#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_uipass.log";

using UiPassFn = void(__fastcall*)(void* self, void* edx, uint32_t* params);
UiPassFn g_original = nullptr;
void* g_address = nullptr;

// FUN_00CBB4E0, the list replay (OFFSET_UI_LIST_REPLAY): __thiscall(state, list), RET 4.
using ReplayFn = void(__fastcall*)(void* state, void* edx, int list);
ReplayFn g_originalReplay = nullptr;
void* g_replayAddress = nullptr;

// UI LAYOUT SIZE (2026-11-12, setting UiLayoutScale). FUN_00CB7480 (OFFSET_UI_LAYOUT_RECT): __thiscall(Scaleform state,
// int out[4]) -> out, RET 4: the screen rectangle the UI lays its movies out in -- DxRenderer +0x1A0 (the screen, x y w
// h) inside the safe area (+0x2344/+0x2348 of the state). Every movie's viewport comes from it: the UI manager's update
// (FUN_008fcfd0 -> FUN_008f4c70, re-laid out whenever it changes) and each movie's constructor (FUN_00a75220). The
// menus are laid out in pixels, so with the backbuffer at SteamVR's eye size they came out smaller the higher the
// resolution (the user, 2026-11-12: "the menu gets smaller and smaller"). Dividing the rectangle by the setting lays
// the UI out as on a screen that much smaller: menus that much bigger, still drawn at the full resolution, since the
// mod's placement sets the viewport the UI is drawn into (draw_trace_diag.cpp). Markers stay right: their caller
// (FUN_016ab1c0) maps a fraction of the screen into the HUD movie's own visible rectangle. Only with the HUD placement
// on (without it the UI would draw into the top-left part of the frame).
using LayoutRectFn = int*(__fastcall*)(void* state, void* edx, int* out);
LayoutRectFn g_originalLayoutRect = nullptr;
void* g_layoutRectAddress = nullptr;

int* __fastcall HookedLayoutRect(void* state, void* edx, int* out)
{
    int* result = g_originalLayoutRect(state, edx, out);
    const float scale = GetUiLayoutScale();
    if (!out || !GetHudPlacementEnabled() || !(scale >= 0.25f && scale <= 8.0f) || fabsf(scale - 1.0f) < 0.001f)
        return result;
    int before[4];
    memcpy(before, out, sizeof(before));
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<int>(floorf(static_cast<float>(out[i]) / scale + 0.5f));
    static float lastScale = 0.0f; // render thread
    static int lastBefore[4] = {};
    if (scale != lastScale || memcmp(before, lastBefore, sizeof(before)) != 0)
    {
        lastScale = scale;
        memcpy(lastBefore, before, sizeof(before));
        MOHW_LOG(kLogFile, "HUD element scale %.2f: the UI's screen rectangle (%d,%d)-(%d,%d) -> (%d,%d)-(%d,%d)", scale,
                 before[0], before[1], before[2], before[3], out[0], out[1], out[2], out[3]);
    }
    return result;
}

// The UI render interface's clear (OFFSET_UI_RENDER_INTERFACE, vtable +0x08): __thiscall(self, flags, const float
// color[4]*, 0), RET 0xC.
using ClearFn = void(__fastcall*)(void* self, void* edx, uint32_t flags, const float* color, uint32_t zero);

thread_local int t_pass = -1;
thread_local int t_hudEye = -1;
thread_local int t_hudReplay = 0;
std::atomic<long> g_calls[2] = {{0}, {0}};
std::atomic<long> g_draws[2] = {{0}, {0}};
std::atomic<DWORD> g_thread[2] = {{0}, {0}};
std::atomic<long> g_halfReplays{0};

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
                 "call(s), %ld draw(s) inside, thread %lu (draws inside = the D3D calls are made in the pass itself); HUD "
                 "replayed into both halves %ld time(s) (native two-view)",
                 g_calls[0].exchange(0), g_draws[0].exchange(0), g_thread[0].load(), g_calls[1].exchange(0),
                 g_draws[1].exchange(0), g_thread[1].load(), g_halfReplays.exchange(0));
    }
}

// Before the second half's replay: the clear FUN_00CBE3D0 makes before each eye's replay in the engine's own stereo
// loop (flags 0x200 with a zero colour, at 0x00CBE5A6). SEH-guarded: a bad pointer just skips it.
void ClearBeforeSecondHalf()
{
    __try
    {
        void* iface = *reinterpret_cast<void* const*>(g_baseDelta + OFFSET_UI_RENDER_INTERFACE);
        if (!iface)
            return;
        void* const* vtable = *reinterpret_cast<void* const* const*>(iface);
        const ClearFn clear = reinterpret_cast<ClearFn>(vtable[2]);
        alignas(16) float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        clear(iface, nullptr, 0x200, zero, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

bool SafeCopy(void* to, const void* from, size_t n)
{
    __try
    {
        memcpy(to, from, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// DIAGNOSTIC (2026-11-08): the HUD list's recorded commands, to find the in-world markers' draws and how a draw's place
// is stored (the markers share the HUD's depth in two-view mode). The list is a vector of command pointers at the
// renderer state's +0x30C + list * 0x1010 (end at +0x310); FUN_00CBB4E0 replays nothing while +0x233E is set. A command:
// +0x00 the type (0 a draw, FUN_00CBB3C0 "Ui/drawIndexed"; 1 viewport; 2 target; 3 clear), +0x10 the draw's data block,
// as FUN_00CBB100 reads it: +0xAC/+0xB0 ids, +0xB4 the vertex shader constants' count (32-byte entries from +0xC0: a
// handle, a float4 at +0x10), +0x120 the pixel shader constants' count, +0x190/+0x1A0 a handle and its data, +0x1E0/+0x1F0
// the right eye's when the engine's stereo is on; +0x240.. the draw call's own arguments.
void DumpHudCommands(const unsigned char* state, int list)
{
    char skip = 0;
    uint32_t range[2] = {};
    if (!SafeCopy(&skip, state + 0x233E, 1) || !SafeCopy(range, state + 0x30C + list * 0x1010, sizeof(range)))
        return;
    const uint32_t count = range[1] >= range[0] ? (range[1] - range[0]) / 4 : 0;
    char markers[1200];
    const int markerCount = DescribeRecentMarkers(markers, sizeof(markers), 250);
    MOHW_LOG(kLogFile, "HUD LIST (two-view): %u command(s)%s; markers mapped in the last 250 ms (%d):%s", count,
             skip ? ", replay off (+0x233E set)" : "", markerCount, markerCount ? markers : " none");
    constexpr uint32_t kMaxLines = 48;
    for (uint32_t i = 0; i < count && i < kMaxLines; ++i)
    {
        uint32_t cmdAddress = 0;
        if (!SafeCopy(&cmdAddress, reinterpret_cast<const void*>(range[0] + i * 4), 4) || !cmdAddress)
            continue;
        const unsigned char* cmd = reinterpret_cast<const unsigned char*>(cmdAddress);
        int type = -1;
        if (!SafeCopy(&type, cmd, 4))
            continue;
        char line[1400];
        int used = _snprintf_s(line, sizeof(line), _TRUNCATE, "  #%u type %d", i, type);
        if (type == 0)
        {
            const unsigned char* data = cmd + 0x10;
            int ids[2] = {};
            unsigned char vsCount = 0, psCount = 0;
            uint32_t handles[2] = {};
            float blockA[16] = {}, blockB[8] = {};
            uint32_t args[7] = {};
            SafeCopy(ids, data + 0xAC, sizeof(ids));
            SafeCopy(&vsCount, data + 0xB4, 1);
            SafeCopy(&psCount, data + 0x120, 1);
            SafeCopy(&handles[0], data + 0x190, 4);
            SafeCopy(blockA, data + 0x1A0, sizeof(blockA));
            SafeCopy(&handles[1], data + 0x1E0, 4);
            SafeCopy(blockB, data + 0x1F0, sizeof(blockB));
            SafeCopy(args, cmd + 0x240, sizeof(args));
            used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
                                " ids %d/%d args %u %u %u %u %u %u %u vs %u ps %u", ids[0], ids[1], args[0], args[1],
                                args[2], args[3], args[4], args[5], args[6], vsCount, psCount);
            for (unsigned k = 0; k < vsCount && k < 6 && used > 0 && used < static_cast<int>(sizeof(line)); ++k)
            {
                uint32_t handle = 0;
                float v[4] = {};
                SafeCopy(&handle, data + 0xC0 + k * 0x20, 4);
                SafeCopy(v, data + 0xC0 + k * 0x20 + 0x10, sizeof(v));
                used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " c%u[%08X](%.4f %.4f %.4f %.4f)", k,
                                    handle, v[0], v[1], v[2], v[3]);
            }
            // Block A is the draw's transform to clip space, 4 rows (2026-11-09: rows 0 and 1 of the HUD's flat items
            // are (2/width, 0, 0, x) and (0, -2/height, 0, y); some items, the markers' frame among them, come with
            // perspective-looking values, so all 16 are printed, at enough precision for the 2/width scale).
            if (used > 0 && used < static_cast<int>(sizeof(line)))
                used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
                                    " A[%08X](%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g "
                                    "%.6g %.6g) B[%08X](%.3f %.3f %.3f %.3f)",
                                    handles[0], blockA[0], blockA[1], blockA[2], blockA[3], blockA[4], blockA[5],
                                    blockA[6], blockA[7], blockA[8], blockA[9], blockA[10], blockA[11], blockA[12],
                                    blockA[13], blockA[14], blockA[15], handles[1], blockB[0], blockB[1], blockB[2],
                                    blockB[3]);
        }
        else
        {
            uint32_t words[6] = {};
            SafeCopy(words, cmd + 4, sizeof(words));
            used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " words %08X %08X %08X %08X %08X %08X",
                                words[0], words[1], words[2], words[3], words[4], words[5]);
        }
        MOHW_LOG(kLogFile, "%s", line);
    }
}

// NATIVE TWO-VIEW (hooks/native_stereo.h): the HUD once into each half, as the engine's own stereo HUD does it once per
// eye (FUN_00CBE3D0's eye loop: a clear, then this replay, per eye). hooks/draw_trace_diag.cpp moves each replay's draws
// into that eye's HUD rectangle.
void __fastcall HookedReplay(void* state, void* edx, int list)
{
    if (list != 1 || t_pass != 1 || !IsNativeStereoActive())
    {
        g_originalReplay(state, edx, list);
        return;
    }
    static unsigned long long nextDumpMs = 0; // render thread
    const unsigned long long now = GetTickCount64();
    if (state && now >= nextDumpMs)
    {
        nextDumpMs = now + 10000;
        DumpHudCommands(static_cast<const unsigned char*>(state), list);
    }
    for (int eye = 0; eye < 2; ++eye)
    {
        if (eye == 1)
            ClearBeforeSecondHalf();
        t_hudEye = eye;
        ++t_hudReplay;
        g_originalReplay(state, edx, list);
    }
    t_hudEye = -1;
    g_halfReplays.fetch_add(1, std::memory_order_relaxed);
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

    g_replayAddress = Offset<void*>(OFFSET_UI_LIST_REPLAY);
    s = MH_CreateHook(g_replayAddress, reinterpret_cast<void*>(&HookedReplay), reinterpret_cast<void**>(&g_originalReplay));
    if (s == MH_OK)
        s = MH_EnableHook(g_replayAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "UI list replay hook (FUN_00CBB4E0 @ %p) FAILED: %s -- no HUD per half in native two-view",
                 g_replayAddress, MH_StatusToString(s));
        g_replayAddress = nullptr;
    }
    else
    {
        MOHW_LOG(kLogFile, "UI list replay hook installed (FUN_00CBB4E0 @ %p)", g_replayAddress);
    }

    g_layoutRectAddress = Offset<void*>(OFFSET_UI_LAYOUT_RECT);
    s = MH_CreateHook(g_layoutRectAddress, reinterpret_cast<void*>(&HookedLayoutRect),
                      reinterpret_cast<void**>(&g_originalLayoutRect));
    if (s == MH_OK)
        s = MH_EnableHook(g_layoutRectAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "UI layout rectangle hook (FUN_00CB7480 @ %p) FAILED: %s -- no HUD element scale",
                 g_layoutRectAddress, MH_StatusToString(s));
        g_layoutRectAddress = nullptr;
    }
    else
    {
        MOHW_LOG(kLogFile, "UI layout rectangle hook installed (FUN_00CB7480 @ %p)", g_layoutRectAddress);
    }
    return true;
}

void RemoveUiPassHook()
{
    if (g_layoutRectAddress)
    {
        MH_DisableHook(g_layoutRectAddress);
        MH_RemoveHook(g_layoutRectAddress);
        g_layoutRectAddress = nullptr;
    }
    if (g_replayAddress)
    {
        MH_DisableHook(g_replayAddress);
        MH_RemoveHook(g_replayAddress);
        g_replayAddress = nullptr;
    }
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

int CurrentHudEye()
{
    return t_hudEye;
}

int CurrentHudReplay()
{
    return t_hudReplay;
}

} // namespace mohw
