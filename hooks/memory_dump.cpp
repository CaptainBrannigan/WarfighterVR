#include "memory_dump.h"

#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_memdump.log";
constexpr int kDumpHotkey = VK_F9;
constexpr int kModuleRegistryDumpHotkey = VK_F10;

std::atomic<bool> g_alreadyDumped{false};
std::atomic<bool> g_moduleRegistryAlreadyDumped{false};

// Isolated in its own function with no C++ objects requiring unwinding in
// its frame -- MSVC's __try/__except can't coexist with those in the same
// function (C2712). Returns false if the copy raised an access violation.
bool SehSafeCopy(void* dst, const void* src, size_t size)
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

// Walks the live module's address range via VirtualQuery, copying only
// committed/readable regions (zero-filling the rest so byte OFFSETS in
// the output file still match virtual addresses 1:1 -- required so the
// dump can be loaded into Ghidra at the known base 0x00400000 and have
// every address line up directly with live-captured addresses, with no
// remapping needed). Wrapped in SEH per-region so a single bad page can't
// crash the whole dump (or the game).
bool DumpLiveModule()
{
    HMODULE hExe = GetModuleHandleA(nullptr);
    if (!hExe)
    {
        MOHW_LOG(kLogFile, "DumpLiveModule: GetModuleHandleA(nullptr) failed, err=%lu", GetLastError());
        return false;
    }

    unsigned char* base = reinterpret_cast<unsigned char*>(hExe);
    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    {
        MOHW_LOG(kLogFile, "DumpLiveModule: bad DOS signature at module base %p", static_cast<void*>(base));
        return false;
    }
    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
    {
        MOHW_LOG(kLogFile, "DumpLiveModule: bad NT signature");
        return false;
    }

    size_t sizeOfImage = ntHeaders->OptionalHeader.SizeOfImage;
    MOHW_LOG(kLogFile, "DumpLiveModule: base=%p sizeOfImage=0x%zX -- starting dump", static_cast<void*>(base),
              sizeOfImage);

    std::vector<unsigned char> buffer(sizeOfImage, 0);

    size_t offset = 0;
    size_t copiedBytes = 0;
    size_t skippedRegions = 0;
    while (offset < sizeOfImage)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T queried = VirtualQuery(base + offset, &mbi, sizeof(mbi));
        if (queried == 0)
        {
            offset += 0x1000; // can't even query this page -- skip it and keep going
            continue;
        }

        size_t regionEnd = offset + mbi.RegionSize;
        if (regionEnd > sizeOfImage)
            regionEnd = sizeOfImage;
        if (regionEnd <= offset)
        {
            offset += 0x1000;
            continue;
        }

        bool readable = (mbi.State == MEM_COMMIT) && mbi.Protect != 0 && !(mbi.Protect & PAGE_NOACCESS) &&
                          !(mbi.Protect & PAGE_GUARD);
        if (readable)
        {
            if (SehSafeCopy(buffer.data() + offset, base + offset, regionEnd - offset))
            {
                copiedBytes += (regionEnd - offset);
            }
            else
            {
                MOHW_LOG(kLogFile, "DumpLiveModule: access violation copying region at offset 0x%zX, skipping",
                          offset);
                ++skippedRegions;
            }
        }
        else
        {
            ++skippedRegions;
        }

        offset = regionEnd;
    }

    std::string path = LogFilePath("mohwvr_memdump.bin");
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
    {
        MOHW_LOG(kLogFile, "DumpLiveModule: failed to open output file %s", path.c_str());
        return false;
    }
    fwrite(buffer.data(), 1, buffer.size(), f);
    fclose(f);

    MOHW_LOG(kLogFile,
              "DumpLiveModule: wrote %s (%zu bytes total, %zu bytes actually copied from live memory, %zu "
              "regions skipped as unreadable/uncommitted)",
              path.c_str(), buffer.size(), copiedBytes, skippedRegions);
    return true;
}

// FUN_0050eba0 (the generic per-module dispatcher found via a live x64dbg
// execution breakpoint on FUN_007582e0, since Ghidra's static analysis
// found zero references to it) walks a dynamic array of module entries
// at [0x2A121D8 .. 0x2A121DC), calling each one's own vtable+0x80 method.
// The array itself is heap-allocated at runtime, so it wasn't captured by
// the static module-image memory dump imported into Ghidra (that only
// covers the module's own mapped image, not general heap state) -- read
// it live instead, from inside the already-injected DLL, to find the
// sibling module (camera/player/input) alongside the render module
// already traced to FUN_007582e0.
void DumpModuleRegistry()
{
    constexpr uintptr_t kArrayStartPtr = 0x2A121D8u;
    constexpr uintptr_t kArrayEndPtr = 0x2A121DCu;

    uint32_t arrayStart = 0, arrayEnd = 0;
    if (!SehSafeCopy(&arrayStart, Offset<void*>(kArrayStartPtr), sizeof(arrayStart)) ||
        !SehSafeCopy(&arrayEnd, Offset<void*>(kArrayEndPtr), sizeof(arrayEnd)))
    {
        MOHW_LOG(kLogFile, "DumpModuleRegistry: access violation reading array bounds pointers");
        return;
    }

    MOHW_LOG(kLogFile, "DumpModuleRegistry: array start=0x%X end=0x%X", arrayStart, arrayEnd);
    if (arrayEnd < arrayStart)
    {
        MOHW_LOG(kLogFile, "DumpModuleRegistry: end < start, array likely not populated yet -- try again later in gameplay");
        return;
    }

    int count = static_cast<int>((arrayEnd - arrayStart) / 4);
    MOHW_LOG(kLogFile, "DumpModuleRegistry: entry count=%d", count);
    if (count <= 0 || count > 4096)
    {
        MOHW_LOG(kLogFile, "DumpModuleRegistry: entry count out of sane range, aborting");
        return;
    }

    for (int i = 0; i < count; ++i)
    {
        uint32_t entry = 0;
        if (!SehSafeCopy(&entry, reinterpret_cast<void*>(arrayStart + i * 4), sizeof(entry)))
        {
            MOHW_LOG(kLogFile, "[%d] FAILED to read entry pointer", i);
            continue;
        }
        if (entry == 0)
        {
            MOHW_LOG(kLogFile, "[%d] entry=NULL", i);
            continue;
        }

        uint32_t moduleObjPtr = 0, iVar2 = 0;
        bool gotFields = SehSafeCopy(&moduleObjPtr, reinterpret_cast<void*>(entry + 0x84), sizeof(moduleObjPtr)) &&
                          SehSafeCopy(&iVar2, reinterpret_cast<void*>(entry + 0x88), sizeof(iVar2));
        if (!gotFields)
        {
            MOHW_LOG(kLogFile, "[%d] entry=0x%X -- FAILED reading entry+0x84/+0x88", i, entry);
            continue;
        }

        uint32_t vtablePtr = 0, targetFn = 0;
        bool gotVtable = moduleObjPtr != 0 &&
                          SehSafeCopy(&vtablePtr, reinterpret_cast<void*>(moduleObjPtr), sizeof(vtablePtr));
        bool gotTarget = gotVtable && vtablePtr != 0 &&
                          SehSafeCopy(&targetFn, reinterpret_cast<void*>(vtablePtr + 0x80), sizeof(targetFn));

        MOHW_LOG(kLogFile,
                  "[%d] entry=0x%X moduleObjPtr=0x%X iVar2=0x%X vtablePtr=0x%X vtable+0x80 target=0x%X", i, entry,
                  moduleObjPtr, iVar2, vtablePtr, gotTarget ? targetFn : 0);
    }
}

} // namespace

void CheckModuleRegistryDumpHotkey()
{
    if (g_moduleRegistryAlreadyDumped.load())
        return;
    if (!(GetAsyncKeyState(kModuleRegistryDumpHotkey) & 0x8000))
        return;

    bool expected = false;
    if (!g_moduleRegistryAlreadyDumped.compare_exchange_strong(expected, true))
        return;

    MOHW_LOG(kLogFile, "F10 pressed -- starting one-shot module registry dump");
    DumpModuleRegistry();
}

void CheckMemoryDumpHotkey()
{
    if (g_alreadyDumped.load())
        return;
    if (!(GetAsyncKeyState(kDumpHotkey) & 0x8000))
        return;

    // Claim the one-shot BEFORE dumping, not after -- the hotkey can read
    // as pressed across several consecutive frames before the dump
    // finishes, and this avoids two overlapping dumps racing on the same
    // output file.
    bool expected = false;
    if (!g_alreadyDumped.compare_exchange_strong(expected, true))
        return;

    MOHW_LOG(kLogFile, "F9 pressed -- starting one-shot live memory dump");
    DumpLiveModule();
}

} // namespace mohw
