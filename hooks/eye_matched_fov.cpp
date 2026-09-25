#include "eye_matched_fov.h"

#include "companion_bridge.h"
#include "../sdk/logging.h"
#include "../shared/eye_frustum.h"
#include "../sdk/settings.h"
#include "projection_aspect_hook.h"
#include "alternating_eye.h"

#include <windows.h>
#include <atomic>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_fovscale.log";
constexpr float kRadToDeg = 57.29577951f;
constexpr float kMaxFovYRad = 3.05f; // same safety clamp as the old method: fovY near pi breaks the tan()-based projection

// Last fovY value actually written to transformPtr+0x48 (RenderViewDesc::fovY), regardless of which branch wrote it
// (true-frustum's fovNew or the enclosing/inner-edge fallthrough's newFovY). Added 2026-09-23 so
// constantbuffer_hook.cpp's ExpectedProjM11 can use OUR live value instead of GameRenderer::Singleton()'s own
// m_viewParams.view.m_desc.fovY copy -- that field is bulk-copied from a DIFFERENT engine structure once per
// simulation tick (per sdk/mohw_offsets.h's OFFSET_UPDATEVIEWCONSTANTS comment), not from what we write here, so it
// never reflected our eye-matched override -- confirmed as the reason the buffer-identification signature match
// (which computes its "expected" value from that stale copy) never succeeded once eye-matched FOV went live.
std::atomic<float> g_lastWrittenFovY{0.0f};

bool SehReadFloat(const float* p, float* out)
{
    __try
    {
        *out = *p;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Copies n floats out of possibly-unmapped memory.
bool SehReadFloats(const float* p, float* out, int n)
{
    __try
    {
        for (int i = 0; i < n; ++i)
            out[i] = p[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// DIAGNOSTIC (2026-09-20): how does the game turn fovY/aspect into its projection? Logs the RenderView's own
// derived fields (fovX, projection matrix rows) next to fovY/aspect so we can tell which axis +0x48 drives and
// what aspect the projection really uses. Layout per sdk/renderview.h (unverified): m_fovX +0x1F0,
// m_projectionMatrix +0x2C0. Read-only.
void LogProjectionDiagnostic(void* transformPtr)
{
    static std::atomic<unsigned long long> nextLogMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogMs.load(std::memory_order_relaxed);
    if (now < allowed || !nextLogMs.compare_exchange_strong(allowed, now + 2000, std::memory_order_relaxed))
        return;
    unsigned char* base = reinterpret_cast<unsigned char*>(transformPtr);
    float desc[8] = {}; // +0x48 fovY, +0x4C defFov, +0x50 near, +0x54 far, +0x58 aspect, +0x5C, +0x60, +0x64
    float derived[4] = {}; // +0x1F0 fovX, depthToWidth, fovScale, fovScaleSqr
    float proj[16] = {};
    bool ok = SehReadFloats(reinterpret_cast<const float*>(base + 0x48), desc, 8) &&
              SehReadFloats(reinterpret_cast<const float*>(base + 0x1F0), derived, 4) &&
              SehReadFloats(reinterpret_cast<const float*>(base + 0x2C0), proj, 16);
    if (!ok)
        return;
    MOHW_LOG(kLogFile,
              "PROJ DIAG this=%p fovY=%.4f defFovY=%.4f near=%.3f far=%.1f aspect=%.4f | fovX=%.4f depthToWidth=%.4f fovScale=%.4f | "
              "proj rows: [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f]",
              transformPtr, desc[0], desc[1], desc[2], desc[3], desc[4], derived[0], derived[1], derived[2], proj[0], proj[1],
              proj[2], proj[3], proj[4], proj[5], proj[6], proj[7], proj[8], proj[9], proj[10], proj[11], proj[12], proj[13],
              proj[14], proj[15]);
}

bool SehWriteFloat(float* p, float v)
{
    __try
    {
        *p = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

} // namespace

bool ApplyEyeMatchedFov(void* transformPtr)
{
    LogProjectionDiagnostic(transformPtr);
    // (An Insert-key bypass used to live here; removed 2026-09-20 -- Insert is the alternating-eye
    // toggle in alternating_eye.cpp, so pressing it also flipped and SAVED that setting.)
    mohwvr::ipc::HmdViewBlock hmd{};
    if (!GetHmdView(&hmd))
        return false;

    // Logged in THIS file (not the companion's own "view mode -> N" line in mohwvr_companion.log) so a view-mode switch
    // and the TRUE FRUSTUM MATCH/CHANGED lines it triggers land in one timeline, no cross-file correlation needed.
    {
        static std::atomic<long> lastLoggedMode{-1};
        long mode = hmd.viewMode;
        long prev = lastLoggedMode.exchange(mode, std::memory_order_relaxed);
        if (prev != mode)
            MOHW_LOG(kLogFile, "VIEW MODE SWITCHED: %ld -> %ld", prev, mode);
    }

    float* fovYPtr = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(transformPtr) + 0x48);
    float nativeFovY = 0.0f;
    if (!SehReadFloat(fovYPtr, &nativeFovY))
        return false;
    if (nativeFovY <= 0.0f || nativeFovY >= 3.14159265f) // reject garbage/uninitialized reads
        return false;

    // TRUE PER-EYE FRUSTUM (companion view mode 3, 2026-09-21): render exactly this eye's real asymmetric frustum, scaled by
    // FrustumScale. The companion maps the whole frame 1:1 onto the eye with the real FOV declared, so nothing is cropped,
    // stretched or offset (the strip/zoom/trim of the inner-edge mode are not needed). See projection_aspect_hook.h for the
    // projection-builder parameterization used here.
    // UNGATED (2026-09-23): the IsPlayerSkeletonLoaded() gate added 2026-09-23 to fix menu/loading-screen double
    // vision was a misuse of that signal -- it exists specifically for the (now-reverted) UI-buffer-separation
    // feature (viewmodel batch rig detection), not as a general "are we in a real level" check, and only reads true
    // while the first-person weapon/hands are actively rendering -- meaning it could also read false during real
    // gameplay moments the viewmodel isn't visible (holstered, cutscenes, etc.), incorrectly falling back to
    // symmetric FOV mid-level. Removed per explicit direction; the menu/loading-screen double-vision regression
    // this was covering for is a known, reintroduced side effect until a correct fix is designed.
    if (hmd.viewMode == mohwvr::ipc::kViewModeTrueFrustum)
    {
        int eyeIdx = IsRightEyeActive() ? 1 : 0; // which eye this game frame will be routed to
        float k = GetFrustumScale();
        if (!(k > 0.2f && k < 4.0f))
            k = 1.0f;
        float tL = tanf(hmd.angleLeft[eyeIdx]) * k;
        float tR = tanf(hmd.angleRight[eyeIdx]) * k;
        float tU = tanf(hmd.angleUp[eyeIdx]) * k;
        float tD = tanf(hmd.angleDown[eyeIdx]) * k;
        float T = (tU - tD) * 0.5f;        // half-height tan of the rendered frustum
        float width = tR - tL;
        if (T > 0.05f && width > 0.05f)
        {
            float aspectNew = width / (2.0f * T);
            float offX = ((tL + tR) * 0.5f) / (aspectNew * T * 2.0f);
            float offY = ((tU + tD) * 0.5f) / (T * 2.0f);
            float fovNew = 2.0f * atanf(T);
            if (fovNew > kMaxFovYRad)
                fovNew = kMaxFovYRad;
            SetInnerEdgeProjection(0.0f, 0.0f); // the inner-edge trim path is not used in this mode
            SetFrustumCandidateIndex(hmd.frustumCandidateIndex); // manual camera selector -- see its own comment
            SetTrueFrustumProjection(fovNew, aspectNew, offX, offY, eyeIdx);
            static std::atomic<unsigned long long> nextTfLogMs{0};
            unsigned long long nowT = GetTickCount64();
            unsigned long long allowedT = nextTfLogMs.load(std::memory_order_relaxed);
            if (nowT >= allowedT && nextTfLogMs.compare_exchange_strong(allowedT, nowT + 2000, std::memory_order_relaxed))
                MOHW_LOG(kLogFile,
                          "TRUE FRUSTUM eye%d: scale %.2f tan L/R/U/D = %.3f/%.3f/%.3f/%.3f -> fovY %.1f deg, aspect %.4f, offset (%.4f, %.4f)",
                          eyeIdx, k, tL, tR, tU, tD, fovNew * kRadToDeg, aspectNew, offX, offY);
            if (SehWriteFloat(fovYPtr, fovNew))
            {
                g_lastWrittenFovY.store(fovNew, std::memory_order_relaxed);
                return true;
            }
            return false;
        }
    }
    SetTrueFrustumProjection(0.0f, 0.0f, 0.0f, 0.0f, -1);

    float newFovY = mohwvr::eyefrustum::EnclosingVerticalFovRad(hmd.angleUp, hmd.angleDown);
    // HYPOTHESIS (2026-09-20, from the 50 vs 110 source dumps + the headset looking magnified):
    // the value at +0x48 is the HORIZONTAL FOV, not vertical, so the game's vertical half-tan is
    // tan(fov/2)/aspect. To get the vertical half-tan T_V the crop assumes, write
    // 2*atan(T_V*aspect). kFovFieldIsHorizontal=false restores the vertical assumption.
    constexpr bool kFovFieldIsHorizontal = true;
    if (kFovFieldIsHorizontal)
    {
        float aspect = 0.0f;
        if (SehReadFloat(reinterpret_cast<const float*>(reinterpret_cast<unsigned char*>(transformPtr) + 0x58), &aspect) &&
            aspect > 0.5f && aspect < 4.0f)
            newFovY = 2.0f * atanf(tanf(newFovY * 0.5f) * aspect);
    }
    // INNER-EDGE MODE: the whole frame is shown, so the enclosing-frustum value above is not what is wanted.
    // Use the user-tunable FovScale (F5/F6, 0.2 steps, saved to the ini) x the game's native FOV instead --
    // the same semantics the old FovScale method had.
    const bool innerEdge = hmd.viewMode == mohwvr::ipc::kViewModeInnerEdge;
    if (innerEdge)
        newFovY = nativeFovY * GetFovScale();
    // Vertical-only FOV (inner-edge mode). VERIFIED FROM THE DECOMPILE (2026-09-20, FUN_006FC090 + FUN_00704C70): +0x48 is a
    // VERTICAL FOV and the projection builder takes the frustum half-width as aspect * half-height (P00 ~ 1/aspect), so
    // scaling tan(fov/2) by the trim AND dividing the aspect by the same trim leaves the horizontal extent unchanged and
    // makes the vertical view `trim` times taller. (Dividing the aspect alone -- the first try -- only magnified the
    // picture horizontally, which is the "stretched" result the user saw.)
    // AUTOMATIC trim (2026-09-20): choose the vertical stretch that makes the displayed image undistorted, from
    // (a) the game frame the companion samples (sourceAspect, inner strip), (b) the eye's tan-space spans, and (c) the FOV
    // being written. ASSUMPTION (unverified): the game's native projection is undistorted for its frame, i.e. its horizontal
    // half-tan is tan(fov/2) * sourceAspect. Then the horizontal magnification into the eye is
    //   Mx = eyeW / (2 * tanH0 * (1 - strip)),   tanH0 = tan(fov/2) * sourceAspect
    // and equal vertical magnification needs tanV = eyeH / (2 * Mx), i.e. trim = tanV / tan(fov/2). The companion's
    // Up/Down arrows scale that automatic value (hmd.aspectTrim, 1.0 = as computed).
    float trim = 1.0f;
    if (innerEdge && hmd.sourceAspect > 0.3f && hmd.innerStrip >= 0.0f && hmd.innerStrip < 0.9f)
    {
        float tv0 = tanf(newFovY * 0.5f);
        float eyeW = tanf(hmd.angleRight[0]) - tanf(hmd.angleLeft[0]);
        float eyeH = tanf(hmd.angleUp[0]) - tanf(hmd.angleDown[0]);
        float th0 = tv0 * hmd.sourceAspect;
        float mx = (th0 > 0.0f) ? eyeW / (2.0f * th0 * (1.0f - hmd.innerStrip)) : 0.0f;
        if (mx > 0.0f && eyeH > 0.0f && tv0 > 0.0f)
        {
            float autoTrim = (eyeH / (2.0f * mx)) / tv0;
            trim = autoTrim * ((hmd.aspectTrim > 0.3f && hmd.aspectTrim < 3.0f) ? hmd.aspectTrim : 1.0f);
            static std::atomic<unsigned long long> nextAutoLogMs{0};
            unsigned long long nowT = GetTickCount64();
            unsigned long long allowedT = nextAutoLogMs.load(std::memory_order_relaxed);
            if (nowT >= allowedT && nextAutoLogMs.compare_exchange_strong(allowedT, nowT + 2000, std::memory_order_relaxed))
                MOHW_LOG(kLogFile, "INNER-EDGE AUTO TRIM: src aspect %.3f strip %.2f eye tan %.3f x %.3f | auto %.3f x user %.2f = %.3f",
                           hmd.sourceAspect, hmd.innerStrip, eyeW, eyeH, autoTrim, hmd.aspectTrim, trim);
        }
    }
    // Taller vertical view: scale tan(fov/2) by the trim here; the matching projection-aspect division (which keeps the
    // horizontal extent unchanged) happens inside the engine matrix rebuild -- hooks/projection_aspect_hook.cpp. Writing
    // the aspect at this commit hook did nothing (tested live 2026-09-20).
    const bool trimActive = innerEdge && trim > 0.3f && trim < 3.5f && fabsf(trim - 1.0f) > 1e-3f;
    if (trimActive)
        newFovY = 2.0f * atanf(tanf(newFovY * 0.5f) * trim);
    bool clamped = newFovY >= kMaxFovYRad;
    if (clamped)
        newFovY = kMaxFovYRad;
    SetInnerEdgeProjection(trimActive ? newFovY : 0.0f, trim); // arms the aspect change for the matching UpdateMatrices call

    static std::atomic<unsigned long long> nextLogMs{0};
    unsigned long long now = GetTickCount64();
    unsigned long long allowed = nextLogMs.load(std::memory_order_relaxed);
    if (now >= allowed && nextLogMs.compare_exchange_strong(allowed, now + 2000, std::memory_order_relaxed))
        MOHW_LOG(kLogFile, "%s: native fovY %.4f rad (%.1f deg) -> %.4f rad (%.1f deg)%s", innerEdge ? "INNER-EDGE FOV (native x FovScale)" : "EYE-MATCHED FOV (enclosing)",
                   nativeFovY, nativeFovY * kRadToDeg, newFovY, newFovY * kRadToDeg, clamped ? " [CLAMPED]" : "");

    if (SehWriteFloat(fovYPtr, newFovY))
    {
        g_lastWrittenFovY.store(newFovY, std::memory_order_relaxed);
        return true;
    }
    return false;
}

float GetLastWrittenFovY()
{
    return g_lastWrittenFovY.load(std::memory_order_relaxed);
}

} // namespace mohw
