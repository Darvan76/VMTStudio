// ============================================================================
// ui theme_slider - implementation of the themed drop-in trackbar replacement.
//
// RENDERING CONSTRAINT (read before adding anything here):
// This application loads the Source engine's shaderapidx9.dll, which creates and
// owns a D3D9 device inside this process. The UI layer must never contend with
// the engine for the graphics stack, so this control creates NO D3D/DXGI DEVICE
// AND NO SWAPCHAIN. It never calls D3DCreateDevice / D3D11CreateDevice, never
// touches IDXGIFactory or IDXGISwapChain, and never creates an
// ID2D1RenderTarget over a DXGI surface. Painting goes through ui::gfx::Canvas,
// whose only D2D object is an ID2D1DCRenderTarget bound to the HDC handed over
// by BeginPaint - a DC target allocates no device and no driver-visible
// resource, so painting survives whatever the engine does to its own device.
//
// The only reason this file includes <ui/d2d_canvas.h> is that header's
// transitive d2d1.h -> dxgi.h dependency; no DXGI object is ever touched.
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

#include <ui/theme_slider.h>

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

#include <ui/d2d_canvas.h>
#include <ui/win32_theme.h>

namespace ui::controls {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr wchar_t kClassName[] = L"VMTPreviewer.ThemeSlider";

// Defaults chosen to match a freshly created TRACKBAR_CLASSW exactly, because
// the shell sets its ranges with TBM_SETRANGEMIN/TBM_SETRANGEMAX using
// wParam == FALSE ("add to the current value"). With these defaults the first
// relative set lands where the real trackbar put it, so replacing the class is
// a true no-op for the caller.
constexpr int kDefaultMinimum = 0;
constexpr int kDefaultMaximum = 100;
constexpr int kDefaultPageSize = 10;

constexpr int kDesignMinThumbHeight = 12; // in 96dpi design units
constexpr int kFallbackDpi = 96;

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

// Everything needed to map between pixels and values. Recomputed whenever the
// client size or the DPI changes, and cached on the window state in between.
struct Layout {
    RECT client{};
    RECT track{};
    int centre_y = 0;    // vertical centre of both track and thumb
    int thumb_size = 0;
    int left = 0;        // smallest allowed thumb centre x
    int right = 0;       // largest allowed thumb centre x
    int travel = 0;      // right - left, never negative
};

// ---------------------------------------------------------------------------
// Window state (GWLP_USERDATA)
// ---------------------------------------------------------------------------

struct SliderState {
    int minimum = kDefaultMinimum;
    int maximum = kDefaultMaximum;
    int position = kDefaultMinimum;
    int page_size = kDefaultPageSize;

    bool dragging = false;
    bool hovered = false;
    bool focused = false;
    bool enabled = true;
    bool notify_on_set = false;

    // Cached layout plus the inputs it was derived from.
    Layout layout{};
    bool layout_valid = false;
    int layout_width = 0;
    int layout_height = 0;
    int layout_dpi = kFallbackDpi;
};

SliderState* state_of(HWND hwnd) noexcept {
    if (hwnd == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<SliderState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// ---------------------------------------------------------------------------
// Range maths
// ---------------------------------------------------------------------------

// The ordered view of [minimum, maximum]. The stored pair is reported verbatim
// by TBM_GETRANGEMIN/MAX, because that is what the real trackbar does and the
// shell relies on it. For every calculation we use the *ordered* pair instead,
// which turns an inverted range (min > max, which the shell does produce for
// degenerate parameter ranges) into a valid ascending one rather than a
// negative span. A degenerate range yields span == 0, never a division by zero.
struct Range {
    long long lo = 0;
    long long hi = 0;
    long long span = 0;
};

Range effective_range(const SliderState* s) noexcept {
    Range r;
    r.lo = static_cast<long long>(s->minimum);
    r.hi = static_cast<long long>(s->maximum);
    if (r.lo > r.hi) {
        std::swap(r.lo, r.hi);
    }
    // Computed in 64 bits: int limits would overflow on a full-width range.
    r.span = r.hi - r.lo;
    return r;
}

int clamp_to_range(const SliderState* s, long long value) noexcept {
    const Range r = effective_range(s);
    if (value < r.lo) {
        value = r.lo;
    }
    if (value > r.hi) {
        value = r.hi;
    }
    return static_cast<int>(value);
}

// The stored range endpoints are plain ints, but TBM_SETRANGEMIN/MAX arrive as
// a DWORD through lParam and the FALSE ("relative") form adds to the current
// endpoint, so both paths need saturation instead of wrapping.
int saturate_to_int(long long value) noexcept {
    constexpr long long kMin = static_cast<long long>(std::numeric_limits<int>::min());
    constexpr long long kMax = static_cast<long long>(std::numeric_limits<int>::max());
    if (value < kMin) {
        return std::numeric_limits<int>::min();
    }
    if (value > kMax) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(value);
}

int saturate_add(int base, long long delta) noexcept {
    return saturate_to_int(static_cast<long long>(base) + delta);
}

// Position -> [0, 1]. Pinned to 0 when the span is zero.
double normalized(const SliderState* s) noexcept {
    const Range r = effective_range(s);
    if (r.span == 0) {
        return 0.0;
    }
    const long long delta = static_cast<long long>(s->position) - r.lo;
    double t = static_cast<double>(delta) / static_cast<double>(r.span);
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Pixel <-> thumb mapping
// ---------------------------------------------------------------------------

Layout compute_layout(const RECT& client, const ui::theming::Metrics& m) noexcept {
    Layout l;
    l.client = client;

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return l; // Nothing visible; every draw below is guarded anyway.
    }

    int track_height = m.scale(m.border_width) * 2;
    if (track_height < 2) {
        track_height = 2;
    }
    if (track_height > height) {
        track_height = height;
    }

    int thumb = m.scale(m.control_height);
    const int min_thumb = m.scale(kDesignMinThumbHeight);
    if (thumb < min_thumb) {
        thumb = min_thumb;
    }
    // The thumb may never overflow the client rect, so it is capped by whichever
    // of the two client dimensions is smaller.
    if (thumb > height) {
        thumb = height;
    }
    if (thumb > width) {
        thumb = width;
    }

    l.thumb_size = thumb;
    l.centre_y = client.top + height / 2;

    l.track.left = client.left;
    l.track.right = client.right;
    l.track.top = l.centre_y - track_height / 2;
    l.track.bottom = l.track.top + track_height;

    // thumb_x = padding + t * (usable_width - thumb_width), with the padding
    // being exactly half a thumb so the circle sits inside the client rect at
    // both ends of the travel.
    const int half = thumb / 2;
    const int lo = client.left + half;
    int hi = client.right - (thumb - half);
    if (hi < lo) {
        // Only reachable if the client shrank without a WM_SIZE reaching us;
        // collapse the travel onto the centre rather than allowing a negative
        // one.
        const int mid = client.left + std::min(width, thumb) / 2;
        l.left = mid;
        l.right = mid;
    } else {
        l.left = lo;
        l.right = hi;
    }
    l.travel = l.right - l.left;
    return l;
}

const Layout& layout_for(HWND hwnd, SliderState* s, const ui::theming::Metrics& m) noexcept {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    if (s->layout_valid && s->layout_width == width && s->layout_height == height &&
        s->layout_dpi == m.dpi) {
        return s->layout;
    }
    s->layout = compute_layout(client, m);
    s->layout_width = width;
    s->layout_height = height;
    s->layout_dpi = m.dpi;
    s->layout_valid = true;
    return s->layout;
}

void invalidate_layout(SliderState* s) noexcept {
    if (s != nullptr) {
        s->layout_valid = false;
    }
}

// Thumb centre for a normalized position, clamped into the travel.
int thumb_centre_x(const Layout& l, double t) noexcept {
    if (l.travel <= 0) {
        return l.left;
    }
    const double span = static_cast<double>(l.travel);
    double offset = t * span;
    if (offset < 0.0) {
        offset = 0.0;
    } else if (offset > span) {
        offset = span;
    }
    return l.left + static_cast<int>(std::llround(offset));
}

// Inverse mapping: client x -> [0, 1]. Pinned to 0 when there is no travel.
double thumb_fraction(const Layout& l, int x) noexcept {
    if (l.travel <= 0) {
        return 0.0;
    }
    long long offset = static_cast<long long>(x) - static_cast<long long>(l.left);
    if (offset < 0) {
        offset = 0;
    } else if (offset > l.travel) {
        offset = l.travel;
    }
    return static_cast<double>(offset) / static_cast<double>(l.travel);
}

int position_from_x(const SliderState* s, const Layout& l, int x) noexcept {
    const double t = thumb_fraction(l, x);
    const Range r = effective_range(s);
    if (r.span == 0) {
        return clamp_to_range(s, r.lo);
    }
    const double value = static_cast<double>(r.lo) + t * static_cast<double>(r.span);
    return clamp_to_range(s, static_cast<long long>(std::llround(value)));
}

RECT thumb_rect_for(const Layout& l, int centre_x) noexcept {
    const int half = l.thumb_size / 2;
    const int rest = l.thumb_size - half;
    RECT r;
    r.left = centre_x - half;
    r.right = centre_x + rest;
    r.top = l.centre_y - half;
    r.bottom = l.centre_y + rest;
    return r;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

// D2D1_ROUNDED_RECT rejects a negative radius, and gfx::rounded_rect clamps
// against half the (possibly negative) shorter side, so every rounded primitive
// is guarded on having a positive extent and gets an explicitly safe radius.
bool drawable(const RECT& r) noexcept {
    return r.right > r.left && r.bottom > r.top;
}

float radius_for(const RECT& r) noexcept {
    if (!drawable(r)) {
        return 0.0f;
    }
    return static_cast<float>(std::min(r.right - r.left, r.bottom - r.top)) * 0.5f;
}

int scaled_border(const ui::theming::Metrics& m) noexcept {
    return std::max(1, m.scale(m.border_width));
}

int scaled_ring(const ui::theming::Metrics& m) noexcept {
    return std::max(1, m.scale(m.focus_ring_width));
}

void draw_slider(ui::gfx::Canvas& canvas, const Layout& l, const ui::theming::Metrics& m,
                 bool enabled, bool hot, bool show_focus, int centre_x) {
    using ui::theming::ColorRole;

    canvas.clear_raw(ui::theming::color(enabled ? ColorRole::Surface : ColorRole::SurfaceAlt));

    // Groove.
    if (drawable(l.track)) {
        canvas.fill_rounded(ui::gfx::rounded_rect(l.track, radius_for(l.track)),
                            ColorRole::SurfaceSunken);
    }

    const RECT thumb = thumb_rect_for(l, centre_x);

    // Filled portion. It runs under the thumb so that no accent peeks out at
    // either extreme; when disabled the fill is skipped entirely.
    RECT filled = l.track;
    filled.right = std::min(thumb.right, l.track.right);
    if (enabled && drawable(filled)) {
        canvas.fill_rounded(ui::gfx::rounded_rect(filled, radius_for(filled)), ColorRole::Accent);
    }

    // Thumb: flat SurfaceAlt with a hairline when idle and focused-free, solid
    // Accent with no border while hovered or dragged, desaturated when the
    // control is disabled.
    if (drawable(thumb)) {
        if (!enabled) {
            canvas.fill_rounded(ui::gfx::rounded_rect(thumb, radius_for(thumb)),
                                ColorRole::BorderStrong);
        } else if (hot) {
            canvas.fill_rounded(ui::gfx::rounded_rect(thumb, radius_for(thumb)), ColorRole::Accent);
        } else {
            canvas.fill_rounded(ui::gfx::rounded_rect(thumb, radius_for(thumb)),
                                ColorRole::SurfaceAlt);
        }

        if (!hot) {
            const int stroke = scaled_border(m);
            RECT outline = thumb;
            InflateRect(&outline, -(stroke / 2), -(stroke / 2));
            if (drawable(outline)) {
                canvas.stroke_rounded(ui::gfx::rounded_rect(outline, radius_for(outline)),
                                      ColorRole::Border, static_cast<float>(stroke));
            }
        }
    }

    // Focus ring, inset by half its width so the stroke stays inside the client.
    if (show_focus && drawable(thumb)) {
        const int ring = scaled_ring(m);
        RECT outline = thumb;
        InflateRect(&outline, -(ring / 2) - 1, -(ring / 2) - 1);
        if (drawable(outline)) {
            canvas.stroke_rounded(ui::gfx::rounded_rect(outline, radius_for(outline)),
                                  ColorRole::FocusRing, static_cast<float>(ring));
        }
    }
}

// Plain GDI fallback, used only when the DC render target could not be created.
// Without it the control would paint nothing at all in that (rare) case.
void paint_fallback(HDC hdc, const Layout& l, bool enabled, int centre_x) {
    using ui::theming::ColorRole;

    const HBRUSH surface =
        CreateSolidBrush(ui::theming::color(enabled ? ColorRole::Surface : ColorRole::SurfaceAlt));
    if (surface != nullptr) {
        FillRect(hdc, &l.client, surface);
        DeleteObject(surface);
    }

    if (!drawable(l.track)) {
        return;
    }

    const HBRUSH groove = CreateSolidBrush(ui::theming::color(ColorRole::SurfaceSunken));
    if (groove != nullptr) {
        FillRect(hdc, &l.track, groove);
        DeleteObject(groove);
    }

    if (!enabled) {
        return;
    }

    const RECT thumb = thumb_rect_for(l, centre_x);

    const HBRUSH accent = CreateSolidBrush(ui::theming::color(ColorRole::Accent));
    if (accent != nullptr) {
        RECT filled = l.track;
        filled.right = std::min(thumb.right, l.track.right);
        if (drawable(filled)) {
            FillRect(hdc, &filled, accent);
        }
        if (drawable(thumb)) {
            ::Ellipse(hdc, thumb.left, thumb.top, thumb.right, thumb.bottom);
        }
        DeleteObject(accent);
    }
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    if (hdc == nullptr) {
        return;
    }

    SliderState* s = state_of(hwnd);
    if (s != nullptr) {
        const ui::theming::Metrics m = ui::theming::metrics_for(hwnd);
        const Layout& l = layout_for(hwnd, s, m);
        const int centre_x = thumb_centre_x(l, normalized(s));
        const bool enabled = s->enabled && IsWindowEnabled(hwnd) != 0;

        ui::gfx::Canvas canvas(hdc);
        if (canvas.valid()) {
            canvas.set_dpi(m.dpi);
            canvas.begin();
            draw_slider(canvas, l, m, enabled, enabled && (s->dragging || s->hovered),
                        enabled && s->focused && !s->dragging, centre_x);
            canvas.end();
        } else {
            paint_fallback(hdc, l, enabled, centre_x);
        }
    }

    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Notification
// ---------------------------------------------------------------------------

// Mirrors what a real trackbar sends: WM_HSCROLL to the parent, the scroll
// request SB_THUMBPOSITION (== SB_THUMBPOSCHANGED, both 4) in the low word, the
// trackbar notification code TB_THUMBPOSITION (also 4) in the high word, and -
// critically - the slider's own HWND in lParam, which is how the parameter panel
// identifies the row that changed before re-querying TBM_GETPOS.
void notify_parent(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    if (parent == nullptr) {
        return;
    }
    const WPARAM wparam = MAKEWPARAM(SB_THUMBPOSITION, TB_THUMBPOSITION);
    SendMessageW(parent, WM_HSCROLL, wparam, reinterpret_cast<LPARAM>(hwnd));
}

void apply_position(HWND hwnd, SliderState* s, long long value) {
    s->position = clamp_to_range(s, value);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

// Returns false when the key is not one this control acts on.
bool keyboard_step(HWND hwnd, SliderState* s, WPARAM key) {
    const int page = (s->page_size > 0) ? s->page_size : 1;
    const long long base = static_cast<long long>(s->position);

    long long next = base;
    bool handled = true;
    switch (key) {
    case VK_LEFT:
    case VK_DOWN:
        next = base - 1;
        break;
    case VK_RIGHT:
    case VK_UP:
        next = base + 1;
        break;
    case VK_PRIOR: // Page Up
        next = base - page;
        break;
    case VK_NEXT: // Page Down
        next = base + page;
        break;
    case VK_HOME:
        next = std::numeric_limits<long long>::min();
        break;
    case VK_END:
        next = std::numeric_limits<long long>::max();
        break;
    default:
        handled = false;
        break;
    }

    if (!handled) {
        return false;
    }
    apply_position(hwnd, s, next);
    notify_parent(hwnd);
    return true;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK slider_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    SliderState* s = state_of(hwnd);

    switch (message) {
    case WM_NCCREATE: {
        auto* created = new (std::nothrow) SliderState();
        if (created == nullptr) {
            return FALSE;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        return TRUE;
    }

    case WM_NCDESTROY: {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete s;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    case WM_ENABLE: {
        if (s != nullptr) {
            s->enabled = (wparam != 0);
            if (!s->enabled) {
                s->dragging = false;
            }
            invalidate_layout(s);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SIZE: {
        invalidate_layout(s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DPICHANGED: {
        invalidate_layout(s);
        // Follow the suggested rect so the control tracks the monitor change
        // instead of keeping its pre-DPI pixel size.
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETFOCUS: {
        if (s != nullptr) {
            s->focused = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_KILLFOCUS: {
        if (s != nullptr) {
            s->focused = false;
            s->dragging = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        // WM_PAINT covers the whole client area, so skip the flicker.
        return 1;

    case WM_GETDLGCODE:
        // Arrows and characters must reach the control instead of being eaten
        // by the parent's dialog-manager navigation.
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_LBUTTONDOWN: {
        if (s == nullptr || IsWindowEnabled(hwnd) == 0) {
            break;
        }
        SetFocus(hwnd);
        SetCapture(hwnd);
        s->dragging = true;
        s->hovered = true;

        const ui::theming::Metrics m = ui::theming::metrics_for(hwnd);
        const Layout& l = layout_for(hwnd, s, m);

        // Standard trackbar behaviour: a press on the thumb begins a drag
        // without moving it, a press anywhere else on the track jumps there.
        const int x = GET_X_LPARAM(lparam);
        if (std::abs(x - thumb_centre_x(l, normalized(s))) > l.thumb_size / 2) {
            apply_position(hwnd, s, position_from_x(s, l, x));
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (s->notify_on_set) {
            notify_parent(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (s == nullptr) {
            break;
        }
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);

        if (s->dragging) {
            if (GetCapture() == hwnd) {
                const ui::theming::Metrics m = ui::theming::metrics_for(hwnd);
                const Layout& l = layout_for(hwnd, s, m);
                apply_position(hwnd, s, position_from_x(s, l, GET_X_LPARAM(lparam)));
                if (s->notify_on_set) {
                    notify_parent(hwnd);
                }
            }
            return 0;
        }

        RECT client{};
        GetClientRect(hwnd, &client);
        const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const bool inside = PtInRect(&client, p) != FALSE;
        if (s->hovered != inside) {
            s->hovered = inside;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (s != nullptr && s->hovered) {
            s->hovered = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (s == nullptr) {
            break;
        }
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        if (s->dragging) {
            const ui::theming::Metrics m = ui::theming::metrics_for(hwnd);
            const Layout& l = layout_for(hwnd, s, m);
            apply_position(hwnd, s, position_from_x(s, l, GET_X_LPARAM(lparam)));
            s->dragging = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (s->notify_on_set) {
                notify_parent(hwnd);
            }
        }
        return 0;
    }

    case WM_CAPTURECHANGED: {
        // Lost the capture mid-drag (another window took it, or the user
        // alt-tabbed): make sure the thumb does not stay stuck highlighted.
        if (s != nullptr && s->dragging) {
            s->dragging = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }

    case WM_KEYDOWN: {
        if (s == nullptr || IsWindowEnabled(hwnd) == 0) {
            break;
        }
        if (keyboard_step(hwnd, s, wparam)) {
            return 0;
        }
        break;
    }

    case WM_PAINT:
        paint(hwnd);
        return 0;

    // -- native trackbar message protocol ----------------------------------

    case TBM_GETRANGEMIN:
        return (s != nullptr) ? static_cast<LRESULT>(s->minimum) : 0;

    case TBM_GETRANGEMAX:
        return (s != nullptr) ? static_cast<LRESULT>(s->maximum) : 0;

    case TBM_GETPOS:
        return (s != nullptr) ? static_cast<LRESULT>(s->position) : 0;

    case TBM_GETPAGESIZE:
        return (s != nullptr) ? static_cast<LRESULT>(s->page_size) : 0;

    case TBM_SETPAGESIZE: {
        if (s == nullptr) {
            break;
        }
        const int previous = s->page_size;
        // Native clamps a zero page size to one rather than freezing the key.
        s->page_size = (static_cast<int>(lparam) > 0) ? static_cast<int>(lparam) : 1;
        return static_cast<LRESULT>(previous);
    }

    case TBM_SETRANGEMIN: {
        if (s == nullptr) {
            break;
        }
        const long long value = static_cast<long long>(static_cast<int>(lparam));
        if (wparam != 0) {
            s->minimum = saturate_to_int(value);
        } else {
            s->minimum = saturate_add(s->minimum, value);
        }
        invalidate_layout(s);
        apply_position(hwnd, s, static_cast<long long>(s->position));
        return 0;
    }

    case TBM_SETRANGEMAX: {
        if (s == nullptr) {
            break;
        }
        const long long value = static_cast<long long>(static_cast<int>(lparam));
        if (wparam != 0) {
            s->maximum = saturate_to_int(value);
        } else {
            s->maximum = saturate_add(s->maximum, value);
        }
        invalidate_layout(s);
        apply_position(hwnd, s, static_cast<long long>(s->position));
        return 0;
    }

    case TBM_SETPOS: {
        if (s == nullptr) {
            break;
        }
        // Sticky, so later mouse interaction honours the same intent.
        s->notify_on_set = (wparam != 0);
        apply_position(hwnd, s, static_cast<long long>(static_cast<int>(lparam)));
        if (s->notify_on_set) {
            notify_parent(hwnd);
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

// ---------------------------------------------------------------------------
// ThemeSlider
// ---------------------------------------------------------------------------

const wchar_t* ThemeSlider::class_name() { return kClassName; }

bool ThemeSlider::register_class(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &slider_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // No background brush: WM_PAINT covers the full client area every time.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;

    if (RegisterClassExW(&wc) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND ThemeSlider::create(HWND parent, int id, int x, int y, int width, int height) {
    if (parent == nullptr) {
        return nullptr;
    }
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        return nullptr;
    }
    register_class(instance);

    return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, width,
                           height, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}

int ThemeSlider::position(HWND slider) {
    SliderState* s = state_of(slider);
    return (s != nullptr) ? s->position : 0;
}

void ThemeSlider::set_range(HWND slider, int minimum, int maximum) {
    SliderState* s = state_of(slider);
    if (s == nullptr) {
        return;
    }
    s->minimum = minimum;
    s->maximum = maximum;
    invalidate_layout(s);
    apply_position(slider, s, static_cast<long long>(s->position));
}

void ThemeSlider::set_position(HWND slider, int value, bool notify) {
    SliderState* s = state_of(slider);
    if (s == nullptr) {
        return;
    }
    s->notify_on_set = notify;
    apply_position(slider, s, static_cast<long long>(value));
    if (notify) {
        notify_parent(slider);
    }
}

void ThemeSlider::set_page_size(HWND slider, int page) {
    SliderState* s = state_of(slider);
    if (s == nullptr) {
        return;
    }
    s->page_size = (page > 0) ? page : 1;
}

} // namespace ui::controls