// Registration and window-chrome glue shared by every top-level window in the
// shell (main window, material browser, the three dialogs, the validation
// window).
//
// This exists as its own tiny unit so the per-window code only has to call
// three functions at the right moments instead of remembering the exact
// ordering of theme application, control-class registration and
// WM_SETTINGCHANGE handling.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ui::controls {

// Registers every custom control class exactly once. Call before creating any
// child controls. Idempotent and process-global, so it is safe to call from
// every window's creation path. Returns true when all classes registered.
bool register_all(HINSTANCE instance);

// Dark caption + rounded corners, re-reading the active theme mode each time.
void apply_chrome(HWND top_level);

// Optional Mica/Acrylic backdrop. No-op below Windows 11 build 22621.
void apply_backdrop(HWND top_level, bool acrylic);

// Re-themes and repaints an existing subtree after the theme changed.
void refresh_theme(HWND top_level);

// Feed WM_SETTINGCHANGE's lParam straight in. Returns true when it was a theme
// change and the window has been re-themed.
bool handle_theme_change(HWND top_level, LPARAM setting_change_param);

// Re-metrics and repaints after a DPI change. Call from WM_DPICHANGED after
// laying out children, so children pick up the new dpi on their next paint.
void handle_dpi_change(HWND top_level);

} // namespace ui::controls