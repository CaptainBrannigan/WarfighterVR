#include "engine_function_hook.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/renderview.h"
#include "constantbuffer_hook.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_enginefn.log";

// __thiscall: MSVC supports this calling-convention keyword on ordinary
// (non-member) functions too -- param1 arrives in ECX exactly like a real
// C++ "this" pointer would, matching FUN_007738c0's real ABI as compiled
// (confirmed from Ghidra's decompilation: `void __thiscall
// FUN_007738c0(int param_1, ...)`), even though this isn't a declared C++
// member function on our side.
using UpdateViewConstantsFn = void(__thiscall*)(int param1, unsigned int param2, int param3, int param4,
                                                   unsigned int param5);

UpdateViewConstantsFn g_originalUpdateViewConstants = nullptr;
void* g_hookAddress = nullptr;

std::atomic<int> g_logsWritten{0};
std::atomic<unsigned long long> g_nextLogAllowedMs{0};
std::atomic<int> g_memoryDumpsWritten{0};
std::atomic<unsigned long long> g_nextMemoryDumpAllowedMs{0};

// Isolated in its own function with no C++ objects requiring unwinding in
// its frame -- MSVC's __try/__except can't coexist with those in the same
// function (C2712). Returns false if the read raised an access violation.
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

// Diagnostic-only for now (task: the +0x87C field this function reads
// turned out to be a generic, reused per-material staging pointer -- a
// hardware write-breakpoint session showed it rewritten dozens of times
// per frame in long same-value "runs" matching material-batched draws, not
// a dedicated camera slot). Rather than solve "who writes it" (a dead
// end), check WHAT'S THERE on every single call using the same
// fovY-derived signature Phase 2B used for the constant buffer
// (proj[1][1] == 1/tan(fovY/2)): most invocations will be some unrelated
// material's data and won't match; the rare invocation where it DOES
// match is genuinely the camera. Logging only until the hit rate/
// reliability is confirmed live -- writing to this memory before that's
// established would be mutating data of unconfirmed ownership/lifetime.
std::atomic<long long> g_signatureChecks{0};
std::atomic<int> g_signatureMatchesLogged{0};
std::atomic<unsigned long long> g_nextSignatureLogAllowedMs{0};

float ExpectedProjectionM11()
{
    GameRenderer* gr = GameRenderer::Singleton();
    if (!gr)
        return 0.0f;
    float fovY = gr->m_viewParams.view.m_desc.fovY;
    if (fovY <= 0.0f || fovY >= 3.14159f)
        return 0.0f;
    return 1.0f / tanf(fovY * 0.5f);
}

// Runs on EVERY call (not time-throttled like LogCallIfDue/
// DumpObjectMemoryIfDue above) -- we don't know in advance which of the
// ~1,244 calls/sec is the camera one, so the check itself can't be
// sampled, only the resulting log output.
void CheckProjectionSignature(int param1)
{
    float target = ExpectedProjectionM11();
    if (target == 0.0f)
        return;

    unsigned char* base = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(param1));
    void* pointerAtOffset = nullptr;
    if (!SehSafeRead(&pointerAtOffset, base + 0x87c, sizeof(pointerAtOffset)) || !pointerAtOffset)
        return;

    // Row-major 4x4, proj[1][1] is float index 5 (row 1 * 4 + col 1) --
    // same position confirmed in the original DumpObjectMemoryIfDue float
    // dump (index 5 == 2.1445 alongside index 0 == 1.2063, matching a
    // standard perspective projection's [0][0]/[1][1] pair). Also read
    // index 6 (row1/col2) and index 14 (row3/col2) in the same read --
    // a live logging pass showed proj[1][1] alone isn't unique: a second,
    // oblique/skewed matrix (almost certainly a reflection/portal camera
    // clipping its near plane against the reflection surface, a standard
    // technique) shares the exact same proj[1][1] as the main camera
    // (same FOV) but has a nonzero skew term here and a translate-like
    // value at index 14 that visibly drifts frame to frame -- whereas the
    // real, unskewed main-camera projection consistently shows index 6 ==
    // 0 and index 14 == -0.06 (the near-plane constant). Both terms need
    // checking together to reject the oblique impostor.
    float candidates[7]{};
    if (!SehSafeRead(candidates, pointerAtOffset, sizeof(candidates)))
        return;
    float candidate = candidates[5];
    float skewTerm = candidates[6];

    long long totalChecks = ++g_signatureChecks;
    if (fabsf(candidate - target) >= 0.01f)
        return;
    if (fabsf(skewTerm) >= 0.001f)
        return; // oblique/reflection-camera projection -- not the main view

    if (g_signatureMatchesLogged.load() >= 40)
        return;
    unsigned long long now = GetTickCount64();
    if (now < g_nextSignatureLogAllowedMs.load())
        return;
    g_nextSignatureLogAllowedMs.store(now + 250); // finer-grained than the 1s dump throttle -- want to see clustering/frequency within a single frame

    int n = ++g_signatureMatchesLogged;

    unsigned char rawFloats[16 * sizeof(float)]{};
    bool gotFullDump = SehSafeRead(rawFloats, pointerAtOffset, sizeof(rawFloats));
    std::string dump;
    if (gotFullDump)
    {
        const float* floats = reinterpret_cast<const float*>(rawFloats);
        char fbuf[24];
        for (size_t i = 0; i < 16; ++i)
        {
            snprintf(fbuf, sizeof(fbuf), "%.4f ", floats[i]);
            dump += fbuf;
            if (i % 4 == 3)
                dump += "| ";
        }
    }

    MOHW_LOG(kLogFile,
              "SIGNATURE MATCH #%d: param1=%p pointerAtOffset=%p candidateProj[1][1]=%.6f target=%.6f "
              "(checked %lld calls so far this session) first16floats: %s",
              n, reinterpret_cast<void*>(static_cast<uintptr_t>(param1)), pointerAtOffset, candidate, target,
              totalChecks, gotFullDump ? dump.c_str() : "<read failed>");

    // Mapping step (task: identify what, if anything, downstream consumes
    // each sub-matrix before writing any per-eye patch into this struct).
    // Two position-shaped candidates were spotted in the full struct dump:
    // floats[28..30] (row 3 of the second 4x4 -- a full transform WITH
    // translation, offset 0x40 into the struct) and floats[48..50] (a
    // standalone position-shaped triple past the third, rotation-only
    // 4x4, offset 0xC0). Cross-check both against the already-proven-
    // correct cameraPos this project already uses for real eye-offset math
    // (constantbuffer_hook.cpp's 352-byte view buffer, floats[80..82],
    // byte offset 320) -- whichever candidate here numerically matches
    // that trusted value is very likely tracking the same real-world
    // camera position, just reached via a different code path.
    unsigned char restOfStruct[56 * sizeof(float)]{};
    bool gotRest = SehSafeRead(restOfStruct, pointerAtOffset, sizeof(restOfStruct));
    unsigned char knownBuffer[352]{};
    bool gotKnownCameraPos = GetLastKnownViewBufferFullBytes(knownBuffer);
    if (gotRest && gotKnownCameraPos)
    {
        const float* floats = reinterpret_cast<const float*>(restOfStruct);
        const float* knownFloats = reinterpret_cast<const float*>(knownBuffer);
        const float* matrix2Translation = floats + 28; // offset 0x40 into the struct, row 3 (x,y,z)
        const float* standalonePosition = floats + 48; // offset 0xC0 into the struct (x,y,z)
        const float* trustedCameraPos = knownFloats + 80; // byte offset 320 in the known-good 352-byte CB
        MOHW_LOG(kLogFile,
                  "MATCH #%d position candidates: matrix2Translation=(%.4f,%.4f,%.4f) "
                  "standalonePosition=(%.4f,%.4f,%.4f) trustedCameraPos(from CB)=(%.4f,%.4f,%.4f)",
                  n, matrix2Translation[0], matrix2Translation[1], matrix2Translation[2], standalonePosition[0],
                  standalonePosition[1], standalonePosition[2], trustedCameraPos[0], trustedCameraPos[1],
                  trustedCameraPos[2]);
    }
    else
    {
        MOHW_LOG(kLogFile,
                  "MATCH #%d position candidates: gotRest=%d gotKnownCameraPos=%d -- one or both reads failed",
                  n, gotRest, gotKnownCameraPos);
    }
}

// Dumps the object's own bytes around the field cluster FUN_007738c0
// touches (0x85c..0x908, padded a bit on each side), then follows the
// pointer stored at +0x87c (confirmed via Ghidra's decompilation to be
// read as `*(float **)(param_1 + 0x87c)` and indexed far beyond, i.e. a
// pointer to a separate float array/matrix, not raw data at +0x87c
// itself) and dumps what IT points to as well -- task: trace the real
// camera-matrix source now that param1 is known to sit at a fixed,
// trusted offset from GameRenderer::Singleton() (+0x2C20), rather than
// inside GameRenderViewParams as originally guessed.
void DumpObjectMemoryIfDue(int param1)
{
    // Time-throttled, not a raw call-count cap -- the earlier count-based
    // cap (5 samples) burned through its whole budget in ~3 milliseconds,
    // all during the same loading/menu moment (same trap this project's
    // other diagnostics hit earlier: a tiny sample cap can trivially land
    // entirely within one non-representative instant). Spreading samples
    // out over wall-clock time instead gives real coverage across a whole
    // play session, letting us see whether this pointer ever changes to
    // something other than the identity-matrix placeholder seen so far.
    if (g_memoryDumpsWritten.load() >= 60)
        return;
    unsigned long long now = GetTickCount64();
    if (now < g_nextMemoryDumpAllowedMs.load())
        return;
    g_nextMemoryDumpAllowedMs.store(now + 1000);
    ++g_memoryDumpsWritten;

    unsigned char* base = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(param1));

    unsigned char localBytes[0x160]{};
    bool ok = SehSafeRead(localBytes, base + 0x800, sizeof(localBytes));
    if (!ok)
    {
        MOHW_LOG(kLogFile, "DumpObjectMemoryIfDue: access violation reading param1+0x800.."
                            "0x960, skipping this sample");
    }
    else
    {
        std::string dump;
        char buf[16];
        for (size_t i = 0; i < sizeof(localBytes); i += 4)
        {
            unsigned int word;
            memcpy(&word, localBytes + i, 4);
            snprintf(buf, sizeof(buf), "%08X ", word);
            dump += buf;
            if (i % 16 == 12)
                dump += "| ";
        }
        MOHW_LOG(kLogFile, "object bytes [param1+0x800 .. +0x960), 4 bytes per group, offset 0x800 first: %s",
                  dump.c_str());
    }

    void* pointerAtOffset = nullptr;
    if (!SehSafeRead(&pointerAtOffset, base + 0x87c, sizeof(pointerAtOffset)))
    {
        MOHW_LOG(kLogFile, "DumpObjectMemoryIfDue: access violation reading the pointer at param1+0x87c itself");
        return;
    }

    MOHW_LOG(kLogFile, "pointer stored at param1+0x87c = %p (delta from param1 = 0x%tX)", pointerAtOffset,
              reinterpret_cast<unsigned char*>(pointerAtOffset) - base);

    if (!pointerAtOffset)
        return;

    unsigned char pointedBytes[0xE0]{};
    if (!SehSafeRead(pointedBytes, pointerAtOffset, sizeof(pointedBytes)))
    {
        MOHW_LOG(kLogFile, "DumpObjectMemoryIfDue: access violation reading *(param1+0x87c) at %p",
                  pointerAtOffset);
        return;
    }

    const float* floats = reinterpret_cast<const float*>(pointedBytes);
    size_t floatCount = sizeof(pointedBytes) / sizeof(float);
    std::string floatDump;
    char fbuf[24];
    for (size_t i = 0; i < floatCount; ++i)
    {
        snprintf(fbuf, sizeof(fbuf), "%.4f ", floats[i]);
        floatDump += fbuf;
        if (i % 4 == 3)
            floatDump += "| ";
    }
    MOHW_LOG(kLogFile, "*(param1+0x87c) as floats, offset 0x00 first: %s", floatDump.c_str());
}

// Ordinary calling convention on purpose -- keeps the boundary between
// "hand-specified __thiscall matching the real function's ABI" and
// "normal C++ code" explicit, even though calling into GameRenderer::
// Singleton()/MOHW_LOG from inside the __thiscall hook body directly
// would also work fine with MSVC.
void LogCallIfDue(int param1)
{
    // Time-throttled for the same reason as DumpObjectMemoryIfDue's own
    // cap below -- a raw call-count cap at ~1,244 calls/sec exhausts in
    // milliseconds, all within one non-representative instant.
    if (g_logsWritten.load() >= 60)
        return;
    unsigned long long now = GetTickCount64();
    if (now < g_nextLogAllowedMs.load())
        return;
    g_nextLogAllowedMs.store(now + 1000);
    ++g_logsWritten;

    GameRenderer* renderer = GameRenderer::Singleton();
    void* rendererAddr = static_cast<void*>(renderer);
    ptrdiff_t deltaFromRenderer = 0;
    ptrdiff_t deltaFromViewParams = 0;
    if (renderer)
    {
        deltaFromRenderer =
            reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(param1)) - reinterpret_cast<unsigned char*>(renderer);
        deltaFromViewParams = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(param1)) -
                               reinterpret_cast<unsigned char*>(&renderer->m_viewParams);
    }

    MOHW_LOG(kLogFile,
              "FUN_007738c0 called: param1=0x%p GameRenderer::Singleton()=0x%p "
              "(param1 - Singleton())=0x%tX (param1 - &m_viewParams)=0x%tX",
              reinterpret_cast<void*>(static_cast<uintptr_t>(param1)), rendererAddr, deltaFromRenderer,
              deltaFromViewParams);

    DumpObjectMemoryIfDue(param1);
}

// MSVC rejects __thiscall directly on a free function under /permissive-
// (error C3865). The standard workaround: a non-static member function
// naturally compiles to __thiscall with no keyword needed (param1 becomes
// the implicit "this"), and for a simple, non-virtual, no-multiple-
// inheritance class on the 32-bit MSVC ABI, a pointer to that member
// function has the exact same representation as a plain code address --
// letting it be reinterpret_cast to a plain function pointer for MinHook.
// Implementation-defined, not portable C++, but this project is
// unconditionally MSVC/x86-only already, and this exact pattern is the
// established technique throughout the hooking/reverse-engineering
// community for this situation.
struct ThisCallTrampoline
{
    void Hooked(unsigned int param2, int param3, int param4, unsigned int param5)
    {
        int param1 = reinterpret_cast<int>(this);
        LogCallIfDue(param1);
        CheckProjectionSignature(param1);
        g_originalUpdateViewConstants(param1, param2, param3, param4, param5);
    }
};

} // namespace

bool InstallEngineFunctionHook()
{
    // g_moduleBase/VerifyBuild() are already established by the time
    // EnsureInitialized() in dllmain.cpp reaches this call -- Offset()
    // applies the same (currently zero, but always computed properly
    // rather than hard-assumed) ASLR delta every other offset-based
    // lookup in this project uses.
    if (g_moduleBase == 0 || !g_buildVerified)
    {
        MOHW_LOG(kLogFile, "InstallEngineFunctionHook: module base not resolved / build not verified yet -- "
                            "refusing to install");
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    g_hookAddress = Offset<void*>(OFFSET_UPDATEVIEWCONSTANTS);

    // A pointer-to-member-function isn't reinterpret_cast-able to void*
    // directly (different representation in the standard's object model) --
    // memcpy is the standard-compliant way to type-pun it into a plain
    // code address. Safe specifically because ThisCallTrampoline has no
    // virtual functions and no (multiple/virtual) inheritance, so on the
    // MSVC x86 ABI a member function pointer is just that address, with
    // no adjustor-thunk indirection to worry about -- the static_assert
    // below catches it if that ever stops being true.
    auto memberFn = &ThisCallTrampoline::Hooked;
    static_assert(sizeof(memberFn) == sizeof(void*),
                   "ThisCallTrampoline::Hooked's member-function-pointer isn't a plain code address -- "
                   "check for accidental virtual functions or multiple inheritance");
    void* detour = nullptr;
    memcpy(&detour, &memberFn, sizeof(detour));

    MH_STATUS s =
        MH_CreateHook(g_hookAddress, detour, reinterpret_cast<void**>(&g_originalUpdateViewConstants));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(FUN_007738c0 @ %p) FAILED: %s", g_hookAddress, MH_StatusToString(s));
        g_hookAddress = nullptr;
        return false;
    }

    s = MH_EnableHook(g_hookAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook FAILED: %s", MH_StatusToString(s));
        return false;
    }

    MOHW_LOG(kLogFile, "Engine function hook installed @ %p", g_hookAddress);
    return true;
}

void RemoveEngineFunctionHook()
{
    if (g_hookAddress)
    {
        MH_DisableHook(g_hookAddress);
        MH_RemoveHook(g_hookAddress);
        g_hookAddress = nullptr;
    }
    // MH_Uninitialize() is called once centrally from dllmain.cpp.
}

} // namespace mohw
