#pragma once

// ui win32_theme - Fluent 2 flavoured palette + window chrome for the Win32 shell.
//
// Everything here is pure Win32: a palette table, a DPI metric table, and the
// few DWM / UxTheme calls needed to make a classic Win32 window look like a
// Windows 11 one. Nothing here paints; callers take colours from `palette()`
// and metrics from `metrics_for()` and draw with whatever backend they use.
//
// THREAD AFFINITY: the theme override and the cached palette are process-global
// state. They are expected to be touched only from the UI thread (the thread
// that owns the HWNDs). No locking is taken; adding it would imply the state is
// safe to share, which it is not.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ui::theming {

enum class ThemeMode { Light, Dark };

// Colour roles for the shell. Every field is a real Fluent 2 token (dark or
// light variant) so the window chrome, the common controls and any custom-painted
// surface agree on the same ramp. Contrast-critical pairs (text on surface,
// text_on_accent on accent) are chosen to clear WCAG AA (>= 4.5:1).
struct Palette {
    COLORREF surface;          // window / panel background
    COLORREF surface_alt;      // row or card background, one step up
    COLORREF surface_hover;
    COLORREF surface_pressed;
    COLORREF surface_sunken;   // text fields and list wells
    COLORREF border;           // default 1px hairline
    COLORREF border_strong;    // focused / active tab
    COLORREF divider;
    COLORREF text;
    COLORREF text_muted;
    COLORREF text_disabled;
    COLORREF text_on_accent;
    COLORREF accent;
    COLORREF accent_hover;
    COLORREF accent_pressed;
    COLORREF accent_muted;     // selected row tint
    COLORREF focus_ring;
    COLORREF danger;
    COLORREF warning;
    COLORREF success;
};

// Stable enum <-> Palette mapping. `color()` is the accessor to use from painting
// code; `Palette` is the aggregate to keep when you want the whole ramp at once.
enum class ColorRole {
    Surface, SurfaceAlt, SurfaceHover, SurfacePressed, SurfaceSunken,
    Border, BorderStrong, Divider,
    Text, TextMuted, TextDisabled, TextOnAccent,
    Accent, AccentHover, AccentPressed, AccentMuted, FocusRing,
    Danger, Warning, Success
};

// Sizes are in DESIGN UNITS (the 96 dpi values); call scale() before you hand
// them to a Win32 API or to a drawing backend, otherwise the UI will not follow
// per-monitor DPI.
struct Metrics {
    int dpi;
    int scale(int design_px) const;   // MulDiv(design_px, dpi, 96)
    int corner_radius;               // design units, 96dpi: 6
    int border_width;                // 1
    int focus_ring_width;            // 1
    int row_height;                  // 28
    int control_height;              // 24
    int padding;                     // 8
    int gap;                         // 6
};

// HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize ->
// AppsUseLightTheme. Returns Light when the key or value cannot be read
// (Windows 7, a locked-down profile, or a stripped-down container image).
ThemeMode system_theme_mode();

// Override if one is set, otherwise whatever the system currently reports.
// This is the value every other function here keys off.
ThemeMode active_theme_mode();

// Process-global override (see the thread-affinity note at the top of this file).
// These do not persist and do not notify windows; after changing the override
// call apply_window_chrome() + apply_subtree() on the affected windows.
void set_theme_override(ThemeMode mode);
void clear_theme_override();

// Reference-valid until the active mode changes. Cheap: no registry hit when an
// override is set, and the palette itself is rebuilt only when the mode flips.
const Palette& palette();

// Full ramp for a specific mode, independent of any override.
Palette palette_for(ThemeMode mode);

// Convenience accessor for painting code.
COLORREF color(ColorRole role);

// DPI of `hwnd` via GetDpiForWindow, resolved dynamically so there is no static
// dependency on Win10 1607+. Falls back to 96 for a null window or when the
// call is unavailable / returns 0.
Metrics metrics_for(HWND hwnd);

// Dark mode attribute + rounded corners for the frame. Re-reads
// active_theme_mode(), so calling it again after a mode change is all it takes
// to re-theme. Every DWM call is best effort: on Windows 10, with composition
// disabled, or on an attribute this build does not know, this is a silent no-op.
void apply_window_chrome(HWND hwnd);

// Opts the *process* into UxTheme's dark visuals, which is what makes menus and
// the remaining common controls render dark. Call once before creating windows.
// No-op in Light mode, and a silent no-op on any build where the (undocumented,
// ordinal-resolved) entry points are absent.
void enable_native_dark_mode();

// Opts a single window in. Called per top-level window from apply_chrome.
void allow_native_dark_mode(HWND hwnd);

// System backdrop (Mica/Acrylic). Acrylic only on build 22621+, silently ignored
// anywhere older.
void apply_backdrop(HWND hwnd, bool acrylic);

// Recursive SetWindowTheme pass over every descendant of `root` plus a redraw.
// In Dark mode the per-class dark theme name is applied; in Light mode the theme
// is reset to nullptr so no dark visual style is left baked in.
void apply_subtree(HWND root);

// True for the WM_SETTINGCHANGE payload that means "the system theme changed".
bool is_theme_change(LPARAM setting_change_param);

// Repaint everything under `root`, `root` included.
void redraw_subtree(HWND root);

// Brush for the current surface colour, for use as a class background brush or
// from WM_ERASEBKGND. Cached against the resolved colour and rebuilt when the
// theme changes.
HBRUSH surface_brush();

// Same, for the sunken (input well) surface used by edit boxes and list boxes.
HBRUSH sunken_brush();

// General form of the above, for WM_CTLCOLOR* handlers that need a background
// matching a specific palette role.
HBRUSH theme_brush(ColorRole role);

} // namespace ui::theming
