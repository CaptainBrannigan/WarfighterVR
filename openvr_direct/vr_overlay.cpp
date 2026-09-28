#include "vr_overlay.h"

#include "../sdk/logging.h"
#include "../sdk/settings.h"
#include "../hooks/aiming_controller_hook.h"

#include <windows.h>
#include <cmath>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace mohw::openvr_direct {
namespace {

constexpr const char* kLogFile = "mohwvr_overlay.log";

// IVROverlay_028 vtable slots, counted from openvr.h's class IVROverlay declaration order.
constexpr int kCreateOverlayIndex = 1;
constexpr int kSetOverlayWidthInMetersIndex = 22;
constexpr int kSetOverlayTransformAbsoluteIndex = 33;
constexpr int kShowOverlayIndex = 43;
constexpr int kHideOverlayIndex = 44;
constexpr int kIsOverlayVisibleIndex = 45;
constexpr int kPollNextOverlayEventIndex = 48;
constexpr int kSetOverlayInputMethodIndex = 50;
constexpr int kSetOverlayMouseScaleIndex = 52;
constexpr int kSetOverlayRawIndex = 62;
constexpr int kCreateDashboardOverlayIndex = 67;

constexpr int kInputMethodMouse = 1;   // VROverlayInputMethod_Mouse
constexpr uint32_t kEventMouseMove = 300;       // VREvent_MouseMove
constexpr uint32_t kEventMouseButtonDown = 301; // VREvent_MouseButtonDown

using PFN_CreateDashboardOverlay = int(__thiscall*)(void* self, const char* key, const char* name, uint64_t* mainHandle,
                                                    uint64_t* thumbnailHandle);
using PFN_SetOverlayWidthInMeters = int(__thiscall*)(void* self, uint64_t handle, float meters);
using PFN_SetOverlayInputMethod = int(__thiscall*)(void* self, uint64_t handle, int method);
using PFN_SetOverlayMouseScale = int(__thiscall*)(void* self, uint64_t handle, const float* scale);
using PFN_SetOverlayRaw = int(__thiscall*)(void* self, uint64_t handle, void* buffer, uint32_t width, uint32_t height,
                                           uint32_t bytesPerPixel);
using PFN_PollNextOverlayEvent = bool(__thiscall*)(void* self, uint64_t handle, void* event, uint32_t size);
using PFN_IsOverlayVisible = bool(__thiscall*)(void* self, uint64_t handle);
using PFN_CreateOverlay = int(__thiscall*)(void* self, const char* key, const char* name, uint64_t* handle);
using PFN_SetOverlayTransformAbsolute = int(__thiscall*)(void* self, uint64_t handle, int trackingOrigin,
                                                         const float* matrix34);
using PFN_ShowHideOverlay = int(__thiscall*)(void* self, uint64_t handle);

// vr::VREvent_t on Windows: pack(8), and the data union holds 64-bit members, so data starts at 16; the union is
// sized by its 48-byte reserved member.
struct VrEvent
{
    uint32_t eventType;
    uint32_t trackedDeviceIndex;
    float eventAgeSeconds;
    uint32_t padding;
    union
    {
        struct
        {
            float x, y; // origin bottom-left, in SetOverlayMouseScale units
            uint32_t button;
            uint32_t cursorIndex;
        } mouse;
        uint64_t reserved[6];
    } data;
};
static_assert(offsetof(VrEvent, data) == 16, "must match vr::VREvent_t");
static_assert(sizeof(VrEvent) == 64, "must match vr::VREvent_t");

constexpr int kWidth = 2385;
constexpr int kHeight = 1080;
constexpr int kThumbSize = 256;

// Layout.
constexpr int kTitleHeight = 80;
constexpr int kColumnCount = 3;
constexpr int kColumnX[kColumnCount] = {30, 815, 1600};
constexpr int kColumnWidth = 755;
constexpr int kHeaderHeight = 40;
constexpr int kRowHeight = 32;
constexpr int kButtonWidth = 50;
constexpr int kButtonGap = 6;
constexpr int kToggleWidth = 110;

struct HitBox
{
    RECT rect;
    int index; // menu setting index, or one of the action ids below
    int steps;
};
constexpr int kRecenterButton = -100;

void* g_overlay = nullptr;
uint64_t g_main = 0, g_thumb = 0;
PFN_SetOverlayRaw g_setRaw = nullptr;
PFN_PollNextOverlayEvent g_poll = nullptr;
PFN_IsOverlayVisible g_isVisible = nullptr;
PFN_SetOverlayWidthInMeters g_setWidth = nullptr;
PFN_SetOverlayTransformAbsolute g_setTransform = nullptr;
PFN_ShowHideOverlay g_show = nullptr;
PFN_ShowHideOverlay g_hide = nullptr;
int g_trackingUniverse = 1;
// The sight dots are ready (handles created, thread started); g_dotHandles below are written before it is set.
std::atomic<bool> g_sightReady{false};

// Every IVROverlay call goes through this: the menu thread and the sight thread both use the interface.
std::mutex g_overlayCallMutex;

bool PollEvent(VrEvent* ev)
{
    std::lock_guard<std::mutex> lock(g_overlayCallMutex);
    return g_poll(g_overlay, g_main, ev, sizeof(*ev));
}

// Overlay thread only.
std::vector<HitBox> g_hits;
int g_hover = -1;

// Which of the three columns a group goes in (groups are matched by prefix, so "Holsters (...)" counts as Holsters).
int ColumnForGroup(const char* group)
{
    auto is = [group](const char* name) { return strncmp(group, name, strlen(name)) == 0; };
    if (is("Head") || is("View") || is("Legacy"))
        return 0;
    if (is("Weapon") || is("Holster"))
        return 2;
    return 1; // Controls, Hands, HUD
}

class Canvas
{
public:
    Canvas(int width, int height) : m_width(width), m_height(height)
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        m_dc = CreateCompatibleDC(nullptr);
        m_bitmap = CreateDIBSection(m_dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&m_bits), nullptr, 0);
        if (m_dc && m_bitmap)
            m_old = SelectObject(m_dc, m_bitmap);
        SetBkMode(m_dc, TRANSPARENT);
        m_rgba.resize(static_cast<size_t>(width) * height * 4);
    }
    ~Canvas()
    {
        if (m_old)
            SelectObject(m_dc, m_old);
        if (m_bitmap)
            DeleteObject(m_bitmap);
        if (m_dc)
            DeleteDC(m_dc);
    }
    bool Ok() const { return m_dc && m_bitmap && m_bits; }
    HDC Dc() const { return m_dc; }

    void Fill(const RECT& r, COLORREF color)
    {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(m_dc, &r, brush);
        DeleteObject(brush);
    }

    void Text(const char* text, RECT r, HFONT font, COLORREF color, UINT format)
    {
        HGDIOBJ old = SelectObject(m_dc, font);
        SetTextColor(m_dc, color);
        DrawTextA(m_dc, text, -1, &r, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(m_dc, old);
    }

    // GDI writes BGRX; SetOverlayRaw wants RGBA.
    void Upload(uint64_t handle)
    {
        GdiFlush();
        const uint8_t* src = m_bits;
        uint8_t* dst = m_rgba.data();
        for (size_t i = 0, n = static_cast<size_t>(m_width) * m_height; i < n; ++i, src += 4, dst += 4)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
        }
        int err = 0;
        {
            std::lock_guard<std::mutex> lock(g_overlayCallMutex);
            err = g_setRaw(g_overlay, handle, m_rgba.data(), m_width, m_height, 4);
        }
        static int errLines = 0;
        if (err != 0 && errLines < 10)
        {
            ++errLines;
            MOHW_LOG(kLogFile, "SetOverlayRaw err=%d", err);
        }
    }

private:
    int m_width, m_height;
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_old = nullptr;
    uint8_t* m_bits = nullptr;
    std::vector<uint8_t> m_rgba;
};

HFONT MakeFont(int height, int weight)
{
    return CreateFontA(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
}

const COLORREF kBackground = RGB(22, 24, 28);
const COLORREF kRowA = RGB(30, 33, 38);
const COLORREF kRowB = RGB(36, 39, 45);
const COLORREF kTextColor = RGB(230, 232, 236);
const COLORREF kDimText = RGB(150, 156, 166);
const COLORREF kButton = RGB(58, 63, 72);
const COLORREF kButtonHover = RGB(80, 118, 170);
const COLORREF kToggleOn = RGB(46, 125, 72);
const COLORREF kAccent = RGB(214, 170, 72);

// Draws a button and registers it for hit testing; g_hover is an index into g_hits, which is rebuilt in the same
// order every draw, so the hovered button keeps its highlight across redraws.
void DrawButton(Canvas& c, const RECT& r, const char* text, HFONT font, COLORREF color, int index, int steps)
{
    int myIndex = static_cast<int>(g_hits.size());
    g_hits.push_back(HitBox{r, index, steps});
    c.Fill(r, myIndex == g_hover ? kButtonHover : color);
    c.Text(text, r, font, kTextColor, DT_CENTER);
}

void DrawMenu(Canvas& c)
{
    static HFONT titleFont = MakeFont(34, FW_BOLD);
    static HFONT headerFont = MakeFont(25, FW_BOLD);
    static HFONT rowFont = MakeFont(21, FW_NORMAL);
    static HFONT smallFont = MakeFont(18, FW_NORMAL);

    g_hits.clear();
    RECT all{0, 0, kWidth, kHeight};
    c.Fill(all, kBackground);

    RECT title{kColumnX[0], 0, kWidth - 30, kTitleHeight};
    c.Text("MOHW VR settings", title, titleFont, kAccent, DT_LEFT);
    RECT hint{kColumnX[0] + 330, 0, kWidth - 250, kTitleHeight};
    c.Text("Changes apply and save immediately", hint, smallFont, kDimText, DT_LEFT);
    RECT recenter{kWidth - 30 - 200, 18, kWidth - 30, kTitleHeight - 14};
    DrawButton(c, recenter, "Recenter view", rowFont, kButton, kRecenterButton, 0);

    int y[kColumnCount] = {kTitleHeight, kTitleHeight, kTitleHeight};
    const char* lastGroup[kColumnCount] = {nullptr, nullptr, nullptr};
    int rowParity[kColumnCount] = {0, 0, 0};
    for (int i = 0, n = GetMenuSettingCount(); i < n; ++i)
    {
        MenuSettingInfo info{};
        if (!GetMenuSettingInfo(i, &info))
            continue;
        int col = ColumnForGroup(info.group);
        int x = kColumnX[col];
        if (!lastGroup[col] || strcmp(lastGroup[col], info.group) != 0)
        {
            RECT header{x, y[col] + 6, x + kColumnWidth, y[col] + kHeaderHeight};
            c.Text(info.group, header, headerFont, kAccent, DT_LEFT);
            y[col] += kHeaderHeight;
            lastGroup[col] = info.group;
            rowParity[col] = 0;
        }

        RECT row{x, y[col], x + kColumnWidth, y[col] + kRowHeight};
        c.Fill(row, (rowParity[col]++ & 1) ? kRowB : kRowA);
        RECT label{x + 12, row.top, x + 420, row.bottom};
        c.Text(info.label, label, rowFont, kTextColor, DT_LEFT | DT_END_ELLIPSIS);

        int right = x + kColumnWidth - 8;
        int top = row.top + 3, bottom = row.bottom - 3;
        if (info.isToggle)
        {
            RECT button{right - kToggleWidth, top, right, bottom};
            bool on = info.value != 0.0f;
            DrawButton(c, button, on ? "ON" : "OFF", rowFont, on ? kToggleOn : kButton, i, 1);
        }
        else if (info.isChoice)
        {
            // [<] choice [>]
            RECT next{right - kButtonWidth, top, right, bottom};
            RECT prev{right - 2 * kButtonWidth - kButtonGap - 250, top, right - kButtonWidth - kButtonGap - 250, bottom};
            RECT valueRect{prev.right + 8, row.top, next.left - 8, row.bottom};
            DrawButton(c, prev, "<", rowFont, kButton, i, -1);
            c.Text(info.text, valueRect, rowFont, kTextColor, DT_CENTER | DT_END_ELLIPSIS);
            DrawButton(c, next, ">", rowFont, kButton, i, 1);
        }
        else
        {
            // [--] [-] value [+] [++]; the doubled buttons move ten steps.
            const char* labels[4] = {"--", "-", "+", "++"};
            const int steps[4] = {-10, -1, 1, 10};
            int buttonsLeft = right - 4 * kButtonWidth - 3 * kButtonGap;
            char value[48];
            snprintf(value, sizeof(value), "%.*f %s", info.decimals, info.value, info.unit);
            RECT valueRect{x + 420, row.top, buttonsLeft - 12, row.bottom};
            c.Text(value, valueRect, rowFont, kTextColor, DT_RIGHT);
            for (int b = 0; b < 4; ++b)
            {
                int left = buttonsLeft + b * (kButtonWidth + kButtonGap);
                RECT button{left, top, left + kButtonWidth, bottom};
                DrawButton(c, button, labels[b], rowFont, kButton, i, steps[b]);
            }
        }
        y[col] += kRowHeight;
    }
}

void DrawThumbnail(Canvas& c)
{
    HFONT font = MakeFont(60, FW_BOLD);
    RECT all{0, 0, kThumbSize, kThumbSize};
    c.Fill(all, kBackground);
    RECT top{0, 40, kThumbSize, 130};
    RECT bottom{0, 125, kThumbSize, 215};
    c.Text("MOHW", top, font, kAccent, DT_CENTER);
    c.Text("VR", bottom, font, kTextColor, DT_CENTER);
    DeleteObject(font);
}

int HitTest(float px, float py)
{
    for (int i = 0; i < static_cast<int>(g_hits.size()); ++i)
    {
        const RECT& r = g_hits[i].rect;
        if (px >= r.left && px < r.right && py >= r.top && py < r.bottom)
            return i;
    }
    return -1;
}

void OverlayThreadProc()
{
    Canvas menu(kWidth, kHeight);
    Canvas thumb(kThumbSize, kThumbSize);
    if (!menu.Ok() || !thumb.Ok())
    {
        MOHW_LOG(kLogFile, "overlay thread: GDI canvas creation FAILED -- no menu");
        return;
    }
    DrawThumbnail(thumb);
    thumb.Upload(g_thumb);

    bool dirty = true;
    unsigned long long lastDraw = 0;
    for (;;)
    {
        VrEvent ev{};
        while (PollEvent(&ev))
        {
            if (ev.eventType == kEventMouseMove || ev.eventType == kEventMouseButtonDown)
            {
                float px = ev.data.mouse.x;
                float py = static_cast<float>(kHeight) - ev.data.mouse.y; // events are bottom-left origin
                int hit = HitTest(px, py);
                if (hit != g_hover)
                {
                    g_hover = hit;
                    dirty = true;
                }
                if (ev.eventType == kEventMouseButtonDown && hit >= 0)
                {
                    if (g_hits[hit].index == kRecenterButton)
                        RequestRecenter("Recenter button (dashboard)");
                    else
                        AdjustMenuSetting(g_hits[hit].index, g_hits[hit].steps);
                    dirty = true;
                }
            }
            ev = VrEvent{};
        }

        bool visible = false;
        {
            std::lock_guard<std::mutex> lock(g_overlayCallMutex);
            visible = g_isVisible(g_overlay, g_main);
        }
        unsigned long long now = GetTickCount64();
        // Redraw on change, and twice a second while open so values changed by hotkeys show up too.
        if (visible && (dirty || now - lastDraw >= 500))
        {
            DrawMenu(menu);
            menu.Upload(g_main);
            dirty = false;
            lastDraw = now;
        }
        Sleep(visible ? 15 : 100);
    }
}

// ---- Sight dots (2026-09-28) ----------------------------------------------------------------------------------------
// Two small world-placed overlays, drawn over the scene like a real sight:
//  - the OPTIC dot: fixed to the weapon hand like a sight mounted on the gun -- at the hand, raised by the zero
//    (OpticHeight), moved right by windage and forward by OpticDistance, in the hand's own frame. Zeroed by eye:
//    raise or lower it until eye, dot and impacts line up at the range you care about.
//  - the AIM RAY dot: straight along the hand's forward at SightZeroDistance.
// Each is red, or green with its ...Green setting on.
// The submit thread only stores the latest placement; this thread makes the SteamVR calls, woken once per frame, so
// they never delay eye submission.
constexpr int kDotOptic = 0;
constexpr int kDotRay = 1;
constexpr int kDotCount = 2;

struct DotPlacement
{
    bool visible = false;
    float pos[3] = {};
    float width = 0.0f;
};
struct SightState
{
    DotPlacement dots[kDotCount];
    float head[3] = {};
};
std::mutex g_sightMutex;
SightState g_sightState; // guarded by g_sightMutex
HANDLE g_sightEvent = nullptr;
uint64_t g_dotHandles[kDotCount] = {0, 0}; // written once before g_sightReady

void UploadDotTexture(uint64_t handle, uint8_t red, uint8_t green, uint8_t blue)
{
    constexpr int kSize = 64;
    std::vector<uint8_t> rgba(kSize * kSize * 4);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
        {
            float dx = (x + 0.5f) / kSize * 2.0f - 1.0f, dy = (y + 0.5f) / kSize * 2.0f - 1.0f;
            float r = sqrtf(dx * dx + dy * dy);
            float alpha = r >= 1.0f ? 0.0f : (r <= 0.6f ? 1.0f : (1.0f - r) / 0.4f);
            bool core = r < 0.35f; // brighter centre
            uint8_t* p = &rgba[(y * kSize + x) * 4];
            p[0] = core ? static_cast<uint8_t>(red / 2 + 128) : red;
            p[1] = core ? static_cast<uint8_t>(green / 2 + 128) : green;
            p[2] = core ? static_cast<uint8_t>(blue / 2 + 128) : blue;
            p[3] = static_cast<uint8_t>(alpha * 255.0f);
        }
    std::lock_guard<std::mutex> lock(g_overlayCallMutex);
    int err = g_setRaw(g_overlay, handle, rgba.data(), kSize, kSize, 4);
    MOHW_LOG(kLogFile, "dot texture (%u,%u,%u) err=%d", red, green, blue, err);
}

// Billboard facing the head: rows of the 3x4 are x/y/z components, columns the overlay's axes (+Z toward the viewer).
bool BillboardMatrix(const float pos[3], const float head[3], float m[12])
{
    float z[3] = {head[0] - pos[0], head[1] - pos[1], head[2] - pos[2]};
    float zl = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (zl < 1e-4f)
        return false;
    for (float& c : z)
        c /= zl;
    float x[3] = {z[2], 0.0f, -z[0]}; // up x z, up = +Y
    float xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-4f)
    {
        x[0] = 1.0f;
        x[2] = 0.0f;
        xl = 1.0f;
    }
    x[0] /= xl;
    x[2] /= xl;
    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]}; // z x x
    for (int r = 0; r < 3; ++r)
    {
        m[r * 4 + 0] = x[r];
        m[r * 4 + 1] = y[r];
        m[r * 4 + 2] = z[r];
        m[r * 4 + 3] = pos[r];
    }
    return true;
}

void UploadDotColour(int dot, bool green)
{
    if (green)
        UploadDotTexture(g_dotHandles[dot], 40, 255, 60);
    else
        UploadDotTexture(g_dotHandles[dot], 255, 30, 30);
}

void SightThreadProc()
{
    bool green[kDotCount] = {GetOpticDotGreen(), GetRayDotGreen()};
    for (int d = 0; d < kDotCount; ++d)
        UploadDotColour(d, green[d]);

    bool shown[kDotCount] = {false, false};
    for (;;)
    {
        WaitForSingleObject(g_sightEvent, 200);
        bool wantGreen[kDotCount] = {GetOpticDotGreen(), GetRayDotGreen()};
        for (int d = 0; d < kDotCount; ++d)
            if (wantGreen[d] != green[d])
            {
                green[d] = wantGreen[d];
                UploadDotColour(d, green[d]);
            }
        SightState s;
        {
            std::lock_guard<std::mutex> lock(g_sightMutex);
            s = g_sightState;
        }
        for (int d = 0; d < kDotCount; ++d)
        {
            uint64_t handle = g_dotHandles[d];
            float m[12];
            if (!s.dots[d].visible || !BillboardMatrix(s.dots[d].pos, s.head, m))
            {
                if (shown[d] && !s.dots[d].visible)
                {
                    std::lock_guard<std::mutex> lock(g_overlayCallMutex);
                    g_hide(g_overlay, handle);
                    shown[d] = false;
                }
                continue;
            }
            std::lock_guard<std::mutex> lock(g_overlayCallMutex);
            g_setWidth(g_overlay, handle, s.dots[d].width);
            g_setTransform(g_overlay, handle, g_trackingUniverse, m);
            if (!shown[d])
            {
                g_show(g_overlay, handle);
                shown[d] = true;
            }
        }
    }
}

} // namespace

void UpdateSightDot(bool visible, const float origin[3], const Quat& aimOrientation, const float headPos[3])
{
    if (!g_sightReady.load(std::memory_order_acquire))
        return;
    float sizeDeg = GetSightDotSizeDeg();
    if (!(sizeDeg > 0.0f))
        sizeDeg = 0.35f;
    float halfTan = tanf(sizeDeg * 0.5f * 3.14159265f / 180.0f);
    float height = 0.0f, windage = 0.0f;
    GetOpticOffset(&height, &windage);
    float opticForward = GetOpticDistance();
    if (!(opticForward >= 0.0f))
        opticForward = 0.0f;
    float rayDistance = GetSightZeroDistance();
    if (!(rayDistance >= 0.5f))
        rayDistance = 25.0f;

    // Offsets in the hand's own frame (x right, y up, -z forward), rotated into tracking space.
    Vec3 opticLocal{};
    opticLocal.x = windage;
    opticLocal.y = height;
    opticLocal.z = -opticForward;
    // The aim ray dot: the hand's forward turned up by elevation and right by windage (degrees).
    float rayUpDeg = 0.0f, rayRightDeg = 0.0f;
    GetRayDotOffsetDeg(&rayUpDeg, &rayRightDeg);
    const float kRad = 3.14159265f / 180.0f;
    float e = rayUpDeg * kRad, w = rayRightDeg * kRad;
    Vec3 rayLocal{};
    rayLocal.x = sinf(w) * cosf(e) * rayDistance;
    rayLocal.y = sinf(e) * rayDistance;
    rayLocal.z = -cosf(w) * cosf(e) * rayDistance;
    Vec3 offsets[kDotCount] = {QuatRotateVector(aimOrientation, opticLocal), QuatRotateVector(aimOrientation, rayLocal)};
    bool enabled[kDotCount] = {GetOpticDotEnabled(), GetSightDotEnabled()};

    SightState s;
    for (int i = 0; i < 3; ++i)
        s.head[i] = headPos[i];
    for (int d = 0; d < kDotCount; ++d)
    {
        DotPlacement& dot = s.dots[d];
        dot.visible = visible && enabled[d];
        if (!dot.visible)
            continue;
        dot.pos[0] = origin[0] + offsets[d].x;
        dot.pos[1] = origin[1] + offsets[d].y;
        dot.pos[2] = origin[2] + offsets[d].z;
        // Constant angular size as seen from the head (the optic dot can sit right at the hand).
        float dx = dot.pos[0] - headPos[0], dy = dot.pos[1] - headPos[1], dz = dot.pos[2] - headPos[2];
        float fromHead = sqrtf(dx * dx + dy * dy + dz * dz);
        dot.width = 2.0f * (fromHead > 0.05f ? fromHead : 0.05f) * halfTan;
    }
    {
        std::lock_guard<std::mutex> lock(g_sightMutex);
        g_sightState = s;
    }
    SetEvent(g_sightEvent);
}

bool InitVrOverlay(void* overlay, int trackingUniverse)
{
    if (!overlay)
        return false;
    g_overlay = overlay;
    void** vtable = *reinterpret_cast<void***>(overlay);
    auto create = reinterpret_cast<PFN_CreateDashboardOverlay>(vtable[kCreateDashboardOverlayIndex]);
    auto setWidth = reinterpret_cast<PFN_SetOverlayWidthInMeters>(vtable[kSetOverlayWidthInMetersIndex]);
    auto setInput = reinterpret_cast<PFN_SetOverlayInputMethod>(vtable[kSetOverlayInputMethodIndex]);
    auto setMouseScale = reinterpret_cast<PFN_SetOverlayMouseScale>(vtable[kSetOverlayMouseScaleIndex]);
    g_setRaw = reinterpret_cast<PFN_SetOverlayRaw>(vtable[kSetOverlayRawIndex]);
    g_poll = reinterpret_cast<PFN_PollNextOverlayEvent>(vtable[kPollNextOverlayEventIndex]);
    g_isVisible = reinterpret_cast<PFN_IsOverlayVisible>(vtable[kIsOverlayVisibleIndex]);

    int err = create(overlay, "mohwvr.settings", "MOHW VR", &g_main, &g_thumb);
    MOHW_LOG(kLogFile, "CreateDashboardOverlay err=%d main=%llu thumb=%llu", err,
              static_cast<unsigned long long>(g_main), static_cast<unsigned long long>(g_thumb));
    if (err != 0 || g_main == 0)
        return false;
    float mouseScale[2] = {static_cast<float>(kWidth), static_cast<float>(kHeight)};
    int widthErr = setWidth(overlay, g_main, 2.5f);
    int inputErr = setInput(overlay, g_main, kInputMethodMouse);
    int scaleErr = setMouseScale(overlay, g_main, mouseScale);
    MOHW_LOG(kLogFile, "SetOverlayWidthInMeters err=%d, SetOverlayInputMethod err=%d, SetOverlayMouseScale err=%d", widthErr,
              inputErr, scaleErr);

    std::thread(OverlayThreadProc).detach();
    MOHW_LOG(kLogFile, "dashboard tab ready (%d settings)", GetMenuSettingCount());

    // Sight dots: ordinary (non-dashboard) overlays placed in the world every frame.
    g_trackingUniverse = trackingUniverse;
    g_setWidth = setWidth;
    g_setTransform = reinterpret_cast<PFN_SetOverlayTransformAbsolute>(vtable[kSetOverlayTransformAbsoluteIndex]);
    g_show = reinterpret_cast<PFN_ShowHideOverlay>(vtable[kShowOverlayIndex]);
    g_hide = reinterpret_cast<PFN_ShowHideOverlay>(vtable[kHideOverlayIndex]);
    auto createOverlay = reinterpret_cast<PFN_CreateOverlay>(vtable[kCreateOverlayIndex]);
    const char* keys[kDotCount] = {"mohwvr.optic", "mohwvr.sight"};
    const char* names[kDotCount] = {"MOHW VR optic dot", "MOHW VR aim ray dot"};
    bool allCreated = true;
    for (int d = 0; d < kDotCount; ++d)
    {
        int dotErr = 0;
        {
            std::lock_guard<std::mutex> lock(g_overlayCallMutex);
            dotErr = createOverlay(overlay, keys[d], names[d], &g_dotHandles[d]);
        }
        MOHW_LOG(kLogFile, "CreateOverlay(%s) err=%d handle=%llu", keys[d], dotErr,
                  static_cast<unsigned long long>(g_dotHandles[d]));
        allCreated = allCreated && dotErr == 0 && g_dotHandles[d] != 0;
    }
    if (allCreated)
    {
        g_sightEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_sightReady.store(true, std::memory_order_release);
        std::thread(SightThreadProc).detach();
    }
    return true;
}

} // namespace mohw::openvr_direct
