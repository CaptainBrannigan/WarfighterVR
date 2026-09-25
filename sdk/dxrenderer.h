#pragma once
// Frostbite 2's DX11 renderer singleton and its settings block. This is the
// object that owns the real ID3D11Device/DeviceContext/IDXGISwapChain the
// game created -- useful for cross-checking the pointers our dxgi.dll proxy
// hook already sees, and for reading window/back-buffer size.

#include "mohw_common.h"
#include "mohw_offsets.h"

#include <d3d11.h>

namespace mohw {

class DxRenderSettings
{
public:
    char unknown0[32];               // 0x0000
    int32_t Width;                   // 0x0020
    int32_t Height;                  // 0x0024
    char unknown40[48];               // 0x0028
    int32_t ResX;                    // 0x0058
    int32_t ResY;                    // 0x005C
    char unknown96[120];              // 0x0060
    ID3D11Device* m_device;          // 0x00D8
    ID3D11DeviceContext* m_deviceContext; // 0x00DC
    char unknown224[20];              // 0x00E0
    IDXGISwapChain* m_swapChain;     // 0x00F4
    char unknown248[4];               // 0x00F8
};

class DxRenderer
{
public:
    DxRenderSettings* m_settings;
    PAD(0x4);                     // 0x00
    uint32_t m_nFrameCounter;      // 0x08
    BOOL m_bFrameInProgress;       // 0x0C
    HWND m_hWnd;                   // 0x10
    PAD(0x4);                     // 0x14
    BYTE m_bFullscreenWanted;      // 0x18
    BYTE m_bFullscreenActive;      // 0x19
    BYTE m_bMinimized;             // 0x1A
    BYTE m_bMinimizing;            // 0x1B
    BYTE m_bResizing;              // 0x1C
    BYTE m_bOccluded;              // 0x1D
    BYTE m_bVSync;                 // 0x1E
    PAD(0x1);                     // 0x1F
    RenderScreenInfo m_screenInfo; // 0x20
    PAD(0xA4);                    // 0x34
    ID3D11Device* pDevice;         // 0xD8
    ID3D11DeviceContext* pContext; // 0xDC
    PAD(0x14);                    // 0xE0
    IDXGISwapChain* pSwapChain;    // 0xF4

public:
    static DxRenderer* Singleton() { return *Offset<DxRenderer**>(OFFSET_DXRENDERER); }
};

} // namespace mohw
