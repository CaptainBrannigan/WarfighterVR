#pragma once
// Minimal file logger shared by proxy_dll and investigation.
//
// Was originally open-append-close per call (fine for Phase 0/1's low log
// volume). Changed (2026-08-03) to keep one FILE* open per log file for the
// process lifetime instead, guarded by a mutex, with an fflush() after every
// write so a crash still can't lose recently-written lines (the property
// open-append-close was incidentally providing). Motivated by a real,
// recurring live crash (d3d11.dll+0x270d4, always the same fault offset)
// during heavy stereo/SRV-redirection logging -- gating individual log call
// sites got the volume down but didn't stop the crash, which means the per-
// call fopen/fclose overhead itself (a real, if unproven, contributor: two
// CreateFile-class syscalls per line, on the render thread, potentially
// contended with antivirus/indexing) was worth removing regardless. This
// also means verbose diagnostic logging can be safely re-enabled without
// reintroducing that specific I/O cost.

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace mohw {

// Log files truncated at first-open-this-session (see Log()'s comment)
// are the default -- these are all live-debugging aids tied to whatever's
// currently being investigated, where the previous session's content is
// just clutter once you've moved past that investigation, not history
// worth keeping. Add a file name here to opt it OUT of truncation
// (preserve full history across every relaunch) if that ever changes --
// none currently need it.
inline std::unordered_set<std::string>& PreserveHistoryLogFiles()
{
    static std::unordered_set<std::string> files;
    return files;
}

// A file next to the DLL -- for the driving copy in game\, next to MOHW.exe. For files the user places there
// (openvr_api.dll); what the mod writes goes through LogFilePath.
inline std::string ModuleFolderPath(const char* fileName)
{
    char modulePath[MAX_PATH]{};
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ModuleFolderPath),
        &hSelf);
    GetModuleFileNameA(hSelf, modulePath, MAX_PATH);

    std::string path(modulePath);
    size_t slash = path.find_last_of("\\/");
    path = (slash == std::string::npos) ? "" : path.substr(0, slash + 1);
    // The driving missions run in game\MOHWD.exe, with a copy of this DLL next to it (2026-10-14): its logs, settings
    // and SteamVR binding files go to the main folder, so both executables share them.
    if (path.size() > 6 && _stricmp(path.c_str() + path.size() - 6, "\\game\\") == 0)
    {
        std::string parent = path.substr(0, path.size() - 5);
        if (GetFileAttributesA((parent + "MOHW.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
            path = parent;
    }
    path += fileName;
    return path;
}

inline bool FolderIsWritable(const std::string& folder)
{
    HANDLE file = CreateFileA((folder + "mohwvr_write_test.tmp").c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(file);
    return true;
}

// Where the mod writes: its logs, settings and SteamVR binding files. Next to the DLL, or %LOCALAPPDATA%\MOHWVR when
// that isn't writable (2026-10-31): the game runs asInvoker, so under Program Files, the EA app's default, it can't
// write there -- which lost every log, and the action manifest the controllers need.
inline const std::string& DataFolder()
{
    static const std::string folder = [] {
        std::string own = ModuleFolderPath("");
        if (FolderIsWritable(own))
            return own;
        char local[MAX_PATH]{};
        DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return own;
        std::string fallback = std::string(local) + "\\MOHWVR\\";
        CreateDirectoryA(fallback.c_str(), nullptr);
        return FolderIsWritable(fallback) ? fallback : own;
    }();
    return folder;
}

inline std::string LogFilePath(const char* fileName)
{
    return DataFolder() + fileName;
}

// Function-local statics in an inline function are merged across every TU
// that includes this header (guaranteed since C++17, which this project
// already targets) -- so this is the one shared handle cache/mutex for the
// whole proxy DLL, not a per-TU copy.
inline std::unordered_map<std::string, FILE*>& LogFileHandles()
{
    static std::unordered_map<std::string, FILE*> handles;
    return handles;
}

inline std::mutex& LogMutex()
{
    static std::mutex m;
    return m;
}

// A file past this size moves to <name>.older.log and starts again (2026-10-31): the end of a session, which is what
// matters after a crash, is always kept, and a log costs at most twice this (mohwvr_fovscale.log reached 478 MB in one
// session).
constexpr long long kMaxLogFileBytes = 32 * 1024 * 1024;

inline std::unordered_map<std::string, long long>& LogFileSizes()
{
    static std::unordered_map<std::string, long long> sizes;
    return sizes;
}

inline void Log(const char* fileName, const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(LogMutex());

    auto& handles = LogFileHandles();
    auto it = handles.find(fileName);
    FILE* f = (it != handles.end()) ? it->second : nullptr;
    if (!f)
    {
        // CreateFile+FILE_SHARE_READ instead of fopen_s -- fopen's CRT-level
        // sharing denies other processes read access while this handle is
        // open, which blocked reading the log live during gameplay (a text
        // editor, or Claude's own tools, both hit sharing-violation/"device
        // busy" errors). FILE_SHARE_READ lets any other process read freely
        // while this process keeps appending.
        //
        // CREATE_ALWAYS (truncate) vs OPEN_ALWAYS (keep existing content):
        // only decided once per file per process lifetime (the handle is
        // cached above for every write after this), so CREATE_ALWAYS
        // truncates once per fresh launch and appends normally for the
        // rest of the session -- see PreserveHistoryLogFiles' comment for
        // why that's the default. Without this these files never rotate
        // and grow unbounded across every relaunch forever (one hit 147K
        // lines / 63% of it a single throttling bug's output from one
        // 21-second window -- see constantbuffer_hook.cpp's
        // LogPoseTableIfLockedEntity history).
        std::string path = LogFilePath(fileName);
        bool preserveHistory = PreserveHistoryLogFiles().count(fileName) != 0;
        // The previous session's file is kept as <name>.previous.log (2026-09-30): a crash followed by a relaunch
        // used to truncate the crashed session's logs before they could be read.
        if (!preserveHistory)
        {
            std::string previous = path;
            size_t dot = previous.find_last_of('.');
            previous = (dot == std::string::npos ? previous : previous.substr(0, dot)) + ".previous.log";
            MoveFileExA(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                     preserveHistory ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return;
        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hFile), _O_APPEND | _O_TEXT);
        if (fd == -1)
        {
            CloseHandle(hFile);
            return;
        }
        f = _fdopen(fd, "a"); // _O_APPEND on the fd is what actually guarantees append-only writes
        if (!f)
        {
            _close(fd); // also closes hFile
            return;
        }
        handles[fileName] = f;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    int written = fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    written += vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fflush(f); // keep durable for crash forensics without paying open/close cost per call

    long long& size = LogFileSizes()[fileName];
    size += written > 0 ? written + 1 : 1;
    if (size > kMaxLogFileBytes)
    {
        fclose(f);
        handles.erase(fileName);
        size = 0;
        std::string path = LogFilePath(fileName);
        size_t dot = path.find_last_of('.');
        std::string older = (dot == std::string::npos ? path : path.substr(0, dot)) + ".older.log";
        MoveFileExA(path.c_str(), older.c_str(), MOVEFILE_REPLACE_EXISTING); // the next line opens a new one
    }
}

} // namespace mohw

#define MOHW_LOG(file, ...) ::mohw::Log(file, __VA_ARGS__)
