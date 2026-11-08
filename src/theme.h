#pragma once

#include <windows.h>

// Light/dark theme support for classic Win32 controls.
//
// Dark mode for common controls relies on undocumented uxtheme.dll ordinals
// (133 = AllowDarkModeForWindow, 135 = SetPreferredAppMode, 136 = FlushMenuThemes)
// plus DwmSetWindowAttribute for the title bar. All calls are resolved
// defensively so the app still works if any of them are unavailable.

namespace theme {

bool SystemPrefersDark();

// Must be called before the first window is created; safe to call again later
// to switch the process-wide mode at runtime.
void Init(bool dark);
bool IsDark();

void ApplyWindow(HWND hwnd);   // dark title bar
void StyleControl(HWND ctl);   // dark common-control theme for one child window
void StyleTree(HWND root);     // StyleControl() for every descendant
void Flush();                  // flush uxtheme colour caches after a theme switch

COLORREF WindowColor();
COLORREF FieldColor();
COLORREF TextColor();
COLORREF FieldTextColor();
COLORREF SelectionTextColor();
COLORREF SelectionBkColor();
COLORREF AccentColor();
COLORREF AccentHoverColor();
COLORREF AccentPressedColor();
COLORREF HoverColor();
COLORREF PressedColor();
COLORREF BorderColor();
COLORREF MutedTextColor();
COLORREF SeparatorColor();

HBRUSH WindowBrush();
HBRUSH FieldBrush();

}  // namespace theme