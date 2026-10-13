// ============================================================================
// ui theme_combo - themed dropdown combo, DC-backed rendering only.
//
// ============================================================================
// ARCHITECTURAL CONSTRAINT - READ THIS BEFORE ADDING ANY DEVICE-BASED DRAWING
// ============================================================================
//
// This application loads the Source engine's `shaderapidx9.dll`, which creates
// and owns a **D3D9 device** on a child HWND inside this very process. The
// project has previously faulted during device resets, so the UI layer must
// never contend with the engine for the graphics stack.
//
// Every pixel this control draws therefore goes through ui::gfx::Canvas, and
// that wrapper's one and only D2D object is an **ID2D1DCRenderTarget** built
// from the HDC handed to it inside WM_PAINT:
//
//   BeginPaint -> ui::gfx::Canvas canvas(hdc) -> BeginDraw / Draw / EndDraw -> EndPaint
//
// What that means, concretely, for this file:
//
//   * NO D3D9 / D3D10 / D3D11 / D3D12 device is created.
//   * NO DXGI device and NO swap chain are created.
//   * NO ID2D1Factory::CreateHwndRenderTarget and no DXGI-surface render
//     target. The only factory this control touches is ui::gfx's, and the only
//     thing it ever produces is an ID2D1DCRenderTarget via CreateDCRenderTarget.
//   * No layered window / UpdateLayeredWindow path either: the popup paints
//     opaque DC-backed content and draws its own rounded border hairline.
//
// (d2d1.h transitively includes dxgi.h because D2D1_PIXEL_FORMAT is defined in
// terms of DXGI_FORMAT. That is a header dependency only. Nothing in this file
// names a DXGI type, a DXGI entry point, or a D3D creation function.)
//
// The shell therefore stays completely independent of the engine's graphics
// state: the engine can reset, lose or tear down its device and this control
// carries on painting regardless.
//
// ============================================================================
// LIFETIME - THE POPUP NEVER OUTLIVES ITS COMBO
// ============================================================================
//
// The popup is a separate HWND, so it can outlive the message that created it,
// and it holds a raw `ComboState*` in its own GWLP_USERDATA. The ordering that
// makes that safe is enforced in exactly one place and relied on everywhere:
//
//   * `destroy_popup_window()` is called from the combo's WM_DESTROY *and*
//     WM_NCDESTROY, and from `close_dropdown()`. DestroyWindow() is
//     synchronous, so the popup's WM_NCDESTROY - the only place its window
//     procedure dereferences the state - always runs to completion while the
//     combo's state is still allocated.
//   * Only after that does the combo's WM_NCDESTROY `delete` the state.
//   * The popup is additionally owned by the combo (hWndParent of a WS_POPUP
//     window is its owner), so Windows destroys it as part of destroying the
//     combo even if the explicit call above were somehow skipped.
//
// Net effect: from the moment the combo frees its state there is no popup that
// could point at it, and no code path uses the combo HWND once WM_DESTROY has
// begun.
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

#include <ui/theme_combo.h>

#include <windowsx.h>

#include <ui/d2d_canvas.h>
#include <ui/win32_theme.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <wchar.h>

namespace ui::controls {
namespace {

using ui::theming::ColorRole;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr wchar_t kComboClassName[] = L"VMT.Previewer.ThemeCombo";
constexpr wchar_t kPopupClassName[] = L"VMT.Previewer.ThemeComboPopup";

// Drawn when nothing is selected.
constexpr wchar_t kPlaceholderText[] = L"Select...";

// A type-ahead prefix expires after this long without a keystroke.
constexpr ULONGLONG kTypeAheadTimeoutMs = 1000;

// One wheel notch scrolls this many rows.
constexpr int kWheelRowsPerNotch = 3;

// Rows shown before the popup starts scrolling.
constexpr int kMaxVisibleRows = 16;

// Hard bound on one item's length in wchar_t, applied at INSERT time.
//
// CB_GETLBTEXT carries no destination size, exactly like the native control, so
// the only way to guarantee it never writes further than a caller that sized
// its buffer from CB_GETLBTEXTLEN is to keep that number truthful. Trimming on
// the way in makes `items[i].size() <= kMaxItemChars` an invariant of the
// container rather than a read-time clamp, so CB_GETLBTEXTLEN and the number
// of wchar_t CB_GETLBTEXT writes can never disagree. 1024 characters is two
// orders of magnitude more than any shader name or enum label.
constexpr size_t kMaxItemChars = 1024;

// Body text. The point size is fixed and the canvas DPI does the scaling, which
// is how ui::gfx::text_format is designed to be used: one cached format serves
// every monitor.
constexpr float kBodyFontSizePt = 10.0f;
constexpr wchar_t kBodyFontFamily[] = L"Segoe UI";

// ---------------------------------------------------------------------------
// Per-control state
// ---------------------------------------------------------------------------

struct ComboState {
    std::vector<std::wstring> items;
    std::vector<INT_PTR> item_data; // CB_SETITEMDATA payload, index-aligned

    int selected = -1;  // committed selection; what CB_GETCURSEL reports
    int highlight = -1; // keyboard / hover row while the popup is open
    int scroll = 0;     // first visible row

    bool dropdown_open = false;
    HWND popup = nullptr;

    // Selection as of the last commit, or the last time the popup opened.
    // Escape reverts to this.
    INT_PTR committed_selection = -1;

    std::wstring typeahead;
    ULONGLONG typeahead_tick = 0;
};

// How a closing popup is being resolved.
enum class CloseMode {
    Discard, // closed without choosing: second click on the control, a click
             // outside the list, focus lost, or the list mutating underneath
    Commit,  // Enter, or a click that landed on a row
    Cancel   // Escape: revert to committed_selection
};

ComboState* state_of(HWND hwnd) {
    return reinterpret_cast<ComboState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

wchar_t fold_case(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

// Case-insensitive prefix test with ASCII folding, which is what the native
// combobox's own type-ahead does. An empty prefix matches everything.
bool has_prefix_ci(const std::wstring& item, const std::wstring& prefix) {
    if (prefix.size() > item.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (fold_case(item[i]) != fold_case(prefix[i])) {
            return false;
        }
    }
    return true;
}

int item_count(const ComboState* s) {
    return (s != nullptr) ? static_cast<int>(s->items.size()) : 0;
}

bool valid_index(const ComboState* s, int index) {
    return index >= 0 && index < item_count(s);
}

// Keeps item_data exactly as long as items, so the two can never drift apart
// across the several mutation paths below.
void align_item_data(ComboState* s) {
    if (s->item_data.size() < s->items.size()) {
        s->item_data.resize(s->items.size(), static_cast<INT_PTR>(CB_ERR));
    } else if (s->item_data.size() > s->items.size()) {
        s->item_data.resize(s->items.size());
    }
}

bool push_item(ComboState* s, const std::wstring& value) noexcept {
    try {
        s->items.push_back(value);
        s->item_data.push_back(static_cast<INT_PTR>(CB_ERR));
        return true;
    } catch (...) {
        return false;
    }
}

bool insert_item(ComboState* s, int index, const std::wstring& value) noexcept {
    try {
        s->items.insert(s->items.begin() + index, value);
        s->item_data.insert(s->item_data.begin() + index, static_cast<INT_PTR>(CB_ERR));
        return true;
    } catch (...) {
        return false;
    }
}

void erase_item(ComboState* s, int index) noexcept {
    s->items.erase(s->items.begin() + index);
    s->item_data.erase(s->item_data.begin() + index);
}

// Copies at most kMaxItemChars characters out of an LPCWSTR the caller owns,
// with no CRT length helper in the way. False only on an allocation failure.
bool read_item_text(LPCWSTR text, std::wstring& out) {
    if (text == nullptr) {
        return false;
    }
    size_t length = 0;
    while (length < kMaxItemChars && text[length] != L'\0') {
        ++length;
    }
    try {
        out.assign(text, length);
    } catch (...) {
        return false;
    }
    return true;
}

void notify(HWND combo, int code) {
    const HWND parent = GetParent(combo);
    if (parent == nullptr) {
        return;
    }
    const int id = GetDlgCtrlID(combo);
    SendMessageW(parent, WM_COMMAND,
                 MAKEWPARAM(static_cast<WORD>(id), static_cast<WORD>(code)),
                 reinterpret_cast<LPARAM>(combo));
}

void invalidate_combo(HWND combo) {
    if (combo != nullptr) {
        InvalidateRect(combo, nullptr, FALSE);
    }
}

void invalidate_popup(const ComboState* s) {
    if (s != nullptr && s->popup != nullptr) {
        InvalidateRect(s->popup, nullptr, FALSE);
    }
}

// Sets the committed selection, refreshes committed_selection and repaints.
// `send_change` is what separates a real, user-visible change (which fires
// CBN_SELCHANGE) from a silent CB_SETCURSEL, which native does not notify for.
void set_selection(HWND combo, ComboState* s, int index, bool send_change) {
    const int clamped = valid_index(s, index) ? index : -1;
    const bool changed = (clamped != s->selected);

    s->selected = clamped;
    if (clamped >= 0) {
        s->committed_selection = clamped;
    }
    invalidate_combo(combo);

    if (send_change && changed) {
        notify(combo, CBN_SELCHANGE);
    }
}

// ---------------------------------------------------------------------------
// Popup geometry
// ---------------------------------------------------------------------------

struct PopupGeom {
    int border = 0;
    int row_h = 0;
    int visible = 1;    // whole rows that fit
    int max_scroll = 0; // rows hidden below the fold
    bool has_scroll = false;
    RECT row_area{};
    RECT track{};
    RECT thumb{};
};

PopupGeom compute_geom(HWND popup, const ComboState* s, const ui::theming::Metrics& m) {
    PopupGeom g;

    RECT client{};
    if (popup != nullptr) {
        GetClientRect(popup, &client);
    }
    // RECT members are LONG; everything below is plain int arithmetic, so the
    // two never meet in a std::max/std::clamp overload set.
    const int w = static_cast<int>(client.right) - static_cast<int>(client.left);
    const int h = static_cast<int>(client.bottom) - static_cast<int>(client.top);
    const int count = item_count(s);

    g.border = std::max(1, m.scale(m.border_width));
    g.row_h = std::max(1, m.scale(m.row_height));
    g.visible = std::max(1, h / g.row_h);
    g.max_scroll = std::max(0, count - g.visible);
    g.has_scroll = (g.max_scroll > 0);

    const int gutter = g.has_scroll ? std::max(1, m.scale(10)) : 0;
    const int rows_left = g.border;
    const int rows_top = g.border;
    const int rows_right = std::max(g.border + 1, w - g.border - gutter);
    const int rows_bottom = std::max(g.border + 1, h - g.border);
    g.row_area = RECT{rows_left, rows_top, rows_right, rows_bottom};

    if (g.has_scroll) {
        const int track_left = std::max(0, w - g.border - gutter);
        const int track_top = g.border;
        const int track_right = std::max(track_left + 1, w - g.border);
        const int track_bottom = std::max(track_top + 1, h - g.border);
        g.track = RECT{track_left, track_top, track_right, track_bottom};

        const int track_h = std::max(1, track_bottom - track_top);
        const int min_thumb = std::min(std::max(1, m.scale(20)), track_h);
        int thumb_h = (track_h * g.visible) / std::max(1, count);
        thumb_h = std::clamp(thumb_h, min_thumb, track_h);

        const int travel = std::max(0, track_h - thumb_h);
        const int offset = std::clamp(s->scroll, 0, g.max_scroll);
        int thumb_top = track_top + (g.max_scroll > 0 ? (travel * offset) / g.max_scroll : 0);
        thumb_top = std::clamp(thumb_top, track_top, std::max(track_top, track_bottom - thumb_h));

        const int thumb_bottom = std::max(thumb_top + 1, std::min(track_bottom, thumb_top + thumb_h));
        g.thumb = RECT{track_left, thumb_top, track_right, thumb_bottom};
    }
    return g;
}

// Row under a popup-client y coordinate, or -1 for "above the first row" /
// "below the last row", which is what drives drag-to-auto-scroll.
int hit_row(const ComboState* s, const PopupGeom& g, int client_y) {
    const int local = client_y - static_cast<int>(g.row_area.top);
    if (local < 0) {
        return -1;
    }
    const int index = s->scroll + (local / g.row_h);
    return valid_index(s, index) ? index : -1;
}

void set_scroll(HWND combo, ComboState* s, int value) {
    if (s == nullptr || s->popup == nullptr) {
        return;
    }
    const PopupGeom g = compute_geom(s->popup, s, ui::theming::metrics_for(combo));
    const int next = std::clamp(value, 0, g.max_scroll);
    if (next != s->scroll) {
        s->scroll = next;
        invalidate_popup(s);
    }
}

// Brings `row` into the visible window without otherwise touching it.
void scroll_into_view(HWND combo, ComboState* s, int row) {
    if (s == nullptr || s->popup == nullptr || !valid_index(s, row)) {
        return;
    }
    const PopupGeom g = compute_geom(s->popup, s, ui::theming::metrics_for(combo));
    if (row < s->scroll) {
        set_scroll(combo, s, row);
    } else if (row >= s->scroll + g.visible) {
        set_scroll(combo, s, row - g.visible + 1);
    }
}

// Keeps highlight and scroll sane after the item list changed shape. Note this
// deliberately does NOT auto-select: only CB_ADDSTRING does that, and only when
// the list was empty, so an explicit CB_SETCURSEL(-1) sticks.
void after_list_change(HWND combo, ComboState* s) {
    const int count = item_count(s);
    if (s->selected >= count) {
        s->selected = count - 1; // -1 once the list is empty
    }
    if (s->highlight >= count) {
        s->highlight = count - 1;
    }
    s->scroll = std::clamp(s->scroll, 0, std::max(0, count - 1));
    invalidate_combo(combo);
    invalidate_popup(s);
}

// ---------------------------------------------------------------------------
// Opening / closing
// ---------------------------------------------------------------------------

void destroy_popup_window(ComboState* s) {
    const HWND popup = s->popup;
    // Clear first. The popup's WM_NCDESTROY - which runs synchronously inside
    // the DestroyWindow below, while `s` is still allocated - then has nothing
    // left to do and cannot resurrect a stale handle.
    s->popup = nullptr;
    s->dropdown_open = false;
    if (popup != nullptr && IsWindow(popup) != FALSE) {
        DestroyWindow(popup);
    }
}

// `notify()` is a SendMessage, so the parent is free to do anything with this
// control from inside a notification - including destroying it. After every
// notification that can happen we re-read the state through the HWND instead
// of trusting the pointer we started with. Returns nullptr once the control is
// gone, which is the signal to stop touching it.
ComboState* revalidate(HWND combo) {
    if (IsWindow(combo) == FALSE) {
        return nullptr;
    }
    return state_of(combo);
}

void close_dropdown(HWND combo, ComboState* s, CloseMode mode) {
    if (s == nullptr || !s->dropdown_open) {
        return;
    }

    switch (mode) {
        case CloseMode::Commit: {
            // Native ordering: SELENDOK, then SELCHANGE (only when the row
            // really changed), then CLOSEUP.
            notify(combo, CBN_SELENDOK);
            s = revalidate(combo);
            if (s == nullptr) {
                return; // the parent destroyed us from CBN_SELENDOK
            }
            if (s->highlight != s->selected) {
                set_selection(combo, s, s->highlight, true);
            }
            break;
        }

        case CloseMode::Cancel:
            notify(combo, CBN_SELENDCANCEL);
            s = revalidate(combo);
            if (s == nullptr) {
                return; // the parent destroyed us from CBN_SELENDCANCEL
            }
            set_selection(combo, s, static_cast<int>(s->committed_selection), true);
            break;

        case CloseMode::Discard:
            break;
    }

    s->dropdown_open = false;
    destroy_popup_window(s); // the popup is gone before CBN_CLOSEUP goes out
    notify(combo, CBN_CLOSEUP);
    if (revalidate(combo) != nullptr) {
        invalidate_combo(combo);
    }
}

bool open_dropdown(HWND combo, ComboState* s) {
    if (s == nullptr || s->dropdown_open || s->items.empty()) {
        return false;
    }

    const ui::theming::Metrics m = ui::theming::metrics_for(combo);
    const int row_h = std::max(1, m.scale(m.row_height));

    RECT wr{};
    if (GetWindowRect(combo, &wr) == FALSE) {
        return false;
    }
    const int width = std::max(1, static_cast<int>(wr.right) - static_cast<int>(wr.left));
    int height = std::min(item_count(s) * row_h, std::max(1, kMaxVisibleRows) * row_h);

    // The monitor work area drives both the flip decision and the final clamp.
    RECT work{};
    HMONITOR monitor = MonitorFromWindow(combo, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &mi) != FALSE) {
        work = mi.rcWork;
    } else if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) == FALSE) {
        work.left = 0;
        work.top = 0;
        work.right = 1920;
        work.bottom = 1080;
    }

    const int work_left = static_cast<int>(work.left);
    const int work_top = static_cast<int>(work.top);
    const int work_right = static_cast<int>(work.right);
    const int work_bottom = static_cast<int>(work.bottom);
    const int wr_left = static_cast<int>(wr.left);
    const int wr_top = static_cast<int>(wr.top);
    const int wr_bottom = static_cast<int>(wr.bottom);

    const int space_below = work_bottom - wr_bottom;
    int x = wr_left;
    int y = 0;
    if (space_below >= height + m.scale(m.gap)) {
        y = wr_bottom; // flush below, like a native dropdown
    } else {
        const int space_above = wr_top - work_top;
        height = std::min(height, std::max(row_h, space_above));
        y = wr_top - height; // not enough room: flip above
    }
    height = std::clamp(height, row_h, std::max(row_h, work_bottom - work_top));
    x = std::clamp(x, work_left, std::max(work_left, work_right - width));

    if (s->popup == nullptr || IsWindow(s->popup) == FALSE) {
        s->popup = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kPopupClassName, nullptr,
                                   WS_POPUP, x, y, width, height,
                                   combo, // owner (hWndParent of a WS_POPUP window)
                                   nullptr, GetModuleHandleW(nullptr), s);
        if (s->popup == nullptr) {
            return false;
        }
    }

    s->dropdown_open = true;
    s->committed_selection = s->selected;
    s->highlight = s->selected;
    s->scroll = std::clamp(s->scroll, 0, std::max(0, item_count(s) - 1));

    SetWindowPos(s->popup, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    invalidate_popup(s);
    invalidate_combo(combo);
    notify(combo, CBN_DROPDOWN);
    return true;
}

// ---------------------------------------------------------------------------
// Type-ahead
// ---------------------------------------------------------------------------

// Next item after `start` whose text begins with `prefix`, wrapping once
// through the list so repeated letters cycle the matches.
int typeahead_search(const ComboState* s, const std::wstring& prefix, int start) {
    const int count = item_count(s);
    if (count == 0 || prefix.empty()) {
        return -1;
    }
    for (int step = 1; step <= count; ++step) {
        int index = start + step;
        if (index >= count) {
            index -= count;
        }
        if (has_prefix_ci(s->items[static_cast<size_t>(index)], prefix)) {
            return index;
        }
    }
    return -1;
}

void apply_typeahead(HWND combo, ComboState* s, wchar_t ch) {
    const ULONGLONG now = GetTickCount64();
    if (now - s->typeahead_tick > kTypeAheadTimeoutMs) {
        s->typeahead.clear();
    }
    s->typeahead_tick = now;

    if (ch >= L'a' && ch <= L'z') {
        s->typeahead.push_back(static_cast<wchar_t>(ch - L'a' + L'A'));
    } else {
        s->typeahead.push_back(ch);
    }
    if (s->typeahead.size() > 32) {
        s->typeahead.erase(0, s->typeahead.size() - 32); // keep the tail
    }

    const int start = (s->dropdown_open && s->highlight >= 0) ? s->highlight : s->selected;
    const int found = typeahead_search(s, s->typeahead, start);
    if (found < 0) {
        return;
    }

    if (s->dropdown_open) {
        s->highlight = found;
        scroll_into_view(combo, s, found);
        invalidate_combo(combo);
        invalidate_popup(s);
    } else {
        set_selection(combo, s, found, true);
    }
}

bool modifier_down(int key) {
    return (GetKeyState(key) & 0x8000) != 0;
}

bool is_printable_char(WPARAM wparam) {
    // Space is excluded: it is the "open the dropdown" key, not a letter. Alt
    // is a menu accelerator and Ctrl a control code; neither is type-ahead.
    return wparam > 0x20 && wparam < 0x7F && !modifier_down(VK_MENU) && !modifier_down(VK_CONTROL);
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

// While the popup is open the arrows move the highlight and scroll the view.
// The selection is not touched until Enter or a click commits it.
bool handle_open_key(HWND combo, ComboState* s, UINT key) {
    switch (key) {
        case VK_UP:
            s->highlight = std::max(0, s->highlight - 1);
            break;
        case VK_DOWN:
            s->highlight = std::min(item_count(s) - 1, s->highlight + 1);
            break;
        case VK_HOME:
            s->highlight = 0;
            break;
        case VK_END:
            s->highlight = item_count(s) - 1;
            break;
        default:
            return false;
    }
    scroll_into_view(combo, s, s->highlight);
    invalidate_popup(s);
    return true;
}

// While closed the arrows move the selection, clamping rather than wrapping
// (native does not wrap), and each real change fires CBN_SELCHANGE.
bool handle_closed_key(HWND combo, ComboState* s, UINT key) {
    const int count = item_count(s);
    if (count == 0) {
        return false;
    }

    int next = s->selected;
    switch (key) {
        case VK_UP:
            next = std::max(0, s->selected - 1);
            break;
        case VK_DOWN:
            next = std::min(count - 1, s->selected + 1);
            break;
        case VK_HOME:
            next = 0;
            break;
        case VK_END:
            next = count - 1;
            break;
        default:
            return false;
    }
    set_selection(combo, s, next, true);
    return true;
}

bool handle_combo_key(HWND combo, ComboState* s, UINT key) {
    if (s == nullptr) {
        return false;
    }

    if (s->dropdown_open) {
        switch (key) {
            case VK_ESCAPE:
                close_dropdown(combo, s, CloseMode::Cancel);
                return true;
            case VK_RETURN:
            case VK_SPACE:
                close_dropdown(combo, s, CloseMode::Commit);
                return true;
            default:
                return handle_open_key(combo, s, key);
        }
    }

    switch (key) {
        case VK_F4:
        case VK_SPACE:
        case VK_RETURN:
            open_dropdown(combo, s);
            return true;
        case VK_DOWN:
            if (modifier_down(VK_MENU)) { // Alt+Down, the classic "open me"
                open_dropdown(combo, s);
            } else {
                handle_closed_key(combo, s, key);
            }
            return true;
        default:
            return handle_closed_key(combo, s, key);
    }
}

// ---------------------------------------------------------------------------
// Painting - GDI fallbacks
// ---------------------------------------------------------------------------

// Used when DirectWrite is unavailable, or returned no format for the
// requested family. Keeps the control legible instead of blank.
void draw_text_gdi(HDC dc, const std::wstring& s, RECT rc, ColorRole role, int dpi) {
    if (dc == nullptr || s.empty()) {
        return;
    }

    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(static_cast<int>(kBodyFontSizePt), (dpi > 0) ? dpi : 96, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, kBodyFontFamily);

    HFONT font = CreateFontIndirectW(&lf);
    const HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::theming::color(role));
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &rc,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
    if (font != nullptr) {
        DeleteObject(font);
    }
}

void fill_rect_gdi(HDC dc, const RECT& rc, ColorRole role) {
    if (dc == nullptr) {
        return;
    }
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    const HGDIOBJ old =
        SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, ui::theming::color(role));
    RECT r = rc;
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SelectObject(dc, old);
}

// ---------------------------------------------------------------------------
// Painting - closed-state combo
// ---------------------------------------------------------------------------

void paint_combo(HWND combo, HDC dc, ComboState* s) {
    RECT client{};
    GetClientRect(combo, &client);
    const ui::theming::Metrics m = ui::theming::metrics_for(combo);
    const bool open = (s != nullptr && s->dropdown_open);

    const int border = std::max(1, m.scale(m.border_width));
    const int radius = std::max(0, m.scale(m.corner_radius));
    const int padding = std::max(0, m.scale(m.padding));
    const int w = static_cast<int>(client.right) - static_cast<int>(client.left);
    const int h = static_cast<int>(client.bottom) - static_cast<int>(client.top);

    // Arrow geometry is settled first so the text rect can stop short of it:
    // the label then physically cannot run under the chevron, ellipsis or not.
    const int arrow_w = std::max(2, m.scale(8));
    const int arrow_h = std::max(2, m.scale(5));
    const int arrow_left = std::max(0, w - padding - arrow_w);
    const int arrow_top = (h - arrow_h) / 2;

    const int count = item_count(s);
    const int sel = (s != nullptr) ? s->selected : -1;
    const bool have_selection = (sel >= 0 && sel < count);

    static const std::wstring placeholder(kPlaceholderText);
    const std::wstring& label = have_selection ? s->items[static_cast<size_t>(sel)] : placeholder;
    const ColorRole label_role = have_selection ? ColorRole::Text : ColorRole::TextDisabled;
    const ColorRole arrow_role = open ? ColorRole::Accent : ColorRole::TextMuted;

    RECT text_rc{};
    text_rc.left = border + padding;
    text_rc.top = client.top;
    text_rc.right =
        std::max(static_cast<int>(text_rc.left) + 1, arrow_left - padding);
    text_rc.bottom = client.bottom;

    IDWriteTextFormat* fmt =
        ui::gfx::text_format(kBodyFontFamily, kBodyFontSizePt, DWRITE_FONT_WEIGHT_NORMAL);

    bool text_drawn = false;
    ui::gfx::Canvas canvas(dc);
    if (canvas.valid()) {
        canvas.set_dpi(m.dpi);
        canvas.begin();

        // The window stays rectangular; only the *body* is rounded, so the
        // corners show the parent's own background rather than a square slab of
        // sunken fill.
        canvas.clear_raw(ui::theming::color(ColorRole::Surface));
        const D2D1_ROUNDED_RECT body = ui::gfx::rounded_rect(client, static_cast<float>(radius));
        canvas.fill_rounded(body, ColorRole::SurfaceSunken);
        canvas.stroke_rounded(body, open ? ColorRole::BorderStrong : ColorRole::Border,
                              static_cast<float>(border));

        // Chevron: two strokes meeting at a tip that flips when the list is open.
        const float stroke = static_cast<float>(std::max(1, m.scale(1)));
        const float tip_x = static_cast<float>(arrow_left + (arrow_w / 2));
        const float left_x = static_cast<float>(arrow_left);
        const float right_x = static_cast<float>(arrow_left + arrow_w);
        const float top_y = static_cast<float>(arrow_top);
        const float bottom_y = static_cast<float>(arrow_top + arrow_h);
        const D2D1_POINT_2F tip{tip_x, open ? top_y : bottom_y};
        const D2D1_POINT_2F tail_left{left_x, open ? bottom_y : top_y};
        const D2D1_POINT_2F tail_right{right_x, open ? bottom_y : top_y};
        canvas.line(tip, tail_left, arrow_role, stroke);
        canvas.line(tip, tail_right, arrow_role, stroke);

        if (fmt != nullptr && text_rc.right > text_rc.left) {
            canvas.text(label, text_rc, label_role, fmt);
            text_drawn = true;
        }
        canvas.end();
    } else {
        // No usable DC render target: plain GDI so the control is still legible.
        fill_rect_gdi(dc, client, ColorRole::Surface);
        fill_rect_gdi(dc, client, ColorRole::SurfaceSunken);
        FrameRect(dc, &client, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        draw_text_gdi(dc, label, text_rc, label_role, m.dpi);
        text_drawn = true;
    }

    if (!text_drawn) {
        // DirectWrite produced nothing usable: fall back to DrawTextW, whose
        // DT_END_ELLIPSIS is the GDI spelling of DWRITE_TRIMMING_TRAILING.
        draw_text_gdi(dc, label, text_rc, label_role, m.dpi);
    }
}

// ---------------------------------------------------------------------------
// Painting - popup
// ---------------------------------------------------------------------------

void paint_popup(HWND popup, HDC dc, ComboState* s) {
    if (s == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(popup, &client);
    const ui::theming::Metrics m = ui::theming::metrics_for(popup);
    const PopupGeom g = compute_geom(popup, s, m);
    const int radius = std::max(0, m.scale(m.corner_radius));
    const int row_radius = std::max(0, m.scale(2));
    const int count = item_count(s);

    IDWriteTextFormat* fmt =
        ui::gfx::text_format(kBodyFontFamily, kBodyFontSizePt, DWRITE_FONT_WEIGHT_NORMAL);

    // Every row is drawn - these lists are shader names and enum labels, never
    // long enough to be worth virtualising. The row_area bounds are pulled
    // into ints up front so no LONG ever reaches a std::max/std::min.
    const int rows_top = static_cast<int>(g.row_area.top);
    const int rows_bottom = static_cast<int>(g.row_area.bottom);
    const int rows_left = static_cast<int>(g.row_area.left);
    const int rows_right = static_cast<int>(g.row_area.right);

    auto row_rect = [&](int i) {
        const int top = rows_top + ((i - s->scroll) * g.row_h);
        return RECT{rows_left, std::max(rows_top, top), rows_right,
                    std::min(rows_bottom, top + g.row_h)};
    };
    auto row_visible = [&](int i) {
        const int top = rows_top + ((i - s->scroll) * g.row_h);
        return (top + g.row_h > rows_top) && (top < rows_bottom);
    };
    auto row_text_rect = [&](const RECT& row) {
        RECT tr = row;
        tr.left = static_cast<int>(row.left) + m.scale(m.padding);
        tr.right = std::max(static_cast<int>(tr.left) + 1,
                            static_cast<int>(row.right) - m.scale(m.padding));
        return tr;
    };

    bool text_drawn = false;
    ui::gfx::Canvas canvas(dc);
    if (canvas.valid()) {
        canvas.set_dpi(m.dpi);
        canvas.begin();
        canvas.clear_raw(ui::theming::color(ColorRole::Surface));

        for (int i = 0; i < count; ++i) {
            if (!row_visible(i)) {
                continue;
            }
            const bool selected = (i == s->selected);
            const bool hovered = (i == s->highlight);

            ColorRole fill = ColorRole::SurfaceAlt;
            if (selected) {
                fill = ColorRole::Accent;
            } else if (hovered) {
                fill = ColorRole::AccentMuted;
            }

            const RECT row = row_rect(i);
            canvas.fill_rounded(ui::gfx::rounded_rect(row, static_cast<float>(row_radius)), fill);

            if (fmt != nullptr) {
                canvas.text(s->items[static_cast<size_t>(i)], row_text_rect(row),
                            selected ? ColorRole::TextOnAccent : ColorRole::Text, fmt);
                text_drawn = true;
            }
        }

        // SurfaceHover only reads as a distinct fill on the hover-only row, so
        // nudge it with a hairline rather than flattening it onto SurfaceAlt.
        if (s->highlight >= 0 && s->highlight != s->selected && row_visible(s->highlight)) {
            canvas.stroke_rounded(
                ui::gfx::rounded_rect(row_rect(s->highlight), static_cast<float>(row_radius)),
                ColorRole::Border, static_cast<float>(g.border));
        }

        // Scrollbar drawn by us rather than as a SCROLLBAR_CLASSW child, so it
        // follows the theme instead of the system scrollbar metrics.
        if (g.has_scroll) {
            canvas.fill_rounded(ui::gfx::rounded_rect(g.track, static_cast<float>(row_radius)),
                                ColorRole::SurfaceSunken);
            canvas.fill_rounded(ui::gfx::rounded_rect(g.thumb, static_cast<float>(row_radius)),
                                ColorRole::BorderStrong);
        }

        canvas.stroke_rounded(ui::gfx::rounded_rect(client, static_cast<float>(radius)),
                              ColorRole::Border, static_cast<float>(g.border));
        canvas.end();
    } else {
        fill_rect_gdi(dc, client, ColorRole::Surface);
        for (int i = 0; i < count; ++i) {
            if (!row_visible(i)) {
                continue;
            }
            ColorRole fill = ColorRole::SurfaceAlt;
            if (i == s->selected) {
                fill = ColorRole::Accent;
            } else if (i == s->highlight) {
                fill = ColorRole::AccentMuted;
            }
            const RECT row = row_rect(i);
            fill_rect_gdi(dc, row, fill);
            draw_text_gdi(dc, s->items[static_cast<size_t>(i)], row_text_rect(row),
                          (i == s->selected) ? ColorRole::TextOnAccent : ColorRole::Text, m.dpi);
        }
        if (g.has_scroll) {
            fill_rect_gdi(dc, g.track, ColorRole::SurfaceSunken);
            fill_rect_gdi(dc, g.thumb, ColorRole::BorderStrong);
        }
        FrameRect(dc, &client, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        text_drawn = true;
    }

    // Canvas was fine but DirectWrite had no format: finish the labels with GDI.
    if (!text_drawn && fmt == nullptr) {
        for (int i = 0; i < count; ++i) {
            if (!row_visible(i)) {
                continue;
            }
            const RECT row = row_rect(i);
            draw_text_gdi(dc, s->items[static_cast<size_t>(i)], row_text_rect(row),
                          (i == s->selected) ? ColorRole::TextOnAccent : ColorRole::Text, m.dpi);
        }
    }
}

// ---------------------------------------------------------------------------
// Pointer tracking shared by both windows
// ---------------------------------------------------------------------------
//
// The mouse session normally belongs to the combo (it takes SetCapture when the
// list is opened by a click, so a press on the control and a release on a row
// behave like a native dropdown). When the popup was opened from the keyboard
// nothing holds the capture and the messages land on the popup instead. Either
// way the logic below is the same, so it lives in one place.

void track_pointer(HWND popup, ComboState* s, int client_y) {
    if (s == nullptr || popup == nullptr) {
        return;
    }
    const HWND combo = GetWindow(popup, GW_OWNER);
    if (combo == nullptr) {
        return;
    }

    const PopupGeom g = compute_geom(popup, s, ui::theming::metrics_for(popup));

    // Drag past either edge auto-scrolls.
    if (client_y < g.row_area.top) {
        set_scroll(combo, s, s->scroll - 1);
    } else if (client_y >= g.row_area.bottom) {
        set_scroll(combo, s, s->scroll + 1);
    }

    const int row = hit_row(s, g, client_y);
    if (row >= 0 && row != s->highlight) {
        s->highlight = row;
        invalidate_popup(s);
        invalidate_combo(combo);
    }
}

// Translates a screen point into popup-client space. False when there is no
// popup or the translation fails.
bool to_popup_space(ComboState* s, int screen_x, int screen_y, POINT& out) {
    if (s == nullptr || s->popup == nullptr) {
        return false;
    }
    POINT pt{screen_x, screen_y};
    if (ScreenToClient(s->popup, &pt) == FALSE) {
        return false;
    }
    out = pt;
    return true;
}

// ---------------------------------------------------------------------------
// Popup window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    ComboState* s = state_of(hwnd);

    switch (message) {
        case WM_NCCREATE: {
            const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            if (cs == nullptr || cs->lpCreateParams == nullptr) {
                return FALSE;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }

        case WM_ERASEBKGND:
            // Fully repainted in WM_PAINT; do not let the system flicker it.
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            paint_popup(hwnd, dc, state_of(hwnd));
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE:
            track_pointer(hwnd, s, GET_Y_LPARAM(lparam));
            return 0;

        case WM_LBUTTONDOWN:
            if (s != nullptr) {
                track_pointer(hwnd, s, GET_Y_LPARAM(lparam));
                SetCapture(hwnd);
            }
            return 0;

        case WM_LBUTTONUP: {
            const HWND combo = GetWindow(hwnd, GW_OWNER);
            if (s != nullptr && combo != nullptr && s->popup == hwnd) {
                track_pointer(hwnd, s, GET_Y_LPARAM(lparam));
                const PopupGeom g = compute_geom(hwnd, s, ui::theming::metrics_for(hwnd));
                const int row = hit_row(s, g, GET_Y_LPARAM(lparam));
                if (row >= 0) {
                    s->highlight = row;
                    close_dropdown(combo, s, CloseMode::Commit);
                } else {
                    close_dropdown(combo, s, CloseMode::Discard);
                }
                // close_dropdown destroyed `hwnd`. Nothing below may touch it.
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Capture lost without a commit (Alt+Tab, another window taking the
            // mouse): treat it exactly like a click outside the list.
            if (s != nullptr && s->dropdown_open && s->popup == hwnd) {
                const HWND combo = GetWindow(hwnd, GW_OWNER);
                if (combo != nullptr) {
                    close_dropdown(combo, s, CloseMode::Discard);
                }
            }
            break;

        case WM_MOUSEWHEEL: {
            const HWND combo = GetWindow(hwnd, GW_OWNER);
            if (s != nullptr && combo != nullptr) {
                const int notches =
                    static_cast<int>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
                set_scroll(combo, s, s->scroll - (notches * kWheelRowsPerNotch));
            }
            return 0;
        }

        case WM_KEYDOWN: {
            const HWND combo = GetWindow(hwnd, GW_OWNER);
            if (s != nullptr && combo != nullptr &&
                handle_combo_key(combo, s, static_cast<UINT>(wparam))) {
                return 0;
            }
            break;
        }

        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

        case WM_DPICHANGED:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_NCDESTROY:
            // Runs synchronously out of destroy_popup_window(), i.e. while the
            // owning combo's state is still allocated. This is the ONLY place
            // the popup procedure dereferences that pointer - see the lifetime
            // note at the top of this file.
            if (s != nullptr) {
                s->popup = nullptr;
                s->dropdown_open = false;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;

        default:
            break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Combo window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK ComboWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    ComboState* s = state_of(hwnd);

    switch (message) {
        // ---- lifetime ------------------------------------------------------

        case WM_NCCREATE: {
            ComboState* created = new (std::nothrow) ComboState();
            if (created == nullptr) {
                return FALSE;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            return TRUE;
        }

        case WM_DESTROY:
            // Tear the popup down first: the state must outlive it, not the
            // other way round.
            if (s != nullptr) {
                destroy_popup_window(s);
            }
            return 0;

        case WM_NCDESTROY: {
            ComboState* dying = state_of(hwnd);
            if (dying != nullptr) {
                destroy_popup_window(dying); // the popup's WM_NCDESTROY runs here
                delete dying;                 // and only now is the state freed
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            break;
        }

        // ---- painting ------------------------------------------------------

        case WM_ERASEBKGND:
            // Everything is repainted in WM_PAINT; refuse the system erase so
            // the control cannot flicker.
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            paint_combo(hwnd, dc, state_of(hwnd));
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DPICHANGED:
            invalidate_combo(hwnd);
            invalidate_popup(s);
            return 0;

        case WM_ENABLE:
            invalidate_combo(hwnd);
            return 0;

        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            // Losing focus tears the list down - the popup can never be left
            // stranded on screen behind another window.
            if (s != nullptr && s->dropdown_open) {
                close_dropdown(hwnd, s, CloseMode::Discard);
            }
            invalidate_combo(hwnd);
            break;
        }

        // ---- native combo messages -----------------------------------------

        case CB_ADDSTRING: {
            if (s == nullptr) {
                return CB_ERR;
            }
            // Native ignores wParam; the contract is that it is 0. lParam may
            // be null, and CB_ERR is the documented "bad pointer" sentinel that
            // must be rejected BEFORE it is dereferenced.
            if (lparam == 0 || lparam == static_cast<LPARAM>(CB_ERR)) {
                return CB_ERR;
            }
            std::wstring value;
            if (!read_item_text(reinterpret_cast<const wchar_t*>(lparam), value)) {
                return CB_ERRSPACE;
            }
            if (s->dropdown_open) {
                close_dropdown(hwnd, s, CloseMode::Discard);
            }
            const bool was_empty = s->items.empty();
            if (!push_item(s, value)) {
                return CB_ERRSPACE;
            }
            align_item_data(s);
            // Native auto-selects the first entry of an empty list.
            if (was_empty && s->selected < 0) {
                set_selection(hwnd, s, 0, false);
            }
            after_list_change(hwnd, s);
            return static_cast<LRESULT>(item_count(s) - 1);
        }

        case CB_INSERTSTRING: {
            if (s == nullptr) {
                return CB_ERR;
            }
            if (lparam == 0 || lparam == static_cast<LPARAM>(CB_ERR)) {
                return CB_ERR;
            }
            std::wstring value;
            if (!read_item_text(reinterpret_cast<const wchar_t*>(lparam), value)) {
                return CB_ERRSPACE;
            }
            if (s->dropdown_open) {
                close_dropdown(hwnd, s, CloseMode::Discard);
            }
            // wParam is the index; -1, or anything past the end, appends.
            int index = static_cast<int>(wparam);
            if (index < 0 || index > item_count(s)) {
                index = item_count(s);
            }
            if (!insert_item(s, index, value)) {
                return CB_ERRSPACE;
            }
            align_item_data(s);
            after_list_change(hwnd, s);
            return static_cast<LRESULT>(index);
        }

        case CB_DELETESTRING: {
            if (s == nullptr) {
                return CB_ERR;
            }
            const int index = static_cast<int>(wparam);
            if (!valid_index(s, index)) {
                return CB_ERR;
            }
            if (s->dropdown_open) {
                close_dropdown(hwnd, s, CloseMode::Discard);
            }
            erase_item(s, index);
            align_item_data(s);
            after_list_change(hwnd, s);
            return static_cast<LRESULT>(item_count(s));
        }

        case CB_RESETCONTENT: {
            if (s == nullptr) {
                return 0;
            }
            if (s->dropdown_open) {
                close_dropdown(hwnd, s, CloseMode::Discard);
            }
            s->items.clear();
            s->item_data.clear();
            s->selected = -1;
            s->highlight = -1;
            s->scroll = 0;
            s->committed_selection = -1;
            s->typeahead.clear();
            s->typeahead_tick = 0;
            invalidate_combo(hwnd);
            return 0;
        }

        case CB_GETCOUNT:
            return static_cast<LRESULT>(item_count(s));

        case CB_GETCURSEL:
            if (s == nullptr || s->selected < 0) {
                return CB_ERR;
            }
            return static_cast<LRESULT>(s->selected);

        case CB_SETCURSEL: {
            if (s == nullptr) {
                return 1;
            }
            // Native does NOT notify for CB_SETCURSEL.
            set_selection(hwnd, s, static_cast<int>(wparam), false);
            s->highlight = s->selected;
            return 1;
        }

        case CB_GETLBTEXT: {
            if (s == nullptr || lparam == 0) {
                return CB_ERR;
            }
            const int index = static_cast<int>(wparam);
            if (!valid_index(s, index)) {
                return CB_ERR;
            }
            const std::wstring& item = s->items[static_cast<size_t>(index)];

            // SAFETY: `item.size() <= kMaxItemChars` is an invariant maintained
            // on the way in (read_item_text), and CB_GETLBTEXTLEN reports
            // exactly this length. A caller that sized its buffer from
            // CB_GETLBTEXTLEN therefore has room for item.size() + 1 wchar_t -
            // the text plus its terminating NUL - and not one element more.
            // Exactly length + 1 wchar_t are written below, nothing more.
            wchar_t* buffer = reinterpret_cast<wchar_t*>(lparam);
            const size_t length = item.size();
            if (length > 0) {
                memcpy(buffer, item.c_str(), length * sizeof(wchar_t));
            }
            buffer[length] = L'\0';
            return static_cast<LRESULT>(length);
        }

        case CB_GETLBTEXTLEN: {
            if (s == nullptr) {
                return CB_ERR;
            }
            const int index = static_cast<int>(wparam);
            if (!valid_index(s, index)) {
                return CB_ERR;
            }
            return static_cast<LRESULT>(s->items[static_cast<size_t>(index)].size());
        }

        case CB_SETITEMDATA: {
            if (s == nullptr) {
                return CB_ERR;
            }
            const int index = static_cast<int>(wparam);
            if (!valid_index(s, index)) {
                return CB_ERR;
            }
            align_item_data(s);
            s->item_data[static_cast<size_t>(index)] = static_cast<INT_PTR>(lparam);
            return TRUE;
        }

        case CB_GETITEMDATA: {
            if (s == nullptr) {
                return CB_ERR;
            }
            const int index = static_cast<int>(wparam);
            if (!valid_index(s, index)) {
                return CB_ERR;
            }
            align_item_data(s);
            return static_cast<LRESULT>(s->item_data[static_cast<size_t>(index)]);
        }

        case CB_FINDSTRINGEXACT:
        case CB_FINDSTRING: {
            if (s == nullptr || lparam == 0) {
                return CB_ERR;
            }
            std::wstring needle;
            if (!read_item_text(reinterpret_cast<const wchar_t*>(lparam), needle)) {
                return CB_ERR;
            }
            const int count = item_count(s);
            const int start = (static_cast<int>(wparam) >= 0) ? static_cast<int>(wparam) : -1;
            for (int step = 0; step < count; ++step) {
                int index = start + step;
                if (index >= count) {
                    index -= count;
                }
                if (has_prefix_ci(s->items[static_cast<size_t>(index)], needle)) {
                    return static_cast<LRESULT>(index);
                }
            }
            return CB_ERR;
        }

        case CB_SHOWDROPDOWN: {
            if (s == nullptr) {
                return 0;
            }
            if (wparam != 0) {
                open_dropdown(hwnd, s);
            } else {
                close_dropdown(hwnd, s, CloseMode::Discard);
            }
            return 0;
        }

        // ---- input ---------------------------------------------------------

        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            if (s == nullptr) {
                break;
            }
            // Hold the capture for the whole popup session: a press here and a
            // release over a row then behaves like a native dropdown, and a
            // press anywhere else reaches us and closes the list.
            SetCapture(hwnd);
            if (s->dropdown_open) {
                close_dropdown(hwnd, s, CloseMode::Discard);
            } else {
                open_dropdown(hwnd, s);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (s != nullptr && s->dropdown_open && GetCapture() == hwnd && s->popup != nullptr) {
                POINT pt{};
                int row = -1;
                RECT rc{};
                if (to_popup_space(s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), pt) &&
                    GetClientRect(s->popup, &rc) != FALSE &&
                    pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom) {
                    row = hit_row(s, compute_geom(s->popup, s, ui::theming::metrics_for(hwnd)),
                                  pt.y);
                }
                if (row >= 0) {
                    s->highlight = row;
                    close_dropdown(hwnd, s, CloseMode::Commit);
                } else {
                    close_dropdown(hwnd, s, CloseMode::Discard);
                }
            }
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (s != nullptr && s->dropdown_open && s->popup != nullptr && GetCapture() == hwnd) {
                POINT pt{};
                if (to_popup_space(s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), pt)) {
                    track_pointer(s->popup, s, pt.y);
                }
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            if (s != nullptr && s->dropdown_open) {
                close_dropdown(hwnd, s, CloseMode::Discard);
            }
            break;

        case WM_KEYDOWN:
            if (s != nullptr && handle_combo_key(hwnd, s, static_cast<UINT>(wparam))) {
                return 0;
            }
            break;

        case WM_CHAR:
            if (s != nullptr && is_printable_char(wparam)) {
                apply_typeahead(hwnd, s, static_cast<wchar_t>(wparam));
                return 0;
            }
            break;

        default:
            break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

bool ThemeCombo::register_class(HINSTANCE instance) {
    const HINSTANCE hinst = (instance != nullptr) ? instance : GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ComboWndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // WM_ERASEBKGND + WM_PAINT do the work
    wc.lpszClassName = kComboClassName;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    WNDCLASSEXW wp{};
    wp.cbSize = sizeof(wp);
    wp.style = CS_HREDRAW | CS_VREDRAW;
    wp.lpfnWndProc = PopupWndProc;
    wp.hInstance = hinst;
    wp.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wp.hbrBackground = nullptr;
    wp.lpszClassName = kPopupClassName;
    if (RegisterClassExW(&wp) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    return true;
}

const wchar_t* ThemeCombo::class_name() { return kComboClassName; }

HWND ThemeCombo::create(HWND parent, int id, int x, int y, int width, int height) {
    if (parent == nullptr) {
        return nullptr;
    }
    if (!register_class(GetModuleHandleW(nullptr))) {
        return nullptr;
    }

    int control_height = height;
    if (control_height <= 0) {
        const ui::theming::Metrics m = ui::theming::metrics_for(parent);
        control_height = m.scale(m.control_height);
    }

    return CreateWindowExW(0, kComboClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, width,
                           control_height, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                           nullptr);
}

} // namespace ui::controls