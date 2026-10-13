#pragma once

// ============================================================================
// ui theme_combo - a custom-drawn dropdown combo box.
//
// THREAD AFFINITY: like every other control in the shell, the window
// procedures created here run on the thread that created the windows (the UI
// thread). The state hangs off GWLP_USERDATA and is not synchronised.
// ============================================================================
//
// WHAT THIS IS FOR
// ----------------
// The parameter panel builds one combo per shader parameter plus one for the
// shader selector, and every one of them speaks the stock Win32 combo
// protocol: CB_ADDSTRING / CB_RESETCONTENT / CB_GETCURSEL / CB_SETCURSEL /
// CB_GETLBTEXT with CB_ERR as the failure sentinel, and a parent WM_COMMAND
// carrying CBN_SELCHANGE (BN_SELCHANGE in <windows.h>). ThemeCombo is a
// drop-in replacement for that protocol - it is not a native COMBOBOX and it
// does not pretend to be one, but a caller cannot tell the difference.
//
// It is a DROPDOWN LIST, not a dropdown combo: there is no edit control and no
// free text entry, matching CBS_DROPDOWNLIST.
//
// HOW IT IS DRAWN
// ---------------
// Two window classes: the closed-state control, and a separate WS_POPUP list
// window owned by the control. Both paint through ui::gfx::Canvas, which is
// DC-backed (ID2D1DCRenderTarget over the HDC from BeginPaint) and creates no
// graphics device. See d2d_canvas.h for why that constraint exists and
// theme_combo.cpp for the same note at the top of the implementation.
// ============================================================================

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

class ThemeCombo {
public:
    // Registers both window classes: the closed-state control and the popup
    // list. Call once at startup; calling again is harmless and returns true.
    // Returns false only when registration fails for a reason other than
    // ERROR_CLASS_ALREADY_EXISTS. `instance` of nullptr means the module that
    // owns this code.
    static bool register_class(HINSTANCE instance);

    // Class name of the closed-state control, for callers that would rather
    // build the window with CreateWindowExW themselves (the control id is
    // then taken from the HMENU argument, exactly like a native combo).
    static const wchar_t* class_name();

    // Dropdown-list behaviour: no free text entry, matching CBS_DROPDOWNLIST.
    // A height of 0 (or less) uses ui::theming::Metrics::control_height, which
    // is what the parameter panel wants.
    static HWND create(HWND parent, int id, int x, int y, int width, int height);
};

} // namespace ui::controls