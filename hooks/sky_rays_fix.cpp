#include "sky_rays_fix.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_sky.log";

// FUN_00FD9020's frame (ESP after its AND ESP,-16), at 0x00FD9DC9: its camera copy at +0x380, whose corner points are at
// +0x480..+0x4FF (eight float4: each corner's two points), and the quad's vertices from +0xE0 (24 bytes each: x, y, z,
// then the ray). Vertex 0 (-1,-1) is the points at +0x480 minus +0x490, vertex 1 (1,-1) +0x4C0 - +0x4D0, vertex 2 (1,1)
// +0x4E0 - +0x4F0, vertex 3 (-1,1) +0x4A0 - +0x4B0 (the disassembly at 0x00FD9BA1-0x00FD9DC2); vertices 4 and 5 are
// copied from 0 and 2 after this point.
constexpr uintptr_t kPointsOffset = 0x480;
constexpr int kPairs[4][2] = {{0, 1}, {4, 5}, {6, 7}, {2, 3}};
constexpr uintptr_t kRayOffsets[4] = {0xEC, 0x104, 0x11C, 0x134};
// The bytes the patch replaces and what has to be around them: MOV [ESP+0x13C],ECX / MOVQ XMM0,[ESP+0xE0] /
// MOV ECX,[0x02AC16F4].
constexpr unsigned char kExpected[] = {0x89, 0x8C, 0x24, 0x3C, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x7E,
                                       0x84, 0x24, 0xE0, 0x00, 0x00, 0x00, 0x8B, 0x0D};
constexpr uintptr_t kExpectedStart = 7; // the patch address is this far into kExpected

void* g_address = nullptr;
void* g_trampoline = nullptr;
std::atomic<long> g_rewritten{0};
std::atomic<long> g_mismatched{0};
std::atomic<int> g_detailLogs{0};

bool EngineStereoActive()
{
    __try
    {
        const uintptr_t renderer = *Offset<const uint32_t*>(OFFSET_DXRENDERER);
        return renderer != 0 && *reinterpret_cast<const unsigned char*>(renderer + 0x44) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return true; // unreadable: leave the rays alone
    }
}

void LogIfDue()
{
    static std::atomic<unsigned long long> nextMs{0};
    const unsigned long long now = GetTickCount64();
    unsigned long long due = nextMs.load(std::memory_order_relaxed);
    if (now < due || !nextMs.compare_exchange_strong(due, now + 30000, std::memory_order_relaxed))
        return;
    const long done = g_rewritten.exchange(0), bad = g_mismatched.exchange(0);
    MOHW_LOG(kLogFile, "last 30 s: %ld sky quad(s) drawn with exact rays, %ld left as the game made them (layout check failed)",
             done, bad);
}

// The game's frame at the join, `sp` = its ESP. Runs on the render job threads, once per sky draw (per view, and per
// envmap face).
void __cdecl FixSkyRays(unsigned char* sp)
{
    if (!GetSkyExactRaysEnabled() || EngineStereoActive())
        return;
    const float* points = reinterpret_cast<const float*>(sp + kPointsOffset);
    float rays[4][3];
    float mean[3] = {0.0f, 0.0f, 0.0f};
    for (int v = 0; v < 4; ++v)
    {
        const float* a = points + kPairs[v][0] * 4;
        const float* b = points + kPairs[v][1] * 4;
        float length = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            rays[v][k] = a[k] - b[k];
            length += rays[v][k] * rays[v][k];
        }
        length = std::sqrt(length);
        // The game's ray for this corner is this one normalised: anything else means the frame isn't laid out as read.
        const float* game = reinterpret_cast<const float*>(sp + kRayOffsets[v]);
        const float dot = length > 1e-6f ? (game[0] * rays[v][0] + game[1] * rays[v][1] + game[2] * rays[v][2]) / length
                                         : 0.0f;
        if (!(length > 1e-6f && std::isfinite(length) && dot > 0.999f && dot < 1.001f))
        {
            g_mismatched.fetch_add(1, std::memory_order_relaxed);
            if (g_detailLogs.fetch_add(1, std::memory_order_relaxed) < 6)
                MOHW_LOG(kLogFile, "corner %d: the game's ray (%.4f, %.4f, %.4f) isn't the corner points' (%.3f, %.3f, %.3f) "
                                   "normalised (dot %.5f): left as it was",
                         v, game[0], game[1], game[2], rays[v][0], rays[v][1], rays[v][2], dot);
            LogIfDue();
            return;
        }
        for (int k = 0; k < 3; ++k)
            mean[k] += rays[v][k] * 0.25f;
    }
    const float meanLength = std::sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
    if (!(meanLength > 1e-6f) || !std::isfinite(meanLength))
    {
        g_mismatched.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // One scale for all four, so the centre ray comes out unit length: the interpolated ray is then the exact one,
    // and its length (for a shader that didn't normalise) stays near what the game's had.
    const float scale = 1.0f / meanLength;
    const bool logThis = g_detailLogs.load(std::memory_order_relaxed) < 6 && g_rewritten.load(std::memory_order_relaxed) < 2;
    for (int v = 0; v < 4; ++v)
    {
        float* ray = reinterpret_cast<float*>(sp + kRayOffsets[v]);
        if (logThis)
            MOHW_LOG(kLogFile, "corner %d: (%.4f, %.4f, %.4f) -> (%.4f, %.4f, %.4f)", v, ray[0], ray[1], ray[2],
                     rays[v][0] * scale, rays[v][1] * scale, rays[v][2] * scale);
        for (int k = 0; k < 3; ++k)
            ray[k] = rays[v][k] * scale;
    }
    if (logThis)
        g_detailLogs.fetch_add(1, std::memory_order_relaxed);
    g_rewritten.fetch_add(1, std::memory_order_relaxed);
    LogIfDue();
}

// Entered by a JMP from the patch, with the game's registers and stack; the trampoline replays the MOVQ the patch
// covered and returns to 0x00FD9DD2. No XMM register is live here: XMM0 is reloaded by that MOVQ, and nothing else is
// read before the next call.
__declspec(naked) void SkyRaysStub()
{
    __asm {
        pushfd
        pushad
        lea eax, [esp + 36]
        push eax
        call FixSkyRays
        add esp, 4
        popad
        popfd
        jmp g_trampoline
    }
}

bool BytesMatch(const unsigned char* at)
{
    __try
    {
        return memcmp(at, kExpected, sizeof(kExpected)) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

} // namespace

bool InstallSkyRaysFix()
{
    if (g_moduleBase == 0 || !g_buildVerified || g_drivingExe)
        return false;
    void* address = Offset<void*>(OFFSET_SKY_RAYS_JOIN);
    if (!BytesMatch(static_cast<const unsigned char*>(address) - kExpectedStart))
    {
        MOHW_LOG(kLogFile, "sky rays fix NOT installed: the code at %p isn't the expected MOVQ XMM0,[ESP+0xE0]", address);
        return false;
    }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    s = MH_CreateHook(address, reinterpret_cast<void*>(&SkyRaysStub), &g_trampoline);
    if (s == MH_OK)
        s = MH_EnableHook(address);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "sky rays fix @ %p FAILED: %s", address, MH_StatusToString(s));
        return false;
    }
    g_address = address;
    MOHW_LOG(kLogFile, "sky rays fix installed @ %p: the sky's corner rays exact (SkyExactRays %s)", address,
             GetSkyExactRaysEnabled() ? "on" : "off");
    return true;
}

void RemoveSkyRaysFix()
{
    if (!g_address)
        return;
    MH_DisableHook(g_address);
    MH_RemoveHook(g_address);
    g_address = nullptr;
}

} // namespace mohw
