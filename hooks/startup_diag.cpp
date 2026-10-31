#include "startup_diag.h"

#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/version.h"

#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace mohw {
namespace {

std::atomic<const char*> g_stepName{nullptr};
std::atomic<unsigned long long> g_stepStartMs{0};

std::string ExePath()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    return path;
}

std::string OwnDllPath()
{
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&StartupLogFile), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    return path;
}

std::string FolderOf(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool FileExists(const std::string& path)
{
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

long long FileSize(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
        return -1;
    return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::string FileDate(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    SYSTEMTIME st{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data) ||
        !FileTimeToSystemTime(&data.ftLastWriteTime, &st))
        return "?";
    char text[32];
    snprintf(text, sizeof(text), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return text;
}

// A file's bitness from its PE header, or why not.
std::string PeMachine(const std::string& path)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return "unreadable";
    std::string result = "not a PE file";
    IMAGE_DOS_HEADER dos{};
    DWORD got = 0;
    if (ReadFile(file, &dos, sizeof(dos), &got, nullptr) && got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(file, dos.e_lfanew + 4, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER)
    {
        IMAGE_FILE_HEADER header{};
        if (ReadFile(file, &header, sizeof(header), &got, nullptr) && got == sizeof(header))
            result = header.Machine == IMAGE_FILE_MACHINE_I386    ? "32-bit (right)"
                     : header.Machine == IMAGE_FILE_MACHINE_AMD64 ? "64-bit (WRONG: needs SteamVR's bin\\win32 copy)"
                                                                  : "unknown machine";
    }
    CloseHandle(file);
    return result;
}

// 1 elevated, 0 not, -1 couldn't tell. advapi32 looked up at runtime.
int IsProcessElevated(HANDLE process)
{
    using OpenTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
    using GetTokenInfoFn = BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
    HMODULE advapi = LoadLibraryA("advapi32.dll");
    if (!advapi)
        return -1;
    auto openToken = reinterpret_cast<OpenTokenFn>(GetProcAddress(advapi, "OpenProcessToken"));
    auto getInfo = reinterpret_cast<GetTokenInfoFn>(GetProcAddress(advapi, "GetTokenInformation"));
    HANDLE token = nullptr;
    if (!openToken || !getInfo || !openToken(process, TOKEN_QUERY, &token))
        return -1;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    int result = -1;
    if (getInfo(token, TokenElevation, &elevation, sizeof(elevation), &size))
        result = elevation.TokenIsElevated ? 1 : 0;
    CloseHandle(token);
    return result;
}

const char* ElevationText(int elevated)
{
    return elevated == 1 ? "as administrator" : elevated == 0 ? "not as administrator" : "elevation unknown";
}

std::string RegistryText(const char* key, const char* value, DWORD viewFlag)
{
    using RegGetValueFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
    HMODULE advapi = LoadLibraryA("advapi32.dll");
    auto regGetValue = advapi ? reinterpret_cast<RegGetValueFn>(GetProcAddress(advapi, "RegGetValueA")) : nullptr;
    if (!regGetValue)
        return "?";
    char text[MAX_PATH * 2]{};
    DWORD size = sizeof(text);
    if (regGetValue(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ | viewFlag, nullptr, text, &size) != ERROR_SUCCESS)
        return "(none)";
    return text;
}

std::string ReadTextFile(const std::string& path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
        return std::string();
    std::string text;
    char buffer[4096];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
        text.append(buffer, got);
    fclose(f);
    return text;
}

// The strings of one array in openvrpaths.vrpath, whose format is plain enough not to need a JSON parser.
std::vector<std::string> JsonStringArray(const std::string& text, const char* key)
{
    std::vector<std::string> out;
    size_t pos = text.find(std::string("\"") + key + "\"");
    if (pos == std::string::npos)
        return out;
    pos = text.find('[', pos);
    size_t end = pos == std::string::npos ? pos : text.find(']', pos);
    while (pos != std::string::npos && end != std::string::npos)
    {
        size_t open = text.find('"', pos);
        if (open == std::string::npos || open > end)
            break;
        std::string value;
        size_t i = open + 1;
        for (; i < text.size() && text[i] != '"'; ++i)
        {
            if (text[i] == '\\' && i + 1 < text.size())
                ++i;
            value += text[i];
        }
        out.push_back(value);
        pos = i + 1;
    }
    return out;
}

void ReportMod(const char* log)
{
    MOHW_LOG(log, "MOHWVR %s, built %s %s", MOHWVR_VERSION, __DATE__, __TIME__);
    std::string dll = OwnDllPath();
    MOHW_LOG(log, "mod DLL: %s (%lld bytes)", dll.c_str(), FileSize(dll));
    bool fallback = _stricmp(DataFolder().c_str(), ModuleFolderPath("").c_str()) != 0;
    MOHW_LOG(log, "settings and logs: %s%s", DataFolder().c_str(),
             fallback ? " -- the game folder isn't writable, so they're here instead" : "");
    std::string exeFolder = FolderOf(ExePath());
    if (!g_drivingExe)
    {
        long long size = FileSize(exeFolder + "game\\dxgi.dll");
        MOHW_LOG(log, "driving missions' copy (game\\dxgi.dll): %s",
                 size < 0                  ? "MISSING (those missions run flat)"
                 : size == FileSize(dll) ? "present, same size"
                                         : "present, DIFFERENT SIZE (an older copy?)");
    }
    const char* others[] = {"d3d11.dll",     "d3d9.dll",      "dinput8.dll", "version.dll", "winmm.dll",
                            "xinput1_3.dll", "ReShade.ini",   "ReShade32.dll", "d3dx.ini"};
    for (const char* name : others)
        if (FileExists(exeFolder + name))
            MOHW_LOG(log, "possible conflict in the game folder: %s", name);
}

void ReportGame(const char* log)
{
    std::string exe = ExePath();
    MOHW_LOG(log, "game: %s", exe.c_str());
    auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    DWORD imageSize = nt->OptionalHeader.SizeOfImage, expected = g_drivingExe ? MOHWD_SIZE : MOHW_MP_SIZE;
    MOHW_LOG(log, "game build: image size 0x%lX, the mod needs 0x%lX -- %s; exe %lld bytes, dated %s, PE timestamp %08lX",
             imageSize, expected, imageSize == expected ? "MATCH" : "DIFFERENT BUILD", FileSize(exe),
             FileDate(exe).c_str(), nt->FileHeader.TimeDateStamp);
    MOHW_LOG(log, "game process: running %s", ElevationText(IsProcessElevated(GetCurrentProcess())));
}

void ReportWindows(const char* log)
{
    struct VersionInfo
    {
        ULONG size, major, minor, build, platform;
        WCHAR servicePack[128];
    } version{};
    version.size = sizeof(version);
    using RtlGetVersionFn = LONG(WINAPI*)(VersionInfo*);
    auto getVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion"));
    if (getVersion && getVersion(&version) == 0)
        MOHW_LOG(log, "Windows: %lu.%lu build %lu", version.major, version.minor, version.build);
    DISPLAY_DEVICEA device{};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &device, 0); ++i)
        if (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
            MOHW_LOG(log, "display adapter: %s (%s)%s", device.DeviceString, device.DeviceName,
                     (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " primary" : "");
}

void ReportSteamVr(const char* log)
{
    std::string openvr = ModuleFolderPath("openvr_api.dll");
    long long size = FileSize(openvr);
    if (size < 0)
        MOHW_LOG(log, "openvr_api.dll: MISSING at %s -- copy it from SteamVR's bin\\win32 folder", openvr.c_str());
    else
        MOHW_LOG(log, "openvr_api.dll: %lld bytes, dated %s, %s", size, FileDate(openvr).c_str(),
                 PeMachine(openvr).c_str());

    char local[MAX_PATH]{};
    GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
    std::string vrpath = std::string(local) + "\\openvr\\openvrpaths.vrpath";
    std::string text = ReadTextFile(vrpath);
    if (text.empty())
    {
        MOHW_LOG(log, "SteamVR: no %s -- SteamVR has never run for this user", vrpath.c_str());
    }
    else
    {
        for (const std::string& runtime : JsonStringArray(text, "runtime"))
        {
            std::string client = runtime + "\\bin\\vrclient.dll";
            std::string version = ReadTextFile(runtime + "\\bin\\version.txt");
            while (!version.empty() && (version.back() == '\n' || version.back() == '\r'))
                version.pop_back();
            MOHW_LOG(log, "SteamVR runtime: %s, build %s", runtime.c_str(), version.empty() ? "?" : version.c_str());
            if (FileExists(client))
                MOHW_LOG(log, "SteamVR 32-bit client (bin\\vrclient.dll): present, dated %s", FileDate(client).c_str());
            else
                MOHW_LOG(log, "SteamVR 32-bit client (bin\\vrclient.dll): MISSING -- needs SteamVR 2.17 or newer");
            long long shipped = FileSize(runtime + "\\bin\\win32\\openvr_api.dll");
            if (size >= 0 && shipped >= 0 && shipped != size)
                MOHW_LOG(log, "openvr_api.dll differs from SteamVR's own (%lld bytes): an old copy can hang VR_Init",
                         shipped);
        }
        for (const std::string& driver : JsonStringArray(text, "external_drivers"))
            MOHW_LOG(log, "SteamVR external driver: %s", driver.c_str());
    }
    const char* xrKey = "SOFTWARE\\Khronos\\OpenXR\\1";
    MOHW_LOG(log, "OpenXR runtime for 64-bit apps: %s",
             RegistryText(xrKey, "ActiveRuntime", RRF_SUBKEY_WOW6464KEY).c_str());
    MOHW_LOG(log, "OpenXR runtime for 32-bit apps: %s (for reference: the mod doesn't use OpenXR)",
             RegistryText(xrKey, "ActiveRuntime", RRF_SUBKEY_WOW6432KEY).c_str());
}

void ReportProcesses(const char* log)
{
    const wchar_t* names[] = {L"vrserver.exe",
                              L"vrmonitor.exe",
                              L"vrcompositor.exe",
                              L"VirtualDesktop.Streamer.exe",
                              L"VirtualDesktop.Server.exe",
                              L"VirtualDesktop.Service.exe",
                              L"OVRServer_x64.exe",
                              L"EADesktop.exe",
                              L"Origin.exe",
                              L"steam.exe"};
    for (const wchar_t* name : names)
    {
        unsigned long pid = 0;
        if (!IsProcessRunning(name, &pid))
            continue;
        int elevated = -1;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process)
        {
            elevated = IsProcessElevated(process);
            CloseHandle(process);
        }
        MOHW_LOG(log, "running: %ls (%s)", name, ElevationText(elevated));
    }
}

} // namespace

const char* StartupLogFile()
{
    return g_drivingExe ? "mohwvr_startup_driving.log" : "mohwvr_startup.log";
}

bool IsProcessRunning(const wchar_t* exeName, unsigned long* pid)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        if (_wcsicmp(entry.szExeFile, exeName) == 0)
        {
            found = true;
            if (pid)
                *pid = entry.th32ProcessID;
        }
    CloseHandle(snapshot);
    return found;
}

void WriteStartupReport()
{
    const char* log = StartupLogFile();
    ReportMod(log);
    ReportGame(log);
    ReportWindows(log);
    ReportSteamVr(log);
    ReportProcesses(log);
}

void BeginStartupStep(const char* name, bool quiet)
{
    g_stepStartMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_stepName.store(name, std::memory_order_release);
    if (!quiet)
        MOHW_LOG(StartupLogFile(), "step: %s ...", name);
}

void EndStartupStep()
{
    const char* name = g_stepName.exchange(nullptr, std::memory_order_acq_rel);
    unsigned long long took = GetTickCount64() - g_stepStartMs.load(std::memory_order_relaxed);
    if (name && took >= 1000)
        MOHW_LOG(StartupLogFile(), "step done: %s (%llu ms)", name, took);
}

void StartStartupWatchdog()
{
    std::thread([] {
        const char* last = nullptr;
        int warned = 0;
        for (;;)
        {
            Sleep(1000);
            const char* name = g_stepName.load(std::memory_order_acquire);
            if (name != last)
            {
                last = name;
                warned = 0;
            }
            if (!name)
                continue;
            unsigned long long took = GetTickCount64() - g_stepStartMs.load(std::memory_order_relaxed);
            if ((warned == 0 && took >= 15000) || (warned == 1 && took >= 60000))
            {
                ++warned;
                MOHW_LOG(StartupLogFile(), "STILL IN STEP after %llu s, possibly hung: %s", took / 1000, name);
            }
        }
    }).detach();
}

} // namespace mohw
