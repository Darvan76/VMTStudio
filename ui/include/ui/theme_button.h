#pragma once

// ui theme_button - a custom-painted Win32 button that replaces the stock BUTTON
// class without changing any of the call sites.
//
// ============================================================================
// DROP-IN COMPATIBILITY
// ============================================================================
// Existing code sends BM_GETCHECK / BM_SETCHECK and reads BN_CLICKED / BN_DBLCLK
// out of WM_COMMAND exactly as it would from a native BS_PUSHBUTTON. That is a
// hard contract: this control must never require the shell to change.
//
// The window text of the control *is* its label (CreateWindowExW's lpWindowName),
// so GetWindowTextW / WM_GETTEXT keep working and accessibility tools see the
// caption. The control id passed to create() is the id delivered in WM_COMMAND.
//
// Painting goes through ui::gfx::Canvas, which binds an ID2D1DCRenderTarget to
// the WM_PAINT HDC. This control creates no graphics device of any kind - see
// ui/src/theme_button.cpp for why that matters in this process.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <string>

namespace ui::controls {

// Visual weight. Not a Win32 "style" - it maps onto colour roles only.
enum class ButtonStyle {
    Default,     // neutral: surface_alt background, border hairline
    Accent,      // primary action: accent background, text_on_accent label
    Subtle,      // toolbar-ish: transparent until hovered
};

// Behaviour. All three report activation the same way (WM_COMMAND / BN_CLICKED).
enum class ButtonKind {
    Push,
    Toggle,      // sticky push/pressed state (not checkable)
    Checkbox,    // renders a checkbox + label; supports BM_GETCHECK/BM_SETCHECK
};

class ThemeButton {
public:
    // Registers the window class. Idempotent: safe to call from several windows.
    // Passing nullptr uses the module handle of the running executable.
    static bool register_class(HINSTANCE instance);
    static const wchar_t* class_name();

    // Creates a child button. `id` becomes the control id delivered in WM_COMMAND.
    // Registers the class on first use, so calling this without a prior
    // register_class() is fine. Returns nullptr on failure.
    static HWND create(HWND parent, int id, const std::wstring& label, int x, int y,
                       int width, int height, ButtonKind kind = ButtonKind::Push,
                       ButtonStyle style = ButtonStyle::Default);

    // Push/Toggle state, used by Toggle. Reading or writing a window that is not
    // one of ours is safe and simply reports false / does nothing.
    static bool get_toggled(HWND button);
    static void set_toggled(HWND button, bool toggled);
};

} // namespace ui::controls