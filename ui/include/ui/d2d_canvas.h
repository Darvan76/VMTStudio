#pragma once

// ui d2d_canvas - a small Direct2D + DirectWrite drawing wrapper for the
// Win32 shell's custom controls.
//
// ============================================================================
// ARCHITECTURAL CONSTRAINT - READ THIS BEFORE ADDING ANY DEVICE-BASED DRAWING
// ============================================================================
//
// This application loads the Source engine's `shaderapidx9.dll`, which creates
// and owns a **D3D9 device** on a child HWND inside this very process. The
// project has previously hit faults during device resets, so the UI layer must
// never contend with the engine for the graphics stack.
//
// Consequently this wrapper deliberately creates **no graphics device of any
// kind**:
//
//   * no D3D9 / D3D10 / D3D11 / D3D12 device,
//   * no DXGI device, no swap chain, no D2D1RenderTarget over a DXGI surface,
//   * no ID2D1Factory::CreateHwndRenderTarget,
//   * no swap chain hanging off a single-threaded D2D1Factory.
//
// The one and only D2D object it creates is an **ID2D1DCRenderTarget**, built
// from the HDC passed in inside WM_PAINT. A DC render target rasterises
// directly into GDI-compatible memory: it allocates no device, no swap chain
// and no driver-visible resource, and it keeps the UI layer completely
// independent of the engine's graphics state. The engine can reset, lose or
// tear down its device and painting carries on regardless.
//
// (d2d1.h transitively includes dxgi.h, because D2D1_PIXEL_FORMAT is defined in
// terms of DXGI_FORMAT. That is a header dependency only - this file never
// creates a DXGI object, never calls D3DCreateDevice/D3D11CreateDevice, and
// never references DXGI's device or swap chain interfaces.)
//
// DirectWrite is used solely for text measurement and glyph rasterisation. It
// is a font stack, not a renderer, and shares no state with D3D9.
//
// ============================================================================
// THREADING
// ============================================================================
// The ID2D1Factory is created as D2D1_FACTORY_TYPE_SINGLE_THREADED and the
// IDWriteFactory as DWRITE_FACTORY_TYPE_SHARED, both in function-local
// statics. They are single-threaded-affine objects and are expected to be
// created and used from the UI thread only, which matches this application: a
// single UI thread pumping the message loop. The cached text formats and the
// per-canvas brush cache follow the same rule.
//
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

#include <d2d1.h>
#include <dwrite.h>

#include <memory>
#include <string>

#include <ui/win32_theme.h>

namespace ui::gfx {

// The DPI a Canvas assumes until the owner calls set_dpi(). Irrelevant for the
// target's device-independent metrics (a DC target always rasterises in device
// pixels), but it decides how DirectWrite interprets point-sized text.
constexpr int kDefaultCanvasDpi = 96;

// A scoped drawing session over an ID2D1DCRenderTarget bound to an HDC.
//
// Typical use inside WM_PAINT:
//
//   ui::gfx::Canvas canvas(hdc);
//   if (canvas.valid()) {
//       canvas.set_dpi(g_dpi);
//       canvas.begin();
//       canvas.clear_raw(ui::theming::color(ui::theming::ColorRole::Surface));
//       canvas.fill_rounded(rr, ui::theming::ColorRole::Accent);
//       canvas.text(L"Hello", rc, ui::theming::ColorRole::Text, fmt);
//       canvas.end();
//   }
//
// The constructor never throws. Every helper is a silent no-op when the canvas
// is invalid, so callers only need the one guard above.
class Canvas {
public:
    explicit Canvas(HDC dc);
    ~Canvas();

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // False when the DC was null, the D2D factory is unavailable, or the DC
    // render target could not be created. Also becomes false if EndDraw
    // reports a lost target, which for a DC target means "discard and build a
    // fresh Canvas next paint" - there is nothing to recreate against, because
    // the HDC owns the pixels.
    bool valid() const;

    // The underlying target, or nullptr when invalid. Exposed so callers can
    // reach D2D calls this wrapper does not model (PushAxisAlignedClip,
    // DrawGeometry, bitmap brushes, ...).
    ID2D1RenderTarget* target() const;

    void begin(); // BeginDraw, AA -> per-primitive, text AA -> ClearType
    void end();   // EndDraw; discards the target on a lost-target result

    // Raster modes. Call after begin(); the target keeps them until changed.
    // set_antialiased() is real: it drives ID2D1RenderTarget::SetAntialiasMode
    // and SetTextAntialiasMode. set_interpolation() only records a preference -
    // Direct2D has no render-target-level interpolation mode, it is a parameter
    // of DrawBitmap and a property of ID2D1BitmapBrush, neither of which this
    // wrapper draws with. Every primitive here is a solid brush and is
    // unaffected.
    void set_antialiased(bool enabled);
    void set_interpolation(bool linear);

    // Render-target DPI used for text layout. Custom controls must call this
    // with ui::theming::metrics_for(hwnd).dpi; the default is
    // kDefaultCanvasDpi. Safe to call before or during a session.
    void set_dpi(int dpi);

    void clear_raw(COLORREF color);

    void fill_rounded_raw(const D2D1_ROUNDED_RECT& r, COLORREF c);
    void fill_rounded(const D2D1_ROUNDED_RECT& r, ui::theming::ColorRole role);

    void stroke_rounded_raw(const D2D1_ROUNDED_RECT& r, COLORREF c, float width_px = 1.0f);
    void stroke_rounded(const D2D1_ROUNDED_RECT& r, ui::theming::ColorRole role,
                        float width_px = 1.0f);

    void line(D2D1_POINT_2F a, D2D1_POINT_2F b, ui::theming::ColorRole role,
              float width_px = 1.0f);

    // Single-line text. `layout` is a pixel RECT; the format decides where in
    // it the text lands, and `align` / `valign` are applied to that format
    // immediately before drawing (see the note on the cached formats below).
    // The text is clipped to `layout` and never wraps.
    void text_raw(const std::wstring& s, RECT layout, COLORREF c, IDWriteTextFormat* fmt,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    void text(const std::wstring& s, RECT layout, ui::theming::ColorRole role,
              IDWriteTextFormat* fmt,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Multi-line text for the raw VMT source view: word wrapped, leading
    // aligned, clipped to `layout`. The format must be the one returned by
    // text_format_multiline().
    void text_block_raw(const std::wstring& s, RECT layout, COLORREF c,
                        IDWriteTextFormat* fmt);

private:
    struct Impl; // pimpl: keeps the D2D/DWrite types out of control code
    std::unique_ptr<Impl> impl_;
    bool valid_ = false;
    bool drew_ = false;
    int dpi_ = kDefaultCanvasDpi;
};

// ---------------------------------------------------------------------------
// Text formats
// ---------------------------------------------------------------------------
//
// Formats are cached process-wide on (family, size_pt, weight, mono,
// multiline) and are safe to ask for on every paint; the lookup is a map probe
// and nothing is created after the first request for a given key. Returns
// nullptr on failure, so callers must null-check.
//
// If the requested family is not installed the lookup falls back rather than
// failing: the requested family, then the usual monospace faces when `mono` is
// set, and finally "Segoe UI". Only if that also fails does it return nullptr.
//
// NOTE: a cached format is shared, and Canvas::text_raw writes the requested
// alignment onto it just before DrawTextW. That is safe here because drawing is
// UI-thread only and the value is re-applied on every draw, but it means these
// formats must not be shared across threads.
//
// ---------------------------------------------------------------------------

// Single line: trailing trim, no word wrap, leading/centre alignment.
IDWriteTextFormat* text_format(const wchar_t* family, float size_pt,
                               DWRITE_FONT_WEIGHT weight, bool mono = false);

// Multi-line: no trimming, word wrap, leading/near alignment.
IDWriteTextFormat* text_format_multiline(const wchar_t* family, float size_pt,
                                         DWRITE_FONT_WEIGHT weight, bool mono = false);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// RECT + corner radius -> D2D1_ROUNDED_RECT. The radius is clamped to half of
// the shorter side so a radius larger than the box cannot produce a negative
// D2D1_SIZE_F (which Direct2D rejects).
D2D1_ROUNDED_RECT rounded_rect(const RECT& r, float radius_px);

// Converts COLORREF (0x00BBGGRR) to a D2D1_COLOR_F, preserving the alpha
// argument (clamped to [0, 1]; NaN becomes 0).
D2D1_COLOR_F to_d2d(COLORREF c, float alpha = 1.0f);

} // namespace ui::gfx