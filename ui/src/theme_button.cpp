#include "ui/theme_button.h"

// ============================================================================
// ui theme_button - implementation.
//
// ============================================================================
// GRAPHICS ARCHITECTURE CONSTRAINT - READ THIS FIRST
// ============================================================================
//
// This translation unit creates **no graphics device of any kind**.
//
//   * no D3D9 / D3D10 / D3D11 / D3D12 device,
//   * no DXGI device, no swap chain, no D2D1RenderTarget over a DXGI surface,
//   * no ID2D1Factory::CreateHwndRenderTarget,
//   * no Direct3DCreateDevice / D3D11CreateDevice / D3D11CreateDeviceAndSwapChain.
//
// The reason is specific to this application, not to this control. The process
// loads the Source engine's `shaderapidx9.dll`, which creates and owns a D3D9
// device on its own child HWND inside this very process, and the project has
// previously hit faults during engine device resets. Every button in the UI has
// to be able to repaint while that device is mid-reset, so the UI layer must
// never contend with the engine for the graphics stack.
//
// The one and only Direct2D object involved is the **ID2D1DCRenderTarget** that
// ui::gfx::Canvas builds from the HDC handed to us inside WM_PAINT. A DC render
// target rasterises directly into GDI-compatible memory: it allocates no device,
// no swap chain and no driver-visible resource. The engine can reset, lose or
// tear down its device and button painting carries on regardless.
//
// (d2d1.h transitively pulls in dxgi.h, because D2D1_PIXEL_FORMAT is defined in
// terms of DXGI_FORMAT. That is a header dependency only - nothing here ever
// creates a DXGI object or references DXGI's device/swap-chain interfaces.)
//
// ============================================================================
// THREADING
// ============================================================================
// The control keeps one heap-allocated State per window in GWLP_USERDATA and
// nothing else mutable. The theme palette and the DirectWrite format cache it
// reads are documented as UI-thread-affine in their own headers, so this file
// assumes the same single UI thread that owns the HWNDs.
//
// ============================================================================

#include "ui/d2d_canvas.h"
#include "ui/win32_theme.h"

#include <new>

namespace ui::controls {
namespace {

// ---------------------------------------------------------------------------
// Class identity and tunables
// ---------------------------------------------------------------------------

constexpr wchar_t kClassName[] = L"VMT.Previewer.ThemeButton";

// Ties the GWLP_USERDATA block to this control, so get_toggled()/set_toggled()
// never dereference a window that happens to belong to somebody else.
constexpr unsigned long kStateMagic = 0x5442544Eul; // 'TBTN'

// Label size in points. DirectWrite stores font size in DIPs on a 96 dpi basis
// and the render target's DPI scales it, so one value serves every monitor.
constexpr float kLabelFontPt = 9.5f;

// Checkmark vertices as fractions of the box side, measured from its top-left
// corner: shoulder -> elbow -> tip, drawn as two straight segments.
constexpr float kCheckX0 = 0.22f;
constexpr float kCheckY0 = 0.52f;
constexpr float kCheckX1 = 0.42f;
constexpr float kCheckY1 = 0.72f;
constexpr float kCheckX2 = 0.78f;
constexpr float kCheckY2 = 0.28f;

// Upper bound on a fetched window caption. Buttons are short by construction;
// this only stops a pathological SetWindowText from becoming a huge allocation.
constexpr int kMaxCaptionChars = 512;

// ---------------------------------------------------------------------------
// Per-window state
// ---------------------------------------------------------------------------

struct CreateParams {
    ButtonKind kind;
    ButtonStyle style;
};

struct State {
    unsigned long magic = kStateMagic;
    ButtonKind kind = ButtonKind::Push;
    ButtonStyle style = ButtonStyle::Default;

    bool hovered = false;
    bool pressed = false;
    // Whether a WM_MOUSELEAVE is actually armed, so we never arm it twice and
    // never clear the flag for a leave we did not ask for.
    bool tracking = false;

    // Sticky on-state. For ButtonKind::Checkbox this is what BM_GETCHECK reads
    // and BM_SETCHECK writes; for ButtonKind::Toggle it is what get_toggled()
    // exposes. ButtonKind::Push never uses it.
    bool toggled = false;

    DWORD last_click_time = 0;
    POINT last_click_pos = {0, 0};
};

State* state_of(HWND hwnd) {
    if (hwnd == nullptr) {
        return nullptr;
    }
    const LONG_PTR raw = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (raw == 0) {
        return nullptr;
    }
    State* state = reinterpret_cast<State*>(raw);
    return (state->magic == kStateMagic) ? state : nullptr;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

int scale_or_one(const ui::theming::Metrics& metrics, int design_px) {
    const int scaled = metrics.scale(design_px);
    return (scaled > 0) ? scaled : 1;
}

int clamp_int(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    return (value > high) ? high : value;
}

// WM_LBUTTONDOWN / WM_LBUTTONUP carry the point as a signed 16-bit pair packed
// into LPARAM. Unpacking it by hand avoids a dependency on <windowsx.h> for the
// two GET_*_LPARAM macros, and the sign extension is what stops a click at
// x = 40000 from wrapping negative.
POINT unpack_point(LPARAM lparam) {
    POINT p;
    p.x = static_cast<LONG>(static_cast<short>(LOWORD(lparam)));
    p.y = static_cast<LONG>(static_cast<short>(HIWORD(lparam)));
    return p;
}

bool point_in_client(HWND hwnd, LPARAM lparam) {
    const POINT p = unpack_point(lparam);
    RECT client;
    if (GetClientRect(hwnd, &client) == 0) {
        return false;
    }
    return p.x >= client.left && p.x < client.right && p.y >= client.top && p.y < client.bottom;
}

// Unsigned wrap is the intended behaviour: GetTickCount-style counters roll over
// roughly every 49 days and a plain subtraction handles that correctly.
LONG tick_delta(DWORD later, DWORD earlier) {
    return static_cast<LONG>(later - earlier);
}

// A COLORREF-as-HBRUSH is the documented (if ugly) idiom for painting a flat
// colour with GDI: the value is the brush handle itself, not a pointer to one,
// so this has to be a reinterpret_cast rather than a static_cast.
HBRUSH solid_brush(COLORREF color) {
    return reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(color) + 1);
}

std::wstring window_text(HWND hwnd) {
    const int needed = GetWindowTextLengthW(hwnd);
    if (needed <= 0) {
        return std::wstring();
    }
    const int cap = clamp_int(needed, 1, kMaxCaptionChars);
    std::wstring text(static_cast<std::size_t>(cap) + 1u, L'\0');
    const int copied = GetWindowTextW(hwnd, &text[0], cap + 1);
    if (copied <= 0) {
        return std::wstring();
    }
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

// ---------------------------------------------------------------------------
// Font for the DrawTextW fallback path
// ---------------------------------------------------------------------------
//
// ui::gfx::text_format() returns null only when DirectWrite could not build a
// format at all (no factory, or no system font collection). That is rare, but a
// button with no caption is worse than one drawn through GDI, so the fallback
// exists. The HFONT is cached process-wide and rebuilt only when the DPI
// changes; the previous handle is destroyed so repeated painting cannot leak GDI
// objects. UI-thread only, like everything else in this file.

HFONT fallback_font(int dpi) {
    static HFONT cached = nullptr;
    static int cached_dpi = 0;

    if (cached != nullptr && cached_dpi == dpi) {
        return cached;
    }
    if (cached != nullptr) {
        DeleteObject(cached);
        cached = nullptr;
        cached_dpi = 0;
    }

    const int safe_dpi = (dpi > 0) ? dpi : 96;
    // A negative lfHeight is a character height rather than a cell height.
    // 9.5pt at 96dpi is 12.67px, hence MulDiv(pt * 4, dpi, 72 * 4).
    const int height = -MulDiv(static_cast<int>(kLabelFontPt * 4.0f), safe_dpi, 288);
    cached = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (cached != nullptr) {
        cached_dpi = safe_dpi;
    }
    return cached;
}

void draw_label_gdi(HDC hdc, const std::wstring& label, RECT rect,
                    ui::theming::ColorRole role, int dpi) {
    HFONT font = fallback_font(dpi);
    if (font == nullptr) {
        return;
    }
    const HGDIOBJ old_font = SelectObject(hdc, font);
    const COLORREF old_color = SetTextColor(hdc, ui::theming::color(role));
    const int old_mode = SetBkMode(hdc, TRANSPARENT);

    DrawTextW(hdc, label.c_str(), static_cast<int>(label.size()), &rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    SetBkMode(hdc, old_mode);
    SetTextColor(hdc, old_color);
    SelectObject(hdc, old_font);
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void notify_parent(HWND hwnd, int control_id, UINT code) {
    HWND parent = GetParent(hwnd);
    if (parent == nullptr) {
        return;
    }
    // Identical shape to a native BUTTON: control id in the low word,
    // notification in the high word, source HWND in lParam. Existing shell code
    // reads this with LOWORD()/HIWORD() and needs no changes.
    const WPARAM wparam = MAKEWPARAM(static_cast<WORD>(control_id), static_cast<WORD>(code));
    SendMessageW(parent, WM_COMMAND, wparam, reinterpret_cast<LPARAM>(hwnd));
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

void set_toggled_state(HWND hwnd, State& state, bool on) {
    if (state.toggled == on) {
        return;
    }
    state.toggled = on;
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Colour resolution
// ---------------------------------------------------------------------------

struct Palette {
    ui::theming::ColorRole fill;
    ui::theming::ColorRole stroke;
    ui::theming::ColorRole label;
    bool draw_fill;
    bool draw_stroke;
};

Palette resolve_palette(const State& state, bool disabled) {
    Palette out = {};
    out.draw_fill = true;
    out.draw_stroke = true;

    const bool hover = state.hovered;
    const bool press = state.pressed;

    switch (state.style) {
    case ButtonStyle::Default:
        out.fill = press ? ui::theming::ColorRole::SurfacePressed
                         : (hover ? ui::theming::ColorRole::SurfaceHover
                                  : ui::theming::ColorRole::SurfaceAlt);
        out.stroke = hover ? ui::theming::ColorRole::BorderStrong
                           : ui::theming::ColorRole::Border;
        out.label = ui::theming::ColorRole::Text;
        break;

    case ButtonStyle::Accent:
        out.fill = press ? ui::theming::ColorRole::AccentPressed
                         : (hover ? ui::theming::ColorRole::AccentHover
                                  : ui::theming::ColorRole::Accent);
        out.stroke = ui::theming::ColorRole::AccentPressed;
        out.label = ui::theming::ColorRole::TextOnAccent;
        break;

    case ButtonStyle::Subtle:
        out.fill = press ? ui::theming::ColorRole::SurfacePressed
                         : ui::theming::ColorRole::SurfaceHover;
        out.label = ui::theming::ColorRole::Text;
        // No opaque plate over the panel underneath. Only hover or press paints
        // anything, which is what makes this usable in a toolbar.
        out.draw_fill = hover || press;
        out.draw_stroke = false;
        break;
    }

    // A sticky toggle overrides its style completely: muted accent plate,
    // accent hairline, accent label.
    if (state.kind == ButtonKind::Toggle && state.toggled) {
        out.fill = ui::theming::ColorRole::AccentMuted;
        out.stroke = ui::theming::ColorRole::Accent;
        out.label = ui::theming::ColorRole::Accent;
        out.draw_fill = true;
        out.draw_stroke = true;
    }

    // Disabled has the last word: an accent button loses its accent entirely, and
    // a latched toggle stops advertising itself.
    if (disabled) {
        out.fill = ui::theming::ColorRole::SurfaceAlt;
        out.stroke = ui::theming::ColorRole::Border;
        out.label = ui::theming::ColorRole::TextDisabled;
        out.draw_fill = true;
        out.draw_stroke = true;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Background
// ---------------------------------------------------------------------------

// The window class registers no background brush, so nothing is erased for us
// and every WM_PAINT starts from whatever was last on screen. Redirecting the
// erase to the parent is the standard fix, and it is what makes the rounded
// corners and the transparent Subtle plate read correctly over an arbitrary
// parent background instead of over stale pixels.
void paint_parent_background(HWND hwnd, HDC hdc, const RECT& client) {
    HWND parent = GetParent(hwnd);
    if (parent == nullptr) {
        RECT scratch = client;
        FillRect(hdc, &scratch, solid_brush(ui::theming::color(ui::theming::ColorRole::Surface)));
        return;
    }

    POINT corners[2];
    corners[0].x = client.left;
    corners[0].y = client.top;
    corners[1].x = client.right;
    corners[1].y = client.bottom;
    MapWindowPoints(hwnd, parent, corners, 2);

    RECT in_parent;
    in_parent.left = corners[0].x;
    in_parent.top = corners[0].y;
    in_parent.right = corners[1].x;
    in_parent.bottom = corners[1].y;

    SendMessageW(parent, WM_ERASEBKGND, reinterpret_cast<WPARAM>(hdc),
                 reinterpret_cast<LPARAM>(&in_parent));
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void paint(HWND hwnd, State& state, HDC hdc) {
    RECT client;
    if (GetClientRect(hwnd, &client) == 0) {
        return;
    }
    if (client.right <= client.left || client.bottom <= client.top) {
        return;
    }

    paint_parent_background(hwnd, hdc, client);

    const ui::theming::Metrics metrics = ui::theming::metrics_for(hwnd);
    const bool disabled = (GetWindowLongW(hwnd, GWL_STYLE) & WS_DISABLED) != 0;
    const bool focused = (GetFocus() == hwnd);

    const int height = client.bottom - client.top;

    const int padding = metrics.scale(metrics.padding);
    const int radius = metrics.scale(metrics.corner_radius);
    const int border_w = scale_or_one(metrics, metrics.border_width);
    const int ring_w = scale_or_one(metrics, metrics.focus_ring_width);
    const int gap = metrics.scale(metrics.gap);

    const Palette palette = resolve_palette(state, disabled);

    // Inset the body by half the stroke (rounded up) so a 1px border is drawn
    // fully inside the client rect instead of being clipped in half by the edge.
    const int body_inset = palette.draw_stroke ? (border_w + 1) / 2 : 0;
    RECT body_rect = client;
    body_rect.left += body_inset;
    body_rect.top += body_inset;
    body_rect.right -= body_inset;
    body_rect.bottom -= body_inset;
    if (body_rect.right <= body_rect.left || body_rect.bottom <= body_rect.top) {
        body_rect = client;
    }

    // Checkbox geometry first, because it eats into the label rect.
    const bool is_checkbox = (state.kind == ButtonKind::Checkbox);
    RECT box;
    ZeroMemory(&box, sizeof(box));
    if (is_checkbox) {
        const int control_h = metrics.scale(metrics.control_height);
        const int floor_side = scale_or_one(metrics, 10);
        const int room = height - 2 * padding;
        const int max_side = (room > floor_side) ? room : floor_side;
        const int side = clamp_int(control_h, floor_side, max_side);
        box.left = padding;
        box.top = (height - side) / 2;
        box.right = box.left + side;
        box.bottom = box.top + side;
    }

    RECT label_rect = client;
    if (is_checkbox) {
        label_rect.left = box.right + gap;
    }
    if (label_rect.right - label_rect.left < padding) {
        label_rect.right = label_rect.left + padding;
    }

    ui::gfx::Canvas canvas(hdc);
    if (!canvas.valid()) {
        // Direct2D is unavailable - no factory, or the DC render target could not
        // bind. The parent's background is already on the HDC; put down a plain
        // plate so the control still reads as a control.
        if (palette.draw_fill) {
            RECT scratch = client;
            FillRect(hdc, &scratch, solid_brush(ui::theming::color(palette.fill)));
        }
        return;
    }

    canvas.set_dpi(metrics.dpi);
    canvas.begin();

    const D2D1_ROUNDED_RECT body = ui::gfx::rounded_rect(body_rect, static_cast<float>(radius));

    if (palette.draw_fill) {
        canvas.fill_rounded(body, palette.fill);
    }
    if (palette.draw_stroke) {
        canvas.stroke_rounded(body, palette.stroke, static_cast<float>(border_w));
    }

    if (is_checkbox) {
        const float side = static_cast<float>(box.right - box.left);
        const D2D1_ROUNDED_RECT box_rr = ui::gfx::rounded_rect(box, side * 0.25f);
        const float stroke_w = static_cast<float>(border_w);

        if (disabled) {
            canvas.fill_rounded(box_rr, ui::theming::ColorRole::SurfaceAlt);
            canvas.stroke_rounded(box_rr, ui::theming::ColorRole::Border, stroke_w);
        } else if (state.toggled) {
            canvas.fill_rounded(box_rr, ui::theming::ColorRole::Accent);
            canvas.stroke_rounded(box_rr, ui::theming::ColorRole::Accent, stroke_w);

            const float ox = static_cast<float>(box.left);
            const float oy = static_cast<float>(box.top);
            const float glyph_w = (side * 0.10f > 1.5f) ? side * 0.10f : 1.5f;

            D2D1_POINT_2F p0 = {ox + kCheckX0 * side, oy + kCheckY0 * side};
            D2D1_POINT_2F p1 = {ox + kCheckX1 * side, oy + kCheckY1 * side};
            D2D1_POINT_2F p2 = {ox + kCheckX2 * side, oy + kCheckY2 * side};
            canvas.line(p0, p1, ui::theming::ColorRole::TextOnAccent, glyph_w);
            canvas.line(p1, p2, ui::theming::ColorRole::TextOnAccent, glyph_w);
        } else {
            canvas.fill_rounded(box_rr, ui::theming::ColorRole::Surface);
            canvas.stroke_rounded(box_rr, state.hovered ? ui::theming::ColorRole::BorderStrong
                                                        : ui::theming::ColorRole::Border,
                                  stroke_w);
        }
    }

    const std::wstring label = window_text(hwnd);
    if (!label.empty()) {
        IDWriteTextFormat* format =
            ui::gfx::text_format(L"Segoe UI", kLabelFontPt, DWRITE_FONT_WEIGHT_NORMAL);
        if (format != nullptr) {
            canvas.text(label, label_rect, palette.label, format, DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            // DirectWrite said no. DrawTextW onto the same HDC, same rectangle.
            draw_label_gdi(hdc, label, label_rect, palette.label, metrics.dpi);
        }
    }

    if (focused) {
        // The ring sits one scaled pixel inside the client rect so it never clips
        // the window edge, and it is stroked separately from the body so the two
        // never double up.
        //
        // NOTE ON THE DASHED STYLE: a dashed ring would need
        // ID2D1RenderTarget::SetDashStyle, and that method does not exist on
        // ID2D1RenderTarget in this SDK - dash state lives on ID2D1StrokeStyle,
        // and Canvas::stroke_rounded draws solid only. Inventing a stroke style
        // here would mean duplicating the Canvas' brush handling outside its
        // abstraction, so the ring is a solid FocusRing hairline instead.
        const int inset = scale_or_one(metrics, 1);
        RECT ring_rect = client;
        ring_rect.left += inset;
        ring_rect.top += inset;
        ring_rect.right -= inset;
        ring_rect.bottom -= inset;
        if (ring_rect.right > ring_rect.left && ring_rect.bottom > ring_rect.top) {
            const D2D1_ROUNDED_RECT ring =
                ui::gfx::rounded_rect(ring_rect, static_cast<float>(radius) * 0.75f);
            canvas.stroke_rounded(ring, ui::theming::ColorRole::FocusRing,
                                  static_cast<float>(ring_w));
        }
    }

    canvas.end();
}

// ---------------------------------------------------------------------------
// Input bookkeeping
// ---------------------------------------------------------------------------

void arm_mouse_leave(HWND hwnd, State& state) {
    if (state.tracking) {
        return;
    }
    TRACKMOUSEEVENT track;
    ZeroMemory(&track, sizeof(track));
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd;
    if (TrackMouseEvent(&track) != 0) {
        state.tracking = true;
    }
}

// Clears the pressed state and hands the mouse back to the window manager.
void release_press(HWND hwnd, State& state) {
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    if (state.pressed) {
        state.pressed = false;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

// Drops every transient interaction state. Used when the control is disabled or
// cancelled from underneath the user's finger.
void drop_interaction(HWND hwnd, State& state) {
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    if (state.pressed || state.hovered) {
        state.pressed = false;
        state.hovered = false;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

bool is_double_click(const State& state) {
    if (state.last_click_time == 0) {
        return false;
    }
    const DWORD now = static_cast<DWORD>(GetTickCount());
    if (tick_delta(now, state.last_click_time) > static_cast<LONG>(GetDoubleClickTime())) {
        return false;
    }
    POINT here;
    ZeroMemory(&here, sizeof(here));
    if (GetCursorPos(&here) == 0) {
        return false;
    }
    const long dx = static_cast<long>(here.x) - static_cast<long>(state.last_click_pos.x);
    const long dy = static_cast<long>(here.y) - static_cast<long>(state.last_click_pos.y);
    const long reach_x = static_cast<long>(GetSystemMetrics(SM_CXDOUBLECLK)) / 2;
    const long reach_y = static_cast<long>(GetSystemMetrics(SM_CYDOUBLECLK)) / 2;
    // Squared comparison keeps it to one multiply per axis and avoids abs().
    return (dx * dx) <= (reach_x * reach_x) && (dy * dy) <= (reach_y * reach_y);
}

// Fires the notifications in the order a native BS_NOTIFY button uses: the
// second click of a double click reports BN_DBLCLK first, then BN_CLICKED like
// every other click. Either way BN_CLICKED is the last word, which is the
// notification the shell actually acts on.
void report_activation(HWND hwnd, State& state) {
    const int id = GetDlgCtrlID(hwnd);

    if (is_double_click(state)) {
        notify_parent(hwnd, id, static_cast<UINT>(BN_DBLCLK));
    }
    notify_parent(hwnd, id, static_cast<UINT>(BN_CLICKED));

    POINT here;
    ZeroMemory(&here, sizeof(here));
    GetCursorPos(&here);
    state.last_click_time = static_cast<DWORD>(GetTickCount());
    state.last_click_pos = here;
}

// Keyboard activation goes through the same path as the mouse, so Space or
// Enter on a focused control behaves identically to a click on it.
void activate(HWND hwnd, State& state) {
    if (state.kind != ButtonKind::Push) {
        set_toggled_state(hwnd, state, !state.toggled);
    }
    report_activation(hwnd, state);
}

} // namespace

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK button_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    // --- lifetime ---------------------------------------------------------

    case WM_NCCREATE: {
        if (lparam == 0) {
            return FALSE;
        }
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        State* state = new (std::nothrow) State();
        if (state == nullptr) {
            return FALSE;
        }
        if (cs->lpCreateParams != nullptr) {
            const CreateParams* params = static_cast<const CreateParams*>(cs->lpCreateParams);
            state->kind = params->kind;
            state->style = params->style;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    case WM_NCDESTROY: {
        const LONG_PTR raw = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (raw != 0) {
            State* state = reinterpret_cast<State*>(raw);
            state->magic = 0; // Poisoned before release, so a stale copy is loud.
            delete state;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    // --- painting ----------------------------------------------------------

    case WM_ERASEBKGND: {
        // The class has no background brush; WM_PAINT does the erase itself so
        // the D2D pass always has a clean HDC underneath it.
        return 1;
    }

    case WM_PAINT: {
        State* state = state_of(hwnd);
        if (state == nullptr) {
            return 0;
        }
        PAINTSTRUCT ps;
        ZeroMemory(&ps, sizeof(ps));
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc != nullptr) {
            paint(hwnd, *state, hdc);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }

    case WM_SETTEXT: {
        // DefWindowProc stores the new caption; the repaint is what makes
        // SetWindowText relabel the button.
        const LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
        InvalidateRect(hwnd, nullptr, FALSE);
        return result;
    }

    case WM_SETREDRAW: {
        if (wparam != 0) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    case WM_DPICHANGED: {
        // Metrics are read fresh on every paint via metrics_for(), so a DPI change
        // is just a repaint. The parent owns layout, so no rectangle is moved
        // from here.
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    // --- checkbox / toggle state, via the real BM_* values from <windows.h> ---

    case BM_GETCHECK: {
        State* state = state_of(hwnd);
        if (state == nullptr) {
            return 0;
        }
        return state->toggled ? static_cast<LRESULT>(BST_CHECKED)
                              : static_cast<LRESULT>(BST_UNCHECKED);
    }

    case BM_SETCHECK: {
        State* state = state_of(hwnd);
        if (state == nullptr) {
            return 0;
        }
        // Absolute assignment, exactly like the native control: a caller passing
        // BST_CHECKED or BST_UNCHECKED gets precisely that, and an arbitrary
        // non-zero value folds onto checked the way the native control does.
        set_toggled_state(hwnd, *state, wparam != 0);
        return 0;
    }

    // --- mouse -------------------------------------------------------------

    case WM_MOUSEMOVE: {
        State* state = state_of(hwnd);
        if (state == nullptr) {
            break;
        }
        arm_mouse_leave(hwnd, *state);
        if (!state->hovered) {
            state->hovered = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        State* state = state_of(hwnd);
        if (state == nullptr) {
            break;
        }
        state->tracking = false;
        state->hovered = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        State* state = state_of(hwnd);
        if (state == nullptr) {
            break;
        }
        if ((GetWindowLongW(hwnd, GWL_STYLE) & WS_DISABLED) != 0) {
            return 0;
        }
        SetCapture(hwnd);
        SetFocus(hwnd);
        if (!state->pressed) {
            state->pressed = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        State* state = state_of(hwnd);
        if (state == nullptr) {
            break;
        }
        const bool was_pressed = state->pressed;
        const bool inside = point_in_client(hwnd, lparam);
        release_press(hwnd, *state);

        // Fires only when the press started here and the release landed here,
        // which is what makes press-then-drag-away cancel the click.
        if (was_pressed && inside) {
            activate(hwnd, *state);
        }
        return 0;
    }

    case WM_CAPTURECHANGED: {
        State* state = state_of(hwnd);
        if (state != nullptr && state->pressed) {
            state->pressed = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_CANCELMODE: {
        State* state = state_of(hwnd);
        if (state != nullptr) {
            drop_interaction(hwnd, *state);
        }
        return 0;
    }

    // --- focus / enable ----------------------------------------------------

    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ENABLE: {
        State* state = state_of(hwnd);
        if (state != nullptr && wparam == 0) {
            // A control disabled mid-press must not stay latched.
            drop_interaction(hwnd, *state);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    // --- keyboard ----------------------------------------------------------

    case WM_GETDLGCODE: {
        // The virtual-key in wParam is deliberately ignored: this reports the
        // capability of the control as a whole, which is what lets IsDialogMessage
        // walk the tab chain through us on to the next WS_TABSTOP window.
        State* state = state_of(hwnd);
        const ButtonKind kind = (state != nullptr) ? state->kind : ButtonKind::Push;
        if (kind == ButtonKind::Checkbox) {
            return static_cast<LRESULT>(DLGC_WANTCHARS);
        }
        return static_cast<LRESULT>(DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS);
    }

    case WM_KEYDOWN: {
        State* state = state_of(hwnd);
        if (state == nullptr) {
            break;
        }
        if ((GetWindowLongW(hwnd, GWL_STYLE) & WS_DISABLED) != 0) {
            return 0;
        }
        // Auto-repeat must not toggle a sticky control twenty times a second, so
        // a key that is already down is ignored. Bit 30 of lParam is the
        // "previous key state" flag.
        const bool already_down = (lparam & (static_cast<LPARAM>(1) << 30)) != 0;
        const UINT key = static_cast<UINT>(wparam);

        if (key == VK_SPACE) {
            if (!already_down) {
                activate(hwnd, *state);
            }
            return 0; // Consumed: space must not also scroll the parent.
        }
        if (key == VK_RETURN) {
            // Native buttons activate on Enter; a checkbox does not.
            if ((state->kind == ButtonKind::Push || state->kind == ButtonKind::Toggle) &&
                !already_down) {
                activate(hwnd, *state);
            }
            return 0;
        }
        break;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ThemeButton::register_class(HINSTANCE instance) {
    HMODULE module = static_cast<HMODULE>(instance);
    if (module == nullptr) {
        module = GetModuleHandleW(nullptr);
    }
    if (module == nullptr) {
        return false;
    }

    WNDCLASSEXW probe;
    ZeroMemory(&probe, sizeof(probe));
    probe.cbSize = sizeof(probe);
    if (GetClassInfoExW(module, kClassName, &probe) != 0) {
        return true; // Already registered, by us or by an earlier call.
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    // No CS_HREDRAW/CS_VREDRAW: every state change repaints the whole button
    // anyway, so class-level full-surface erases would be pure flicker.
    wc.style = 0;
    wc.lpfnWndProc = &button_proc;
    wc.hInstance = module;
    // No hbrBackground. WM_PAINT redirects the erase to the parent, so a control
    // with no opaque plate never flashes a system-colour rectangle.
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kClassName;

    if (RegisterClassExW(&wc) == 0) {
        // Another window or another thread got there between the probe and here.
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    return true;
}

const wchar_t* ThemeButton::class_name() {
    return kClassName;
}

HWND ThemeButton::create(HWND parent, int id, const std::wstring& label, int x, int y,
                         int width, int height, ButtonKind kind, ButtonStyle style) {
    if (parent == nullptr) {
        return nullptr;
    }

    HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return nullptr;
    }
    if (!register_class(module)) {
        return nullptr;
    }

    DWORD style_bits = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    switch (kind) {
    case ButtonKind::Push:
        style_bits |= BS_PUSHBUTTON | BS_NOTIFY;
        break;
    case ButtonKind::Toggle:
    case ButtonKind::Checkbox:
        // BS_AUTOCHECKBOX | BS_NOTIFY so anything probing the style window, or an
        // IsDialogMessage-based traversal, sees a control with the shape and the
        // notification semantics it expects.
        style_bits |= BS_AUTOCHECKBOX | BS_NOTIFY;
        break;
    }

    CreateParams params;
    params.kind = kind;
    params.style = style;

    // The caption goes in as the window text, so GetWindowTextW, WM_GETTEXT and
    // accessibility tooling all see the same string WM_PAINT draws.
    return CreateWindowExW(0, kClassName, label.c_str(), style_bits, x, y, width, height, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module, &params);
}

bool ThemeButton::get_toggled(HWND button) {
    State* state = state_of(button);
    return (state != nullptr) && state->toggled;
}

void ThemeButton::set_toggled(HWND button, bool toggled) {
    State* state = state_of(button);
    if (state == nullptr) {
        return;
    }
    set_toggled_state(button, *state, toggled);
}

} // namespace ui::controls