#pragma once
// Live memory dump utility (task: branch off DX11 constant-buffer patching,
// find a stable engine function to hook directly via Ghidra instead).
//
// Ghidra's static analysis of the on-disk MOHW.exe found plenty of real
// functions -- just never at any of several addresses independently
// confirmed to be real, live-executing code via _ReturnAddress() captures
// in this project's own hooks. That pattern is consistent with this
// game's EA DRM/anti-tamper system (already known from this project's
// history to actively defend the executable -- see the earlier "Invalid
// License" import-table finding) encrypting or packing code on disk,
// only decrypting it into memory at runtime. Static analysis of the file
// literally cannot see code that's still encrypted there.
//
// This dumps the game's own ALREADY-DECRYPTED, live in-memory image
// straight from inside the already-injected proxy DLL, so it can be
// imported into Ghidra as ground truth instead of the (likely
// partially-encrypted) on-disk file.

namespace mohw {

// Checks for the dump hotkey (F9) and performs a one-shot dump of the
// live MOHW.exe module's memory to disk if pressed. Safe to call every
// frame (e.g. from the Present hook); internally a no-op after the first
// successful trigger for the lifetime of the process.
void CheckMemoryDumpHotkey();

// Checks for the module-registry dump hotkey (F10) and, if pressed, walks
// the live per-module dispatch array (task: find the sibling module --
// camera/player/input -- alongside the render module already traced to
// FUN_007582e0 via FUN_0050eba0's generic per-module dispatcher) and logs
// each entry's vtable+0x80 dispatch target address. Reads real heap
// memory directly (unlike the static Ghidra memory-image import, which
// only captured the module's own static image, not heap-allocated
// runtime state) -- press only once well into gameplay, after modules
// have had a chance to register. Safe to call every frame; a no-op after
// the first successful trigger.
void CheckModuleRegistryDumpHotkey();

} // namespace mohw
