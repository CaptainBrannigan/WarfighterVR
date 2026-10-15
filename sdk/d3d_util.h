#pragma once
// Small D3D11 helpers shared by the hooks.

#include <d3d11.h>

namespace mohw {

// Whether tex was created on device. The game can tear its device down and create a new one at a level change (the
// proxy is pinned so it survives that, see proxy_dll/dllmain.cpp), so every cached texture checks this as well as its
// size: a texture from the old device can't be used with the new one.
inline bool TextureOnDevice(ID3D11Texture2D* tex, ID3D11Device* device)
{
    if (!tex || !device)
        return false;
    ID3D11Device* owner = nullptr;
    tex->GetDevice(&owner);
    bool same = owner == device;
    if (owner)
        owner->Release();
    return same;
}

} // namespace mohw
