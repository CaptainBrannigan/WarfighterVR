#pragma once
// Camera/view state. This is the primary hook surface for VR: RenderViewDesc
// already has stereoSeparation/stereoConvergence, and GameRenderViewParams
// has a full second RenderView slot (secondaryStreamingView) gated by
// secondaryStreamingViewEnable -- see docs/native_stereo_investigation.md for
// whether that second view is actually wired to a real second render pass.

#include "mohw_common.h"
#include "mohw_offsets.h"
#include "dxrenderer.h"

namespace mohw {

class RenderViewDesc
{
public:
    LinearTransform transform; // 0x00
    int32_t type;               // 0x40
    PAD(0x4);                   // 0x44
    float fovY;                  // 0x48
    float defaultFovY;           // 0x4C
    float nearPlane;             // 0x50
    float farPlane;              // 0x54
    float aspect;                 // 0x58
    float orthoWidth;             // 0x5C
    float orthoHeight;            // 0x60
    float stereoSeparation;       // 0x64 -- native stereo: eye separation
    float stereoConvergence;      // 0x68 -- native stereo: convergence distance
    Vec2 viewportOffset;          // 0x6C
    Vec2 viewportScale;           // 0x74
};

class RenderView
{
public:
    RenderViewDesc m_desc;    // 0x00
    PAD(0x14);                // 0x7C
    int32_t m_dirtyFlags;      // 0x80
    PAD(0x16C);                // 0x84
    float m_fovX;               // 0x1F0
    float m_depthToWidthRatio;  // 0x1F4
    float m_fovScale;            // 0x1F8
    float m_fovScaleSqr;         // 0x1FC
    LinearTransform m_viewMatrix;                     // 0x200
    LinearTransform m_viewMatrixTranspose;             // 0x240
    LinearTransform m_viewMatrixInverse;               // 0x280
    LinearTransform m_projectionMatrix;                // 0x2C0
    LinearTransform m_viewMatrixAtOrigin;              // 0x300
    LinearTransform m_projectionMatrixTranspose;       // 0x340
    LinearTransform m_projectionMatrixInverse;         // 0x380
    LinearTransform m_viewProjectionMatrix;            // 0x3C0
    LinearTransform m_viewProjectionMatrixTranspose;   // 0x400
    LinearTransform m_viewProjectionMatrixInverse;     // 0x440

public:
    // Recomputes aspect from the current window size and calls the engine's
    // own matrix-rebuild routine. Calling this on our own is mainly useful
    // for the Phase 1 investigation DLL; in the real hook we let the game
    // call this itself and intercept it instead of driving it ourselves.
    BOOL Update()
    {
        DxRenderer* dxRenderer = DxRenderer::Singleton();
        if (dxRenderer == nullptr)
            return FALSE;

        float screenX = static_cast<float>(dxRenderer->m_screenInfo.nWindowWidth);
        float screenY = static_cast<float>(dxRenderer->m_screenInfo.nWindowHeight);
        m_desc.aspect = screenX / screenY;

        using UpdateMatricesFn = void(__fastcall*)(RenderView*, LPVOID);
        auto updateMatrices = Offset<UpdateMatricesFn>(OFFSET_UPDATEMATRICES);
        updateMatrices(this, nullptr);
        return TRUE;
    }

    BOOL GetOriginFromMatrix(Vec3* out) const { return m_viewMatrixInverse.GetOrigin(out); }
};

class GameRenderViewParams
{
public:
    RenderView view;                        // 0x00
    RenderView prevView;                    // 0x480
    RenderView secondaryStreamingView;      // 0x900
    int32_t secondaryStreamingViewEnable;    // 0xD80
    PAD(0xC);                                // 0xD84
    LinearTransform firstPersonTransform;    // 0xD90
};

class IGameRenderer
{
public:
    virtual void Function0();
    virtual void Function1();
    virtual void init();
    virtual void postLoadInit();
    virtual void createUpdateJob(float, float, LPVOID, LPVOID, LPVOID);
    virtual void joinUpdateJob();
    virtual void setJobEnable(BOOL);
    virtual BOOL getJobEnable();
    virtual void onUnloadLevel();
    virtual int getAffinity();
    virtual Vec2 getResolution(); // originally D3DXVECTOR2; D3DX is deprecated/unavailable, layout-compatible as Vec2
    virtual void addModule(int, LPVOID);
    virtual void removeModule(int);
    virtual const void* getSettings();
    virtual void* getScreenRenderer();
    virtual void updatePerfOverlay(BOOL, float);
    virtual void resetPerfOverlay();
    virtual BOOL isFadeInAllowed();
    virtual void setVsyncEnable(BOOL);
    virtual BOOL getVsyncEnable();
    virtual void setPresentEnable(BOOL);
    virtual void getAdapterInformation(LPVOID, UINT&, UINT&);
    virtual const RenderView& getTempDeprecatedView();

    int m_refCount; // 0x04
};

class GameRenderer : public IGameRenderer
{
public:
    PAD(0x48);                       // 0x08
    GameRenderViewParams m_viewParams; // 0x50

public:
    static GameRenderer* Singleton() { return *Offset<GameRenderer**>(OFFSET_GAMERENDERER); }
};

} // namespace mohw
