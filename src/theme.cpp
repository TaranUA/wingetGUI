#include "theme.h"

#include <dwmapi.h>

namespace theme {
namespace {

bool g_dark = false;
HBRUSH g_windowBrush = nullptr;
HBRUSH g_fieldBrush = nullptr;

const COLORREF kDarkWindow  = RGB(32, 32, 32);
const COLORREF kDarkField   = RGB(25, 25, 25);
const COLORREF kDarkText    = RGB(236, 236, 236);
const COLORREF kDarkSelText = RGB(255, 255, 255);
const COLORREF kFallbackAccent = RGB(0, 120, 212);  // Windows 10 default blue

// Accent-derived colours, set by Init(). Following the user's accent keeps
// the primary buttons and selection highlights in step with personalisation;
// the fallback covers machines where the DWM cannot report the colour.
COLORREF g_accent = kFallbackAccent;
COLORREF g_accentHover = RGB(16, 118, 205);
COLORREF g_accentPressed = RGB(0, 98, 176);
COLORREF g_darkSelection = RGB(0, 95, 184);

// SetPreferredAppMode values
enum PreferredAppMode { AppModeDefault = 0, AppModeAllowDark = 1,
                        AppModeForceDark = 2, AppModeForceLight = 3 };

// uxtheme.dll ordinals (stable from Windows 10 1809 through Windows 11)
const int kOrdAllowDarkModeForWindow = 133;
const int kOrdSetPreferredAppMode    = 135;
const int kOrdFlushMenuThemes        = 136;

using FnSetPreferredAppMode    = int (WINAPI*)(int);
using FnAllowDarkModeForWindow = BOOL (WINAPI*)(HWND, BOOL);
using FnFlushMenuThemes        = void (WINAPI*)(void);

HMODULE UxTheme() {
    // Loaded once from Init() (UI thread) and then only read, so no
    // thread-safe local-static guard is needed.
    static HMODULE mod = nullptr;
    if (!mod) {
        mod = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!mod) mod = LoadLibraryW(L"uxtheme.dll");
    }
    return mod;
}

void* Ordinal(int value) {
    return reinterpret_cast<void*>(static_cast<ULONG_PTR>(value));
}

// Resolves an undocumented uxtheme entry point. The cast goes through void* so
// the compiler does not warn about converting between function types.
template <typename Fn>
Fn Resolve(HMODULE mod, int ordinal) {
    if (!mod) return nullptr;
    FARPROC proc = GetProcAddress(mod, reinterpret_cast<LPCSTR>(Ordinal(ordinal)));
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(proc));
}

void SetPreferredAppMode(int mode) {
    auto fn = Resolve<FnSetPreferredAppMode>(UxTheme(), kOrdSetPreferredAppMode);
    if (fn) fn(mode);
}

void FlushMenuThemes() {
    auto fn = Resolve<FnFlushMenuThemes>(UxTheme(), kOrdFlushMenuThemes);
    if (fn) fn();
}

void AllowDarkModeForWindow(HWND hwnd, BOOL allow) {
    if (!hwnd) return;
    auto fn = Resolve<FnAllowDarkModeForWindow>(UxTheme(), kOrdAllowDarkModeForWindow);
    if (fn) fn(hwnd, allow);
}

// Scales every channel by num/den, clamped: BrighterBy with num>den, DarkerBy
// with num<den.
COLORREF BrighterBy(COLORREF c, int num, int den) {
    auto ch = [num, den](int v) -> int {
        v = v * num / den;
        return v > 255 ? 255 : v;
    };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

COLORREF DarkerBy(COLORREF c, int num, int den) {
    auto ch = [num, den](int v) -> int {
        v = v * num / den;
        return v < 0 ? 0 : v;
    };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

}  // namespace

bool SystemPrefersDark() {
    DWORD value = 1;  // assume light when the key is missing
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD size = sizeof(value);
        DWORD type = 0;
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
    }
    return value == 0;
}

void Init(bool dark) {
    g_dark = dark;

    // The user's accent colour, as the DWM reports it for glass composition.
    // The value is in 0x00BBGGRR layout, i.e. a plain COLORREF.
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        const COLORREF fromSystem = static_cast<COLORREF>(color & 0x00FFFFFF);
        if (fromSystem != 0) g_accent = fromSystem;
    } else {
        g_accent = kFallbackAccent;
    }

    // Hover is a touch lighter and pressed a touch darker, in both themes;
    // the dark selection background is a darker variant of the same accent.
    g_accentHover   = BrighterBy(g_accent, 9, 8);
    g_accentPressed = DarkerBy(g_accent, 7, 8);
    g_darkSelection = DarkerBy(g_accent, 4, 5);

    SetPreferredAppMode(dark ? AppModeForceDark : AppModeForceLight);
    FlushMenuThemes();
}

bool IsDark() { return g_dark; }

void ApplyWindow(HWND hwnd) {
    if (!hwnd) return;
    AllowDarkModeForWindow(hwnd, g_dark ? TRUE : FALSE);

    BOOL value = g_dark ? TRUE : FALSE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE on Windows 10 1903+ / Windows 11,
    // 19 on Windows 10 1809-1903.
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value)))) {
        DwmSetWindowAttribute(hwnd, 19, &value, sizeof(value));
    }

    // 33 = DWMWA_WINDOW_CORNER_PREFERENCE, 2 = DWMWCP_ROUND (Windows 11 only).
    int corner = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
}

void StyleControl(HWND ctl) {
    if (!ctl) return;
    AllowDarkModeForWindow(ctl, g_dark ? TRUE : FALSE);
    if (g_dark) {
        SetWindowTheme(ctl, L"DarkMode_Explorer", nullptr);
    } else {
        SetWindowTheme(ctl, L"", L"");
    }
}

void StyleTree(HWND root) {
    StyleControl(root);
    EnumChildWindows(root, [](HWND child, LPARAM) -> BOOL {
        StyleControl(child);
        return TRUE;
    }, 0);
}

void Flush() {
    FlushMenuThemes();
}

COLORREF WindowColor()     { return g_dark ? kDarkWindow : GetSysColor(COLOR_BTNFACE); }
COLORREF FieldColor()      { return g_dark ? kDarkField  : GetSysColor(COLOR_WINDOW); }
COLORREF TextColor()       { return g_dark ? kDarkText   : GetSysColor(COLOR_BTNTEXT); }
COLORREF FieldTextColor()  { return g_dark ? kDarkText   : GetSysColor(COLOR_WINDOWTEXT); }
COLORREF SelectionTextColor() { return g_dark ? kDarkSelText : GetSysColor(COLOR_HIGHLIGHTTEXT); }
COLORREF SelectionBkColor()   { return g_dark ? g_darkSelection : GetSysColor(COLOR_HIGHLIGHT); }
COLORREF AccentColor()        { return g_accent; }
COLORREF AccentHoverColor()   { return g_accentHover; }
COLORREF AccentPressedColor() { return g_accentPressed; }
COLORREF HoverColor()         { return g_dark ? RGB(48, 48, 48)   : RGB(242, 242, 242); }
COLORREF PressedColor()       { return g_dark ? RGB(60, 60, 60)   : RGB(228, 228, 228); }
COLORREF BorderColor()        { return g_dark ? RGB(72, 72, 72)   : RGB(200, 200, 200); }
COLORREF MutedTextColor()     { return g_dark ? RGB(150, 150, 150) : RGB(110, 110, 110); }
COLORREF SeparatorColor()     { return g_dark ? RGB(48, 48, 48)   : RGB(232, 232, 232); }

HBRUSH WindowBrush() {
    if (!g_dark) return GetSysColorBrush(COLOR_BTNFACE);
    if (!g_windowBrush) g_windowBrush = CreateSolidBrush(kDarkWindow);
    return g_windowBrush;
}

HBRUSH FieldBrush() {
    if (!g_dark) return GetSysColorBrush(COLOR_WINDOW);
    if (!g_fieldBrush) g_fieldBrush = CreateSolidBrush(kDarkField);
    return g_fieldBrush;
}

}  // namespace theme