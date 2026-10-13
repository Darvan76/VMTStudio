#include "ui/d2d_canvas.h"

// ============================================================================
// ui d2d_canvas - implementation.
//
// See the header for the full architectural note. The short version: this file
// creates an ID2D1DCRenderTarget from an HDC and nothing else. No D3D device,
// no DXGI device, no swap chain, no CreateHwndRenderTarget. The engine's
// shaderapidx9.dll owns the D3D9 device on its child window; the UI paints into
// GDI memory and is immune to whatever that device does.
//
// See the header for the threading rules around the cached factories, formats
// and brush caches: UI thread only.
// ============================================================================

#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <vector>

namespace ui::gfx {
namespace {

using Microsoft::WRL::ComPtr;

// A solid brush is cached per colour for the life of a Canvas. The cap is small
// because Canvas is a per-paint object: this exists to keep a heavy control from
// churning COM allocations within a single frame, not to be a global cache.
constexpr std::size_t kMaxCachedBrushes = 48u;

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
//
// Both are expensive to build, so each is created at most once per process and
// then never retried - D2D1CreateFactory must be called only once per factory
// type for the lifetime of the process, and retrying a failure would risk an
// invalid-com argument. Both are single-threaded-affine objects used from the
// UI thread only; see the threading note in the header.

ID2D1Factory* d2d_factory() noexcept {
    static ComPtr<ID2D1Factory> factory;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        void* raw = nullptr;
        if (SUCCEEDED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                          &raw)) &&
            raw != nullptr) {
            factory.Attach(static_cast<ID2D1Factory*>(raw));
        }
    }
    return factory.Get();
}

IDWriteFactory* dwrite_factory() noexcept {
    static ComPtr<IDWriteFactory> factory;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        IUnknown* raw = nullptr;
        if (SUCCEEDED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                            &raw)) &&
            raw != nullptr) {
            factory.Attach(static_cast<IDWriteFactory*>(raw));
        }
    }
    return factory.Get();
}

// ---------------------------------------------------------------------------
// Small conversions
// ---------------------------------------------------------------------------

// std::wstring::size() is a size_t; DrawText wants a UINT32. Strings this large
// cannot occur here, but the clamp keeps the call well defined rather than
// silently wrapping.
UINT32 to_char_count(const std::wstring& s) noexcept {
    if (s.size() > static_cast<std::size_t>(MAXDWORD)) {
        return static_cast<UINT32>(MAXDWORD);
    }
    return static_cast<UINT32>(s.size());
}

D2D1_RECT_F rect_to_d2d(const RECT& r) noexcept {
    D2D1_RECT_F out;
    out.left = static_cast<float>(r.left);
    out.top = static_cast<float>(r.top);
    out.right = static_cast<float>(r.right);
    out.bottom = static_cast<float>(r.bottom);
    return out;
}

// DrawText takes its alignment from the format, not from a parameter, so the
// only way to honour the per-call align/valign arguments is to write them onto
// the format first. The getters avoid a redundant setter call on the common
// path. Safe because a Canvas is only ever used from the UI thread and the
// value is re-asserted before every draw.
void apply_alignment(IDWriteTextFormat* fmt, DWRITE_TEXT_ALIGNMENT align,
                     DWRITE_PARAGRAPH_ALIGNMENT valign) noexcept {
    if (fmt->GetTextAlignment() != align) {
        fmt->SetTextAlignment(align);
    }
    if (fmt->GetParagraphAlignment() != valign) {
        fmt->SetParagraphAlignment(valign);
    }
}

// ---------------------------------------------------------------------------
// Text format cache
// ---------------------------------------------------------------------------

struct FormatKey {
    std::wstring family;
    float size_pt = 0.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    bool mono = false;
    bool multiline = false;
};

// std::map needs a strict weak ordering, and operator< on float does not give
// one once a NaN is involved. Ordering the raw bit patterns sidesteps that
// entirely, and also groups -0.0f and +0.0f sensibly.
std::uint32_t float_order_key(float v) noexcept {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "float_order_key needs a 32-bit float");
    std::memcpy(&bits, &v, sizeof(bits));
    // Flip everything below a sign bit into a single monotonic ordering.
    return (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
}

struct FormatKeyLess {
    bool operator()(const FormatKey& a, const FormatKey& b) const noexcept {
        if (a.family != b.family) {
            return a.family < b.family;
        }
        const std::uint32_t ka = float_order_key(a.size_pt);
        const std::uint32_t kb = float_order_key(b.size_pt);
        if (ka != kb) {
            return ka < kb;
        }
        if (a.weight != b.weight) {
            return a.weight < b.weight;
        }
        if (a.mono != b.mono) {
            return a.mono < b.mono;
        }
        return a.multiline < b.multiline;
    }
};

using FormatCache = std::map<FormatKey, ComPtr<IDWriteTextFormat>, FormatKeyLess>;

// Candidates are tried in order; the first installed family wins. Pointers into
// `requested` stay valid because it outlives this call.
std::vector<const wchar_t*> fallback_families(const std::wstring& requested, bool mono) {
    std::vector<const wchar_t*> candidates;
    if (!requested.empty()) {
        candidates.push_back(requested.c_str());
    }
    if (mono) {
        candidates.push_back(L"Consolas");
        candidates.push_back(L"Cascadia Mono");
        candidates.push_back(L"Cascadia Code");
        candidates.push_back(L"Courier New");
    }
    // Last resort, and the face guaranteed present on every Windows install
    // this app supports.
    candidates.push_back(L"Segoe UI");
    return candidates;
}

ComPtr<IDWriteTextFormat> build_text_format(IDWriteFactory* factory,
                                            const std::wstring& requested,
                                            float size_pt, DWRITE_FONT_WEIGHT weight,
                                            bool mono, bool multiline) {
    ComPtr<IDWriteFontCollection> system;
    if (FAILED(factory->GetSystemFontCollection(&system)) || !system) {
        return nullptr;
    }

    // DirectWrite stores font size in DIPs, so a fixed 96 dpi basis is what
    // makes `size_pt` mean points. The render target's own DPI then scales the
    // result, which is why one cached format serves every monitor.
    const float size_dip = size_pt * (96.0f / 72.0f);

    const std::vector<const wchar_t*> candidates = fallback_families(requested, mono);
    for (const wchar_t* family : candidates) {
        // A missing family reports S_OK with *exists == FALSE, so that flag is
        // the real test - not the HRESULT.
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(system->FindFamilyName(family, &index, &exists)) || exists == FALSE) {
            continue;
        }

        ComPtr<IDWriteTextFormat> format;
        // A null collection means "the system font collection", which is what
        // the candidate above was just probed against.
        const HRESULT hr = factory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                     DWRITE_FONT_STRETCH_NORMAL, size_dip, L"en-us",
                                                     &format);
        if (FAILED(hr) || !format) {
            continue;
        }

        DWRITE_TRIMMING trimming = {};
        if (multiline) {
            // A wrapped document must not be trimmed at all, or the last line
            // silently disappears.
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_NONE;
            format->SetTrimming(&trimming, nullptr);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        } else {
            // Character-granularity trailing trim is this SDK's spelling of
            // "cut the overflow off at the end".
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            format->SetTrimming(&trimming, nullptr);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            // Matches Canvas::text_raw's defaults, so a caller that never
            // overrides gets what the API advertises.
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return format;
    }

    return nullptr;
}

// Side table backing the GDI fallback: the DirectWrite format handle is opaque
// to GDI, so the parameters it was built from are recorded alongside it.
std::map<IDWriteTextFormat*, FormatKey>& g_font_specs() {
    static std::map<IDWriteTextFormat*, FormatKey> table;
    return table;
}

IDWriteTextFormat* lookup_text_format(IDWriteFactory* factory, const wchar_t* family,
                                      float size_pt, DWRITE_FONT_WEIGHT weight, bool mono,
                                      bool multiline) {
    if (factory == nullptr) {
        return nullptr;
    }

    FormatKey key;
    key.family.assign(family != nullptr ? family : L"");
    // DirectWrite rejects a zero, negative or NaN font size outright. Folding
    // those onto a readable default keeps the call total and keeps the cache key
    // well defined.
    key.size_pt = (size_pt > 0.0f && size_pt < 512.0f) ? size_pt : 12.0f;
    key.weight = weight;
    key.mono = mono;
    key.multiline = multiline;

    // Declared *after* the factory static has already been initialised by the
    // caller, so at shutdown the cache is destroyed first and the formats are
    // released while their factory is still alive.
    static FormatCache cache;

    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second.Get();
    }

    ComPtr<IDWriteTextFormat> format =
        build_text_format(factory, key.family, key.size_pt, key.weight, key.mono, key.multiline);
    if (!format) {
        return nullptr;
    }

    IDWriteTextFormat* raw = format.Get();
    cache.emplace(key, format);
    g_font_specs().emplace(raw, key);
    return raw;
}

// GDI has no notion of an IDWriteTextFormat, so the software backend needs the
// font parameters back out of the handle the controls are holding. The formats
// are process-lifetime cached objects, so this side table lives just as long.
struct GdiFontSpec {
    std::wstring family;
    float size_pt = 12.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    bool mono = false;
    bool multiline = false;
};

std::map<IDWriteTextFormat*, GdiFontSpec>& g_font_spec_table() {
    static std::map<IDWriteTextFormat*, GdiFontSpec> table;
    return table;
}

// One GDI font per (spec, dpi, weight), reused across every paint.
HFONT gdi_font_for(const GdiFontSpec& spec, int dpi) {
    struct CacheKey {
        std::wstring family;
        float size_pt;
        int weight;
        int dpi;
        bool mono;
        bool multiline;

        bool operator<(const CacheKey& other) const {
            if (family != other.family) return family < other.family;
            if (size_pt != other.size_pt) return size_pt < other.size_pt;
            if (weight != other.weight) return weight < other.weight;
            if (dpi != other.dpi) return dpi < other.dpi;
            if (mono != other.mono) return mono < other.mono;
            return multiline < other.multiline;
        }
    };

    const CacheKey key{spec.family, spec.size_pt, static_cast<int>(spec.weight), dpi, spec.mono,
                       spec.multiline};
    static std::map<CacheKey, HFONT> cache;
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(static_cast<int>(spec.size_pt + 0.5f), dpi, 72);
    lf.lfWeight = spec.weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    if (spec.mono) {
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        wcscpy_s(lf.lfFaceName, L"Consolas");
    } else {
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    }

    HFONT font = CreateFontIndirectW(&lf);
    cache.emplace(key, font);
    return font;
}

UINT gdi_text_flags(DWRITE_TEXT_ALIGNMENT h, DWRITE_PARAGRAPH_ALIGNMENT v, bool multiline) {
    UINT flags = multiline ? DT_WORDBREAK : (DT_SINGLELINE | DT_NOPREFIX);

    switch (h) {
    case DWRITE_TEXT_ALIGNMENT_CENTER:
        flags |= DT_CENTER;
        break;
    case DWRITE_TEXT_ALIGNMENT_TRAILING:
        flags |= DT_RIGHT;
        break;
    case DWRITE_TEXT_ALIGNMENT_LEADING:
    default:
        flags |= DT_LEFT;
        break;
    }

    switch (v) {
    case DWRITE_PARAGRAPH_ALIGNMENT_CENTER:
        flags |= DT_VCENTER;
        break;
    case DWRITE_PARAGRAPH_ALIGNMENT_FAR:
        flags |= DT_BOTTOM;
        break;
    case DWRITE_PARAGRAPH_ALIGNMENT_NEAR:
    default:
        flags |= DT_TOP;
        break;
    }
    return flags;
}

} // namespace

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

D2D1_COLOR_F to_d2d(COLORREF c, float alpha) {
    D2D1_COLOR_F out;
    out.r = static_cast<float>(GetRValue(c)) / 255.0f;
    out.g = static_cast<float>(GetGValue(c)) / 255.0f;
    out.b = static_cast<float>(GetBValue(c)) / 255.0f;
    // NaN fails the first comparison and lands on 0.
    float a = 0.0f;
    if (alpha > 0.0f) {
        a = (alpha < 1.0f) ? alpha : 1.0f;
    }
    out.a = a;
    return out;
}

D2D1_ROUNDED_RECT rounded_rect(const RECT& r, float radius_px) {
    D2D1_RECT_F f = rect_to_d2d(r);

    // A NaN radius folds to 0 rather than poisoning the size below.
    float radius = (radius_px > 0.0f) ? radius_px : 0.0f;
    const float half_w = (f.right - f.left) * 0.5f;
    const float half_h = (f.bottom - f.top) * 0.5f;
    const float limit = (half_w < half_h) ? half_w : half_h;
    if (radius > limit) {
        radius = limit;
    }

    D2D1_ROUNDED_RECT out;
    out.rect = f;
    out.radiusX = radius;
    out.radiusY = radius;
    return out;
}

IDWriteTextFormat* text_format(const wchar_t* family, float size_pt, DWRITE_FONT_WEIGHT weight,
                               bool mono) {
    return lookup_text_format(dwrite_factory(), family, size_pt, weight, mono, false);
}

IDWriteTextFormat* text_format_multiline(const wchar_t* family, float size_pt,
                                         DWRITE_FONT_WEIGHT weight, bool mono) {
    return lookup_text_format(dwrite_factory(), family, size_pt, weight, mono, true);
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

struct Canvas::Impl {
    ComPtr<ID2D1DCRenderTarget> target;

    // GDI fallback. Set when no Direct2D render target could be created at all
    // - see Canvas::Canvas - in which case every primitive is issued to the
    // window DC through plain GDI instead. Same public surface either way, so
    // the controls never learn which backend they got.
    HDC dc = nullptr;
    int dpi = kDefaultCanvasDpi;

    // Direct2D has no render-target-level interpolation mode: it is a parameter
    // of ID2D1RenderTarget::DrawBitmap and a property of ID2D1BitmapBrush. This
    // records the preference so bitmap drawing issued through target() has a
    // sensible default; every primitive this wrapper draws is a solid brush and
    // is unaffected by it.
    D2D1_BITMAP_INTERPOLATION_MODE bitmap_interpolation = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;

    bool gdi() const { return target == nullptr && dc != nullptr; }

    // Solid brushes are device-dependent state and belong to the target, so the
    // cache is dropped together with the target.
    std::map<unsigned long, ComPtr<ID2D1SolidColorBrush>> brushes;

    ID2D1SolidColorBrush* brush(COLORREF c) {
        ID2D1RenderTarget* rt = target.Get();
        if (rt == nullptr) {
            return nullptr;
        }

        const unsigned long key = static_cast<unsigned long>(c);
        auto it = brushes.find(key);
        if (it != brushes.end()) {
            return it->second.Get();
        }

        // Alpha-varying callers could otherwise grow this without bound.
        if (brushes.size() >= kMaxCachedBrushes) {
            brushes.clear();
        }

        const D2D1_COLOR_F color = to_d2d(c);
        ComPtr<ID2D1SolidColorBrush> created;
        if (FAILED(rt->CreateSolidColorBrush(color, &created)) || !created) {
            return nullptr;
        }

        ID2D1SolidColorBrush* raw = created.Get();
        brushes.emplace(key, created);
        return raw;
    }

    void discard() {
        brushes.clear();
        target.Reset();
    }
};

Canvas::Canvas(HDC dc) {
    if (dc == nullptr) {
        return;
    }
    ID2D1Factory* factory = d2d_factory();
    if (factory == nullptr) {
        return;
    }

    D2D1_RENDER_TARGET_PROPERTIES props = {};
    props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    // A DC render target rasterises into the HDC's own format, so the "unknown"
    // pair is both correct and required rather than a placeholder.
    props.pixelFormat.format = DXGI_FORMAT_UNKNOWN;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_UNKNOWN;
    // Irrelevant for a DC target's device-independent metrics - the HDC works in
    // device pixels - but set_dpi() may still raise it for text layout.
    props.dpiX = static_cast<float>(kDefaultCanvasDpi);
    props.dpiY = static_cast<float>(kDefaultCanvasDpi);
    props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

    // D2D1_RENDER_TARGET_TYPE_DEFAULT resolves to the hardware device only. It
    // fails with D2DERR_NO_HARDWARE_DEVICE (0x88982F80) wherever no D3D11
    // hardware adapter is exposed to the session: RDP and other remote sessions,
    // VMs with no 3D acceleration, a machine left on the Microsoft Basic
    // Display Adapter, or a session running without a desktop heap for D3D.
    //
    // A UI shell must not simply go blank there, so fall back to the WARP
    // software rasteriser, which is part of Windows itself and always present.
    // For this workload - rounded rectangles, hairlines and text - WARP is
    // imperceptibly faster than nothing at all, which is the alternative.
    ID2D1DCRenderTarget* raw = nullptr;
    HRESULT hr = factory->CreateDCRenderTarget(&props, &raw);
    if (FAILED(hr) || raw == nullptr) {
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        raw = nullptr;
        hr = factory->CreateDCRenderTarget(&props, &raw);
    }
    // The DC is bound after creation, and only now is any drawing state created
    // - entirely against GDI memory, with no device and no swap chain involved.
    // A failed BindDC is treated the same as a failed creation: use GDI.
    if (raw != nullptr && FAILED(raw->BindDC(dc, nullptr))) {
        raw->Release();
        raw = nullptr;
    }
    // Neither DEFAULT nor SOFTWARE succeeds on every machine, and that is not a
    // rare edge case: any session with no usable D3D11 device - RDP, a VM with
    // no 3D acceleration, the Microsoft Basic Display Adapter - answers
    // D2DERR_NO_HARDWARE_DEVICE (0x88982F80) to both, even though WARP itself
    // is present and loadable. A shell that renders nothing on those machines is
    // worse than one that renders plainly, so the canvas falls back to GDI,
    // which has no D3D dependency at all. Every primitive below is implemented
    // twice and the controls cannot tell the difference.
    Impl* impl = new (std::nothrow) Impl();
    if (impl == nullptr) {
        if (raw != nullptr) raw->Release();
        return;
    }
    impl->dc = dc;
    impl->dpi = kDefaultCanvasDpi;

    if (raw != nullptr) {
        impl->target.Attach(raw);
    }

    impl_.reset(impl);
    dpi_ = kDefaultCanvasDpi;
    valid_ = true;
}

Canvas::~Canvas() = default;

bool Canvas::valid() const { return valid_; }

ID2D1RenderTarget* Canvas::target() const {
    if (!impl_) {
        return nullptr;
    }
    return static_cast<ID2D1RenderTarget*>(impl_->target.Get());
}

void Canvas::set_dpi(int dpi) {
    dpi_ = (dpi > 0) ? dpi : kDefaultCanvasDpi;
    if (!impl_) {
        return;
    }
    impl_->dpi = dpi_;
    if (impl_->target) {
        const float value = static_cast<float>(dpi_);
        impl_->target->SetDpi(value, value);
    }
}

void Canvas::begin() {
    if (!valid_ || !impl_) {
        return;
    }
    drew_ = true;
    ID2D1DCRenderTarget* rt = impl_->target.Get();
    if (rt == nullptr) {
        return; // GDI backend: nothing to begin.
    }

    const float value = static_cast<float>(dpi_);
    rt->SetDpi(value, value);
    rt->BeginDraw();
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
}

void Canvas::end() {
    if (!drew_) {
        return;
    }
    drew_ = false;

    if (!valid_ || !impl_) {
        return;
    }
    ID2D1DCRenderTarget* rt = impl_->target.Get();
    if (rt == nullptr) {
        return; // GDI backend: drawing is immediate, nothing to flush.
    }

    const HRESULT hr = rt->EndDraw();
    if (FAILED(hr)) {
        // D2DERR_RECREATE_TARGET is the documented "your device is gone" result.
        // A DC target has no device to recreate - the HDC owns the pixels - so
        // there is nothing to retry against. The target and its device-dependent
        // brushes are discarded and the caller is expected to build a fresh
        // Canvas on the next paint. Any other failure is treated the same way,
        // because the target is equally unusable afterwards. The DC is kept, so
        // the next paint simply continues on the GDI backend.
        impl_->discard();
    }
}

void Canvas::set_antialiased(bool enabled) {
    (void)enabled; // GDI shapes are not user-antialiasable and text AA is a system setting.
    if (!valid_ || !impl_ || !impl_->target) {
        return;
    }
    impl_->target->SetAntialiasMode(enabled ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
                                            : D2D1_ANTIALIAS_MODE_ALIASED);
    impl_->target->SetTextAntialiasMode(enabled ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE
                                                : D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
}

void Canvas::set_interpolation(bool linear) {
    if (!impl_) {
        return;
    }
    impl_->bitmap_interpolation = linear ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                                         : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
}

// --- GDI backend helpers ----------------------------------------------------

// Every GDI primitive below follows the same shape: select the objects it
// needs, draw, restore. Selection is cached in the canvas for the duration of
// one paint because the controls call these in loops (a combo draws one row per
// item), and selecting a brush is not free.
namespace gdi {

HBRUSH brush_for(COLORREF c) {
    return CreateSolidBrush(c);
}

HPEN pen_for(COLORREF c, int width_px) {
    return CreatePen(PS_SOLID, (width_px > 0) ? width_px : 1, c);
}

int radius_to_diameter(float radius) {
    if (!(radius > 0.0f)) {
        return 0;
    }
    if (radius > 4096.0f) {
        return 8192;
    }
    return static_cast<int>(radius * 2.0f);
}

RECT to_gdi(const D2D1_ROUNDED_RECT& r) {
    RECT out;
    out.left = static_cast<LONG>(r.rect.left);
    out.top = static_cast<LONG>(r.rect.top);
    out.right = static_cast<LONG>(r.rect.right);
    out.bottom = static_cast<LONG>(r.rect.bottom);
    return out;
}

} // namespace gdi

void Canvas::clear_raw(COLORREF color) {
    if (!valid_ || !impl_) {
        return;
    }
    if (!impl_->target) {
        // GetClipBox is the honest answer to "how much of this DC am I allowed
        // to touch": the update region for a window DC, the whole surface for a
        // memory DC.
        RECT extent{};
        if (::GetClipBox(impl_->dc, &extent) == ERROR) {
            return;
        }
        HBRUSH brush = gdi::brush_for(color);
        if (brush != nullptr) {
            const HBRUSH old = static_cast<HBRUSH>(SelectObject(impl_->dc, brush));
            ::FillRect(impl_->dc, &extent, static_cast<HBRUSH>(GetStockObject(NULL_BRUSH)));
            ::SelectObject(impl_->dc, old);
            DeleteObject(brush);
        }
        return;
    }
    const D2D1_COLOR_F value = to_d2d(color);
    impl_->target->Clear(&value);
}

void Canvas::fill_rounded_raw(const D2D1_ROUNDED_RECT& r, COLORREF c) {
    if (!valid_ || !impl_) {
        return;
    }
    if (!impl_->target) {
        HBRUSH brush = gdi::brush_for(c);
        HPEN pen = CreatePen(PS_NULL, 0, 0);
        if (brush == nullptr || pen == nullptr) {
            if (brush) DeleteObject(brush);
            if (pen) DeleteObject(pen);
            return;
        }
        const HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(impl_->dc, brush));
        const HPEN old_pen = static_cast<HPEN>(SelectObject(impl_->dc, pen));
        const RECT rect = gdi::to_gdi(r);
        ::RoundRect(impl_->dc, rect.left, rect.top, rect.right, rect.bottom,
                    gdi::radius_to_diameter(r.radiusX), gdi::radius_to_diameter(r.radiusY));
        ::SelectObject(impl_->dc, old_pen);
        ::SelectObject(impl_->dc, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
        return;
    }
    ID2D1SolidColorBrush* brush = impl_->brush(c);
    if (brush == nullptr) {
        return;
    }
    impl_->target->FillRoundedRectangle(&r, brush);
}

void Canvas::fill_rounded(const D2D1_ROUNDED_RECT& r, ui::theming::ColorRole role) {
    fill_rounded_raw(r, ui::theming::color(role));
}

void Canvas::stroke_rounded_raw(const D2D1_ROUNDED_RECT& r, COLORREF c, float width_px) {
    if (!valid_ || !impl_) {
        return;
    }
    // NaN folds to the 1.0f default rather than reaching Direct2D.
    const int width = (width_px > 0.0f) ? static_cast<int>(width_px + 0.5f) : 1;
    if (!impl_->target) {
        HPEN pen = gdi::pen_for(c, width);
        if (pen == nullptr) {
            return;
        }
        const HBRUSH old_brush =
            static_cast<HBRUSH>(SelectObject(impl_->dc, GetStockObject(NULL_BRUSH)));
        const HPEN old_pen = static_cast<HPEN>(SelectObject(impl_->dc, pen));
        const RECT rect = gdi::to_gdi(r);
        ::RoundRect(impl_->dc, rect.left, rect.top, rect.right, rect.bottom,
                    gdi::radius_to_diameter(r.radiusX), gdi::radius_to_diameter(r.radiusY));
        ::SelectObject(impl_->dc, old_pen);
        ::SelectObject(impl_->dc, old_brush);
        DeleteObject(pen);
        return;
    }
    ID2D1SolidColorBrush* brush = impl_->brush(c);
    if (brush == nullptr) {
        return;
    }
    const float d2d_width = (width_px > 0.0f) ? width_px : 1.0f;
    impl_->target->DrawRoundedRectangle(&r, brush, d2d_width);
}

void Canvas::stroke_rounded(const D2D1_ROUNDED_RECT& r, ui::theming::ColorRole role,
                            float width_px) {
    stroke_rounded_raw(r, ui::theming::color(role), width_px);
}

void Canvas::line(D2D1_POINT_2F a, D2D1_POINT_2F b, ui::theming::ColorRole role,
                  float width_px) {
    if (!valid_ || !impl_) {
        return;
    }
    const COLORREF c = ui::theming::color(role);
    const int width = (width_px > 0.0f) ? static_cast<int>(width_px + 0.5f) : 1;
    if (!impl_->target) {
        HPEN pen = gdi::pen_for(c, width);
        if (pen == nullptr) {
            return;
        }
        const HPEN old = static_cast<HPEN>(SelectObject(impl_->dc, pen));
        ::MoveToEx(impl_->dc, static_cast<int>(a.x), static_cast<int>(a.y), nullptr);
        ::LineTo(impl_->dc, static_cast<int>(b.x), static_cast<int>(b.y));
        ::SelectObject(impl_->dc, old);
        DeleteObject(pen);
        return;
    }
    ID2D1SolidColorBrush* brush = impl_->brush(c);
    if (brush == nullptr) {
        return;
    }
    // NaN folds to the 1.0f default rather than reaching Direct2D.
    const float d2d_width = (width_px > 0.0f) ? width_px : 1.0f;
    impl_->target->DrawLine(a, b, brush, d2d_width);
}

// Shared by both backends: resolves the GDI font for a DirectWrite format
// handle, or nullptr when the handle is unknown (in which case the caller
// still draws, using a default UI face).
bool gdi_draw_text(HDC dc, int dpi, const std::wstring& s, RECT layout, COLORREF c,
                   IDWriteTextFormat* fmt, DWRITE_TEXT_ALIGNMENT h, DWRITE_PARAGRAPH_ALIGNMENT v,
                   bool multiline) {
    if (dc == nullptr) {
        return false;
    }

    const int saved_bk = ::SetBkMode(dc, TRANSPARENT);
    const COLORREF saved_color = ::SetTextColor(dc, c);

    HFONT font = nullptr;
    auto spec_it = g_font_spec_table().find(fmt);
    if (spec_it != g_font_spec_table().end()) {
        font = gdi_font_for(spec_it->second, dpi);
    }
    const HGDIOBJ old_font = font != nullptr ? ::SelectObject(dc, font) : nullptr;

    RECT box = layout;
    ::DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &box,
                gdi_text_flags(h, v, multiline) | DT_END_ELLIPSIS);

    if (old_font != nullptr) {
        ::SelectObject(dc, old_font);
    }
    ::SetTextColor(dc, saved_color);
    ::SetBkMode(dc, saved_bk);
    return true;
}

void Canvas::text_raw(const std::wstring& s, RECT layout, COLORREF c, IDWriteTextFormat* fmt,
                      DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign) {
    if (!valid_ || !impl_ || fmt == nullptr || s.empty()) {
        return;
    }

    if (!impl_->target) {
        // The GDI path never touched the DirectWrite format, so there is no
        // alignment to write back onto it.
        gdi_draw_text(impl_->dc, impl_->dpi, s, layout, c, fmt, align, valign, false);
        return;
    }

    ID2D1SolidColorBrush* brush = impl_->brush(c);
    if (brush == nullptr) {
        return;
    }

    apply_alignment(fmt, align, valign);
    const D2D1_RECT_F rect = rect_to_d2d(layout);
    impl_->target->DrawText(s.c_str(), to_char_count(s), fmt, &rect, brush,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void Canvas::text(const std::wstring& s, RECT layout, ui::theming::ColorRole role,
                  IDWriteTextFormat* fmt, DWRITE_TEXT_ALIGNMENT align,
                  DWRITE_PARAGRAPH_ALIGNMENT valign) {
    text_raw(s, layout, ui::theming::color(role), fmt, align, valign);
}

void Canvas::text_block_raw(const std::wstring& s, RECT layout, COLORREF c,
                            IDWriteTextFormat* fmt) {
    if (!valid_ || !impl_ || fmt == nullptr || s.empty()) {
        return;
    }

    if (!impl_->target) {
        gdi_draw_text(impl_->dc, impl_->dpi, s, layout, c, fmt, DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
        return;
    }

    ID2D1SolidColorBrush* brush = impl_->brush(c);
    if (brush == nullptr) {
        return;
    }

    // Alignment and wrapping come from the multiline format itself, so nothing
    // is written onto the format here.
    const D2D1_RECT_F rect = rect_to_d2d(layout);
    impl_->target->DrawText(s.c_str(), to_char_count(s), fmt, &rect, brush,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

} // namespace ui::gfx