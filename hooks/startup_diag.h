#pragma once
// Startup diagnostics (2026-10-31), for finding why the mod doesn't start on someone else's machine:
// mohwvr_startup.log (mohwvr_startup_driving.log from game\MOHWD.exe, which starts while MOHW.exe may still hold the
// main folder's files) reports what was found, in one short file a tester can send: mod version, the game's build and
// folders, Windows, displays, openvr_api.dll, SteamVR's install, the OpenXR runtime, the VR-related processes running,
// then what the master switch decided and why. Read-only: nothing is hooked or changed. advapi32 and ntdll functions
// are looked up at runtime, so this DLL's import table stays small (the game's DRM has objected to unusual imports).

namespace mohw {

// The file to MOHW_LOG startup findings to, for this executable. Needs g_drivingExe set.
const char* StartupLogFile();

// dllmain, once, as soon as the executable is known -- before the build check, so a failed one gets reported too.
void WriteStartupReport();

// Whether a process with this file name is running; *pid (optional) = its id.
bool IsProcessRunning(const wchar_t* exeName, unsigned long* pid = nullptr);

// A blocking startup step, for the watchdog: a step still running after 15 s and 60 s is reported (VR_Init has hung
// forever with an old openvr_api.dll), and its name is the last thing logged if the process dies in it. quiet: don't
// log the start (the 5 s retries).
void StartStartupWatchdog();
void BeginStartupStep(const char* name, bool quiet = false);
void EndStartupStep();

} // namespace mohw
