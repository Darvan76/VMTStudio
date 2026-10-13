#pragma once

// ui theme_slider - a themed, horizontal slider that is a drop-in replacement
// for the common control `TRACKBAR_CLASSW`.
//
// The parameter panel builds one row per shader parameter and puts a slider next
// to every numeric field, so this control is on screen dozens of times at once
// and is the single most interacted-with widget in the shell. It exists because
// the stock trackbar draws itself: there is no way to get it to use the Fluent
// palette from win32_theme.h without a theme hack per process, and the stock
// visuals (ticks, flat groove, square thumb) do not match the rest of the shell.
//
// ============================================================================
// MESSAGE COMPATIBILITY - THIS IS THE POINT OF THE CONTROL
// ============================================================================
// The shell already talks to its sliders with the native trackbar messages, so
// this control speaks the same protocol and nothing above it needs to change:
//
//   TBM_SETRANGEMIN   wParam TRUE  -> minimum := lParam (absolute)
//                     wParam FALSE -> minimum := minimum + lParam (relative)
//   TBM_SETRANGEMAX   same convention
//   TBM_GETRANGEMIN   -> minimum
//   TBM_GETRANGEMAX   -> maximum
//   TBM_SETPOS        wParam TRUE  -> also sends WM_HSCROLL to the parent
//                     wParam FALSE -> silent
//   TBM_GETPOS        -> current position, always authoritative
//   TBM_SETPAGESIZE   -> returns the previous page size
//   TBM_GETPAGESIZE   -> current page size
//
// User interaction (click, drag, keyboard) additionally sends WM_HSCROLL to the
// parent with `lParam` set to **the slider's own HWND**, exactly like the real
// trackbar: the parameter panel resolves which row changed with
// `reinterpret_cast<HWND>(lparam)` and then re-reads TBM_GETPOS.
//
// The range is inclusive at both ends, the position is always clamped into it,
// and an inverted (min > max) or degenerate (min == max) range is tolerated
// without dividing by a negative or zero span.
//
// ============================================================================
// RENDERING CONSTRAINT
// ============================================================================
// Painting goes through ui::gfx::Canvas, which builds an ID2D1DCRenderTarget
// over the WM_PAINT HDC. This control creates no D3D/DXGI device and no swap
// chain, so it never contends with the Source engine's D3D9 device in this
// process. See d2d_canvas.h for the full rationale.
// ============================================================================
//
// THREAD AFFINITY: created, messaged and painted from the UI thread only, like
// every other window in the shell. The per-window state lives in a
// GWLP_USERDATA block owned by the window procedure.

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

namespace ui::controls {

class ThemeSlider {
public:
    // Idempotent: returns true when the class is registered by this call or was
    // already registered by an earlier one.
    static bool register_class(HINSTANCE instance);

    // Process-wide class name, also accepted by CreateWindowExW.
    static const wchar_t* class_name();

    // Horizontal only - that is all the parameter panel uses.
    //
    // The control is a child window and is created focusable (WS_TABSTOP) with
    // no ticks, matching how the shell used TRACKBAR_CLASSW. Registering the
    // class is attempted first, so this is safe to call without a separate
    // register_class() call. Returns nullptr on failure.
    static HWND create(HWND parent, int id, int x, int y, int width, int height);

    // Convenience accessors that bypass SendMessage. Every one of them is a
    // no-op on a null or non-slider window.
    static int position(HWND slider);
    static void set_range(HWND slider, int minimum, int maximum);
    static void set_position(HWND slider, int value, bool notify);
    static void set_page_size(HWND slider, int page);
};

} // namespace ui::controls