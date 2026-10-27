// Window Ring: draws one smooth ring around every normal window, from its own
// process. It injects no code into other apps. WinEvent hooks report window
// moves, shows, hides and focus changes, and each ring is drawn in its own
// click-through layered windows, the same way Tacky Borders does it.
//
// Settings live in window-ring.ini next to the exe. "window-ring.exe --stop"
// quits a running copy.

#include <windows.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <wchar.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr wchar_t kMainClass[] = L"WindowRingMain";
constexpr wchar_t kPartClass[] = L"WindowRingPart";
constexpr wchar_t kMutexName[] = L"Local\\WindowRingSingleInstance";
constexpr UINT_PTR kResyncTimer = 1;
constexpr UINT_PTR kFadeTimer = 2;
constexpr UINT kFadeFrameMs = 16;

struct Ring {
    float r = 0, g = 0, b = 0, a = 0;
};

struct Config {
    int width = 2;
    int gap = -1;
    int radius = 16;
    int fadeMs = 150;
    int gradientAngle = 45;
    int resyncMs = 500;
    bool gradient = true;
    bool focusedAccent = true;
    COLORREF focused = RGB(0x89, 0xb4, 0xfa);
    COLORREF focusedTo = RGB(0xcb, 0xa6, 0xf7);
    COLORREF unfocused = RGB(0x58, 0x5b, 0x70);
    COLORREF unfocusedTo = RGB(0x58, 0x5b, 0x70);
    std::vector<std::wstring> exclude;
};

// Four strips per window, so a big window never needs one huge bitmap.
struct Overlay {
    HWND parts[4] = {};
    RECT frame = {};
    UINT dpi = 96;
    bool focused = false;
    bool shown = false;
    bool dirty = true;
    bool fading = false;
    DWORD fadeStart = 0;
    Ring cur1, cur2, from1, from2, to1, to2;
};

Config g_cfg;
std::wstring g_iniPath;
HWND g_main = nullptr;
COLORREF g_accent = 0;
std::unordered_map<HWND, Overlay> g_overlays;
std::unordered_map<DWORD, bool> g_pidExcluded;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

std::wstring ReadString(const wchar_t* key, const wchar_t* fallback) {
    wchar_t buf[256];
    GetPrivateProfileStringW(L"ring", key, fallback, buf, 256, g_iniPath.c_str());
    return buf;
}

int ReadInt(const wchar_t* key, int fallback, int lo, int hi) {
    int v = (int)wcstol(ReadString(key, std::to_wstring(fallback).c_str()).c_str(), nullptr, 10);
    return v < lo ? lo : (v > hi ? hi : v);
}

COLORREF ParseHex(const std::wstring& text, COLORREF fallback) {
    std::wstring hex = !text.empty() && text[0] == L'#' ? text.substr(1) : text;
    if (hex.size() != 6) {
        return fallback;
    }
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(hex.c_str(), &end, 16);
    if (*end) {
        return fallback;
    }
    return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

void LoadConfig() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    g_iniPath = path.substr(0, path.rfind(L'\\') + 1) + L"window-ring.ini";

    g_cfg.width = ReadInt(L"width", 2, 1, 32);
    g_cfg.gap = ReadInt(L"gap", -1, -8, 32);
    g_cfg.radius = ReadInt(L"radius", 16, 0, 64);
    g_cfg.fadeMs = ReadInt(L"fadeMs", 150, 0, 2000);
    g_cfg.gradientAngle = ReadInt(L"gradientAngle", 45, 0, 360);
    g_cfg.resyncMs = ReadInt(L"resyncMs", 500, 100, 5000);
    g_cfg.gradient = ReadInt(L"gradient", 1, 0, 1) != 0;

    std::wstring focused = ReadString(L"focused", L"accent");
    g_cfg.focusedAccent = _wcsicmp(focused.c_str(), L"accent") == 0;
    g_cfg.focused = ParseHex(focused, g_cfg.focused);
    g_cfg.focusedTo = ParseHex(ReadString(L"focusedTo", L"cba6f7"), g_cfg.focusedTo);
    g_cfg.unfocused = ParseHex(ReadString(L"unfocused", L"585b70"), g_cfg.unfocused);
    g_cfg.unfocusedTo = ParseHex(ReadString(L"unfocusedTo", L"585b70"), g_cfg.unfocusedTo);

    std::wstring list = ReadString(L"exclude", L"windhawk-ui.exe");
    for (size_t start = 0; start < list.size();) {
        size_t end = list.find(L',', start);
        if (end == std::wstring::npos) {
            end = list.size();
        }
        std::wstring item = list.substr(start, end - start);
        size_t first = item.find_first_not_of(L" ");
        if (first != std::wstring::npos) {
            g_cfg.exclude.push_back(item.substr(first, item.find_last_not_of(L" ") - first + 1));
        }
        start = end + 1;
    }
}

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------

COLORREF AccentRgb() {
    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (FAILED(DwmGetColorizationColor(&argb, &opaque))) {
        return RGB(0x89, 0xb4, 0xfa);
    }
    return RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff);
}

Ring ToRing(COLORREF c) {
    return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f};
}

void StateColors(bool focused, Ring* c1, Ring* c2) {
    COLORREF rgb = focused ? (g_cfg.focusedAccent ? AccentRgb() : g_cfg.focused) : g_cfg.unfocused;
    COLORREF to = focused ? g_cfg.focusedTo : g_cfg.unfocusedTo;
    *c1 = ToRing(rgb);
    *c2 = g_cfg.gradient ? ToRing(to) : *c1;
}

// ---------------------------------------------------------------------------
// Ring renderer (same anti-aliased rounded ring as the Windhawk mod)
// ---------------------------------------------------------------------------

float Clamp01(float v) {
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

Ring LerpColor(const Ring& x, const Ring& y, float t) {
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t,
            x.a + (y.a - x.a) * t};
}

// Rows/columns from each canvas edge that can hold ring pixels.
int RingBand(int pad, float radius, float offset) {
    float inside = (offset < 0 ? -offset : 0) + 1;
    return pad + (int)ceilf(radius > inside ? radius : inside) + 1;
}

// The canvas is the window frame grown by pad on every side (fw x fh pixels).
// Draws the part of the ring inside the strip (sx, sy, sw, sh) into a zeroed,
// top-down, premultiplied BGRA buffer. The ring spans [offset, offset + width]
// outside the frame edge.
void RenderRing(uint32_t* px, int sx, int sy, int sw, int sh, int fw, int fh, int pad,
                float radius, float offset, float width, Ring c1, Ring c2, float angleDeg) {
    float cx = fw / 2.0f, cy = fh / 2.0f;
    float hx = fw / 2.0f - pad, hy = fh / 2.0f - pad;
    if (hx <= 0 || hy <= 0) {
        return;
    }
    float r = radius;
    if (r > hx) r = hx;
    if (r > hy) r = hy;

    float rad = angleDeg * 3.14159265f / 180.0f;
    float dx = cosf(rad), dy = sinf(rad);
    float span = fabsf(hx * dx) + fabsf(hy * dy) + pad;
    bool solid = c1.r == c2.r && c1.g == c2.g && c1.b == c2.b && c1.a == c2.a;

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            float fx = sx + x + 0.5f - cx, fy = sy + y + 0.5f - cy;
            float qx = fabsf(fx) - (hx - r), qy = fabsf(fy) - (hy - r);
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float inner = qx > qy ? qx : qy;
            float d = sqrtf(ox * ox + oy * oy) + (inner < 0 ? inner : 0) - r;
            float cov = Clamp01(d - offset + 0.5f) * Clamp01(offset + width - d + 0.5f);
            if (cov <= 0) {
                continue;
            }
            Ring c = solid ? c1
                           : LerpColor(c1, c2, Clamp01((fx * dx + fy * dy) / span * 0.5f + 0.5f));
            float a = c.a * cov;
            px[y * sw + x] = ((uint32_t)(a * 255 + 0.5f) << 24) |
                             ((uint32_t)(c.r * a * 255 + 0.5f) << 16) |
                             ((uint32_t)(c.g * a * 255 + 0.5f) << 8) |
                             (uint32_t)(c.b * a * 255 + 0.5f);
        }
    }
}

// Splits the canvas into top, bottom, left and right strips. Returns how many
// are non-empty (left and right vanish on very short windows).
int RingStrips(int fw, int fh, int band, RECT out[4]) {
    if (band * 2 >= fh) {
        band = (fh + 1) / 2;
    }
    out[0] = {0, 0, fw, band};
    out[1] = {0, fh - band, fw, fh};
    out[2] = {0, band, band, fh - band};
    out[3] = {fw - band, band, fw, fh - band};
    return fh - band * 2 > 0 ? 4 : 2;
}

struct Shape {
    int pad = 0, width = 0, offset = 0, fw = 0, fh = 0, count = 0;
    float radius = 0;
    RECT strips[4] = {};
};

Shape ShapeFor(const Overlay& ov) {
    Shape s;
    float scale = ov.dpi / 96.0f;
    s.width = (int)ceilf(g_cfg.width * scale);
    s.offset = (int)roundf(g_cfg.gap * scale);
    s.pad = s.width + (s.offset > 0 ? s.offset : 0) + 1;
    s.radius = g_cfg.radius * scale;
    s.fw = ov.frame.right - ov.frame.left + s.pad * 2;
    s.fh = ov.frame.bottom - ov.frame.top + s.pad * 2;
    s.count = RingStrips(s.fw, s.fh, RingBand(s.pad, s.radius, (float)s.offset), s.strips);
    return s;
}

// ---------------------------------------------------------------------------
// Overlay windows
// ---------------------------------------------------------------------------

bool CreateParts(HWND target, Overlay& ov) {
    DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) {
        ex |= WS_EX_TOPMOST;
    }
    for (int i = 0; i < 4; i++) {
        ov.parts[i] = CreateWindowExW(ex, kPartClass, L"", WS_POPUP | WS_DISABLED, 0, 0, 0, 0,
                                      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!ov.parts[i]) {
            for (int j = 0; j < i; j++) {
                DestroyWindow(ov.parts[j]);
            }
            return false;
        }
    }
    return true;
}

void DestroyOverlay(HWND target) {
    auto it = g_overlays.find(target);
    if (it == g_overlays.end()) {
        return;
    }
    for (HWND part : it->second.parts) {
        DestroyWindow(part);
    }
    g_overlays.erase(it);
}

// Keeps the strips directly above their target: the ring sits over the window
// it belongs to and under anything that covers that window.
void Restack(Overlay& ov, HWND target) {
    HWND above = GetWindow(target, GW_HWNDPREV);
    bool targetTopmost = GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST;
    if (above && !targetTopmost && (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        above = HWND_TOP;
    }
    if (above == ov.parts[3]) {
        return;
    }
    for (HWND part : ov.parts) {
        SetWindowPos(part, above ? above : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        above = part;
    }
}

void PaintStrips(Overlay& ov) {
    Shape s = ShapeFor(ov);
    HDC dc = CreateCompatibleDC(nullptr);
    for (int i = 0; i < 4; i++) {
        HWND part = ov.parts[i];
        if (i >= s.count) {
            ShowWindow(part, SW_HIDE);
            continue;
        }
        const RECT& strip = s.strips[i];
        int sw = strip.right - strip.left, sh = strip.bottom - strip.top;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = sw;
        bi.bmiHeader.biHeight = -sh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) {
            continue;
        }
        RenderRing((uint32_t*)bits, strip.left, strip.top, sw, sh, s.fw, s.fh, s.pad, s.radius,
                   (float)s.offset, (float)s.width, ov.cur1, ov.cur2, (float)g_cfg.gradientAngle);
        HGDIOBJ old = SelectObject(dc, bmp);
        POINT dst = {ov.frame.left - s.pad + strip.left, ov.frame.top - s.pad + strip.top};
        SIZE size = {sw, sh};
        POINT src = {0, 0};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(part, nullptr, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
        DeleteObject(bmp);
        ShowWindow(part, SW_SHOWNOACTIVATE);
    }
    DeleteDC(dc);
    ov.dirty = false;
    ov.shown = true;
}

// Same size, new position: slide the strips without repainting.
void MoveStrips(Overlay& ov) {
    Shape s = ShapeFor(ov);
    for (int i = 0; i < s.count; i++) {
        POINT dst = {ov.frame.left - s.pad + s.strips[i].left,
                     ov.frame.top - s.pad + s.strips[i].top};
        UpdateLayeredWindow(ov.parts[i], nullptr, &dst, nullptr, nullptr, nullptr, 0, nullptr, 0);
    }
}

void HideStrips(Overlay& ov) {
    if (!ov.shown) {
        return;
    }
    for (HWND part : ov.parts) {
        ShowWindow(part, SW_HIDE);
    }
    ov.shown = false;
}

void StartFade(Overlay& ov) {
    StateColors(ov.focused, &ov.to1, &ov.to2);
    ov.dirty = true;
    if (g_cfg.fadeMs <= 0) {
        ov.cur1 = ov.to1;
        ov.cur2 = ov.to2;
        ov.fading = false;
        return;
    }
    ov.from1 = ov.cur1;
    ov.from2 = ov.cur2;
    ov.fadeStart = GetTickCount();
    ov.fading = true;
    SetTimer(g_main, kFadeTimer, kFadeFrameMs, nullptr);
}

// One timer steps every fading overlay; it stops when none is fading.
void FadeAll() {
    bool any = false;
    for (auto& entry : g_overlays) {
        Overlay& ov = entry.second;
        if (!ov.fading) {
            continue;
        }
        float t = g_cfg.fadeMs ? (float)(GetTickCount() - ov.fadeStart) / g_cfg.fadeMs : 1.0f;
        t = Clamp01(t);
        float e = 1 - (1 - t) * (1 - t);  // ease out
        ov.cur1 = LerpColor(ov.from1, ov.to1, e);
        ov.cur2 = LerpColor(ov.from2, ov.to2, e);
        ov.fading = t < 1;
        ov.dirty = true;
        if (ov.shown) {
            PaintStrips(ov);
        }
        any = any || ov.fading;
    }
    if (!any) {
        KillTimer(g_main, kFadeTimer);
    }
}

// ---------------------------------------------------------------------------
// Which windows get a ring
// ---------------------------------------------------------------------------

// Our own windows and the excluded programs get no ring. Cached per process.
bool ProcessExcluded(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        return true;
    }
    auto it = g_pidExcluded.find(pid);
    if (it != g_pidExcluded.end()) {
        return it->second;
    }
    bool excluded = false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
            const wchar_t* name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            for (const std::wstring& ex : g_cfg.exclude) {
                if (_wcsicmp(name, ex.c_str()) == 0) {
                    excluded = true;
                }
            }
        }
        CloseHandle(proc);
    }
    g_pidExcluded[pid] = excluded;
    return excluded;
}

// A window with a caption or sizing frame, or a frameless app window that would
// show in Alt+Tab (unowned, not a tool window, can take focus). Menus, tooltips,
// popups and the shell's own surfaces fail both tests.
bool IsCandidate(HWND hwnd) {
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & WS_CHILD) {
        return false;
    }
    bool framed = (style & WS_THICKFRAME) == WS_THICKFRAME || (style & WS_CAPTION) == WS_CAPTION;
    // win11-accent-border's "special windows" rule: any top-level window that is
    // not a popup (Flow Launcher, WPF and other custom-drawn windows).
    bool special = !(style & WS_POPUP);
    if (!framed && !special) {
        bool appWindow = ex & WS_EX_APPWINDOW;
        if ((ex & WS_EX_NOACTIVATE) || ((ex & WS_EX_TOOLWINDOW) && !appWindow) ||
            (GetWindow(hwnd, GW_OWNER) && !appWindow)) {
            return false;
        }
    }
    if (!framed) {
        wchar_t cls[64];
        GetClassNameW(hwnd, cls, 64);
        for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd",
                                     L"Shell_SecondaryTrayWnd"}) {
            if (wcscmp(cls, shell) == 0) {
                return false;
            }
        }
    }
    return !ProcessExcluded(hwnd);
}

// False when the window is hidden, minimized, cloaked on another desktop,
// maximized or fullscreen. Those states get no ring.
bool GetDrawFrame(HWND target, RECT* frame) {
    if (!IsWindowVisible(target) || IsIconic(target) || IsZoomed(target)) {
        return false;
    }
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked) {
        return false;
    }
    // Near-invisible layered windows are helpers, not something you look at
    // (RHI parks a 2% opacity drag-and-drop target over its logo).
    BYTE alpha = 255;
    DWORD layeredFlags = 0;
    if ((GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_LAYERED) &&
        GetLayeredWindowAttributes(target, nullptr, &alpha, &layeredFlags) &&
        (layeredFlags & LWA_ALPHA) && alpha < 64) {
        return false;
    }
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, frame,
                                     sizeof(*frame)))) {
        return false;
    }
    // Tiny helper windows and windows parked off every screen get no ring.
    // 40 px keeps a bare Flow Launcher search bar (58 px tall) in.
    if (frame->right - frame->left < 40 || frame->bottom - frame->top < 40) {
        return false;
    }
    HMONITOR monitor = MonitorFromRect(frame, MONITOR_DEFAULTTONULL);
    MONITORINFO mi = {sizeof(mi)};
    if (!monitor) {
        return false;
    }
    if (GetMonitorInfoW(monitor, &mi) &&
        frame->left <= mi.rcMonitor.left && frame->top <= mi.rcMonitor.top &&
        frame->right >= mi.rcMonitor.right && frame->bottom >= mi.rcMonitor.bottom) {
        return false;
    }
    return true;
}

// Brings one window's ring in line with the window. Runs on our thread only.
void SyncTarget(HWND target) {
    if (!IsCandidate(target)) {
        DestroyOverlay(target);
        return;
    }
    RECT frame = {};
    auto it = g_overlays.find(target);
    if (!GetDrawFrame(target, &frame)) {
        if (it != g_overlays.end()) {
            HideStrips(it->second);
        }
        return;
    }

    UINT dpi = 96, dpiY = 96;
    HMONITOR monitor = MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST);
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &dpiY)) || !dpi) {
        dpi = 96;
    }
    bool focused = GetForegroundWindow() == target;

    if (it == g_overlays.end()) {
        Overlay fresh;
        if (!CreateParts(target, fresh)) {
            return;
        }
        fresh.focused = focused;
        StateColors(focused, &fresh.cur1, &fresh.cur2);
        fresh.to1 = fresh.cur1;
        fresh.to2 = fresh.cur2;
        it = g_overlays.emplace(target, fresh).first;
    }
    Overlay& ov = it->second;
    if (focused != ov.focused) {
        ov.focused = focused;
        StartFade(ov);
    }

    bool resized = frame.right - frame.left != ov.frame.right - ov.frame.left ||
                   frame.bottom - frame.top != ov.frame.bottom - ov.frame.top;
    bool moved = !EqualRect(&frame, &ov.frame);
    bool rescaled = dpi != ov.dpi;
    ov.frame = frame;
    ov.dpi = dpi;
    if (resized || rescaled || ov.dirty || !ov.shown) {
        PaintStrips(ov);
    } else if (moved) {
        MoveStrips(ov);
    }
    Restack(ov, target);
}

BOOL CALLBACK SyncWindow(HWND hwnd, LPARAM seen) {
    ((std::vector<HWND>*)seen)->push_back(hwnd);
    SyncTarget(hwnd);
    return TRUE;
}

// Full pass: every top-level window, plus a check that the accent color is
// current. Called on a timer, on focus changes and at startup.
void ResyncAll() {
    COLORREF accent = AccentRgb();
    if (accent != g_accent) {
        g_accent = accent;
        for (auto& entry : g_overlays) {
            if (entry.second.focused) {
                StartFade(entry.second);
            }
        }
    }
    // A ring whose window is no longer top-level (destroyed, or re-parented into
    // another window) would otherwise float on screen with nothing under it.
    std::vector<HWND> seen;
    EnumWindows(SyncWindow, (LPARAM)&seen);
    std::vector<HWND> gone;
    for (auto& entry : g_overlays) {
        if (std::find(seen.begin(), seen.end(), entry.first) == seen.end()) {
            gone.push_back(entry.first);
        }
    }
    for (HWND target : gone) {
        DestroyOverlay(target);
    }
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                         DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        ResyncAll();
        return;
    }
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }
    if (event == EVENT_OBJECT_DESTROY) {
        DestroyOverlay(hwnd);
        return;
    }
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) {
        DestroyOverlay(hwnd);
        return;
    }
    SyncTarget(hwnd);
}

LRESULT CALLBACK PartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kResyncTimer) {
                ResyncAll();
            } else if (wp == kFadeTimer) {
                FadeAll();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR cmdLine, int) {
    if (wcsstr(cmdLine, L"--stop")) {
        HWND running = FindWindowW(kMainClass, nullptr);
        if (running) {
            PostMessageW(running, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    CreateMutexW(nullptr, FALSE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LoadConfig();

    WNDCLASSEXW mainClass = {sizeof(mainClass)};
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = instance;
    mainClass.lpszClassName = kMainClass;
    RegisterClassExW(&mainClass);

    WNDCLASSEXW partClass = {sizeof(partClass)};
    partClass.lpfnWndProc = PartProc;
    partClass.hInstance = instance;
    partClass.lpszClassName = kPartClass;
    RegisterClassExW(&partClass);

    g_main = CreateWindowExW(0, kMainClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                             instance, nullptr);
    if (!g_main) {
        return 1;
    }

    const DWORD events[] = {
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
        EVENT_SYSTEM_MOVESIZEEND, EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW,
        EVENT_OBJECT_HIDE, EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED,
    };
    std::vector<HWINEVENTHOOK> hooks;
    for (DWORD event : events) {
        HWINEVENTHOOK hook = SetWinEventHook(event, event, nullptr, OnWinEvent, 0, 0,
                                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (hook) {
            hooks.push_back(hook);
        }
    }

    SetTimer(g_main, kResyncTimer, g_cfg.resyncMs, nullptr);
    ResyncAll();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (HWINEVENTHOOK hook : hooks) {
        UnhookWinEvent(hook);
    }
    std::vector<HWND> targets;
    for (auto& entry : g_overlays) {
        targets.push_back(entry.first);
    }
    for (HWND target : targets) {
        DestroyOverlay(target);
    }
    return 0;
}
