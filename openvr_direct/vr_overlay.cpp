#include "vr_overlay.h"

#include "../sdk/logging.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace mohw::openvr_direct {
namespace {

constexpr const char* kLogFile = "mohwvr_overlay.log";

// IVROverlay_028 vtable slots, counted from openvr.h's class IVROverlay declaration order.
constexpr int kSetOverlayWidthInMetersIndex = 22;
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

constexpr int kWidth = 1600;
constexpr int kHeight = 900;
constexpr int kThumbSize = 256;

// Layout.
constexpr int kTitleHeight = 80;
constexpr int kColumnX[2] = {30, 815};
constexpr int kColumnWidth = 755;
constexpr int kHeaderHeight = 44;
constexpr int kRowHeight = 36;
constexpr int kButtonWidth = 50;
constexpr int kButtonGap = 6;
constexpr int kToggleWidth = 110;

struct HitBox
{
    RECT rect;
    int index;
    int steps;
};

void* g_overlay = nullptr;
uint64_t g_main = 0, g_thumb = 0;
PFN_SetOverlayRaw g_setRaw = nullptr;
PFN_PollNextOverlayEvent g_poll = nullptr;
PFN_IsOverlayVisible g_isVisible = nullptr;

// Overlay thread only.
std::vector<HitBox> g_hits;
int g_hover = -1;

// Groups on the left column; everything else goes on the right.
bool IsLeftColumnGroup(const char* group)
{
    return strcmp(group, "Head") == 0 || strcmp(group, "View") == 0;
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
        int err = g_setRaw(g_overlay, handle, m_rgba.data(), m_width, m_height, 4);
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
    c.Text("Changes apply and save immediately", title, smallFont, kDimText, DT_RIGHT);

    int y[2] = {kTitleHeight, kTitleHeight};
    const char* lastGroup[2] = {nullptr, nullptr};
    int rowParity[2] = {0, 0};
    for (int i = 0, n = GetMenuSettingCount(); i < n; ++i)
    {
        MenuSettingInfo info{};
        if (!GetMenuSettingInfo(i, &info))
            continue;
        int col = IsLeftColumnGroup(info.group) ? 0 : 1;
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
        while (g_poll(g_overlay, g_main, &ev, sizeof(ev)))
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
                    AdjustMenuSetting(g_hits[hit].index, g_hits[hit].steps);
                    dirty = true;
                }
            }
            ev = VrEvent{};
        }

        bool visible = g_isVisible(g_overlay, g_main);
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

} // namespace

bool InitVrOverlay(void* overlay)
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
    return true;
}

} // namespace mohw::openvr_direct
