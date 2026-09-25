# Phase 2C, high-level approach: findings (abandoned in favor of low-level)

## Question

Does `GameRenderer` expose a single callable "render everything" entry point
(hinted at by `createUpdateJob`/`joinUpdateJob` in the SDK) that could be
called twice per frame -- once per eye, with the camera patched between
calls -- letting the engine's own code redraw the scene naturally instead of
us capturing/replaying raw D3D11 draw calls?

## Method and findings, in order

1. **First attempt** hooked `createUpdateJob`/`joinUpdateJob` (`IGameRenderer`
   vtable slots 4/5, confirmed via the original header's own `V: 0x10`/
   `V: 0x14` comments) with ordinary `__thiscall` C++ detours assuming the
   header's parameter counts (5 stack params / 0 stack params). This crashed
   the game **twice, at the identical fault address** (`0xc0000005` @
   RVA `0x004ab5a9` both times) -- a deterministic bug, not a fluke.
   `createUpdateJob` never logged a single call despite unconditional
   logging for its first 10 calls, while `joinUpdateJob` logged once then
   the game died shortly after.
2. **Root cause hypothesis:** if the assumed stack-parameter count for
   either function is wrong, a normal compiler-generated `__thiscall`
   epilogue (`ret N` for the assumed N) cleans the wrong number of bytes off
   the stack on return, corrupting the real caller's stack frame -- a
   corruption that manifests downstream, not necessarily at the call site
   itself. This is the same category of risk Phase 1 already hit once
   (`RenderView`'s size differing from the old header) -- vtable/class
   layouts from the header that produced the *original* offsets aren't
   necessarily valid for this build's *internals*, only the confirmed
   top-level addresses are trustworthy without re-verification.
3. **Verification build:** rewrote both detours as `__declspec(naked)`
   functions touching *only* `ECX` (the `this` pointer -- guaranteed correct
   for any thiscall method regardless of its true stack parameter count)
   and tail-`JMP`ing (not `CALL`ing) into the trampoline, never asserting or
   relying on any particular parameter count. **This ran with no crash**
   across a multi-minute session -- confirming the hypothesis: the original
   crash was the stack-cleanup mismatch, not a wrong vtable slot or a
   MinHook/trampoline problem with these specific functions.
4. **What the safe verification build revealed:**
   - `joinUpdateJob` fires **exactly once per frame**, in perfect lockstep
     with `Present` (identical call-count timestamps: `joinUpdateJob #300`
     and `Present #300` at the same millisecond, same for `#600`/`#900`).
   - `createUpdateJob` fires **once, total**, near startup -- a one-time
     setup call, not a per-frame one.
   - This means neither function is the "render this frame" trigger we
     hoped for: `createUpdateJob` doesn't run per-frame at all, and
     `joinUpdateJob` is a *wait for already-scheduled work*, not a
     *start new work* call -- invoking it a second time within the same
     frame would very likely just return immediately rather than trigger a
     fresh render pass.
5. **Traced the actual per-frame caller:** extended the naked hooks to also
   capture the return address (read directly off the stack at entry, before
   pushing anything -- always the first value on the stack for a function
   entered via `CALL`). Result: `joinUpdateJob` is called from the *exact
   same address every single frame* -- `RVA+0x496DE4` -- a single, stable
   call site. This is a strong, precise lead for whatever the real
   per-frame orchestrator function is.
6. **Dead end (tooling, not a technical wall):** no disassembler is
   installed on this machine (checked for IDA, Ghidra, x64dbg, radare2 --
   none present). Reading what the code at `RVA+0x496DE4` and its
   containing function actually does would need one; hand-decoding raw x86
   opcodes without a real tool is possible but slow and error-prone, and the
   binary is a Retail/optimized build (confirmed via
   `Engine.BuildInfo.MOHW_Win32_Retail_dll.dll` in the game folder), likely
   with frame-pointer omission making "scan backward for a prologue"
   unreliable too.

## Decision

User chose to abandon the high-level approach here (rather than install
Ghidra or attempt manual hex analysis) and proceed with Phase 2C's low-level
draw-call capture/replay instead, which only depends on the constant buffer
already verified empirically in Phase 2B -- no disassembly required.

## What's preserved for later

- `hooks/framerender_hook.cpp/h` remain in the tree as a working, safe
  (naked/ECX-only, no-crash-confirmed) investigation tool, but are no longer
  wired into `proxy_dll`'s active hook set (`dllmain.cpp`). If a
  disassembler becomes available later, `RVA+0x400000+0x496DE4` is exactly
  where to start reading.
- The naked/ECX-only tail-JMP technique itself is reusable for any future
  need to safely hook a C++ virtual method whose true signature isn't
  confirmed -- documented here since it isn't otherwise written down
  anywhere in the codebase.
