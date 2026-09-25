#pragma once
// Phase 2A: hooks IDXGISwapChain::Present/ResizeBuffers. The swapchain
// vtable is shared per-driver across every swapchain instance, so we obtain
// the real function pointers via a throwaway dummy device+swapchain at
// install time, hook those, then discard the dummy -- the patched code
// applies to the game's own real swapchain without ever having to intercept
// its creation.

namespace mohw {

bool InstallPresentHook();
void RemovePresentHook();

} // namespace mohw
