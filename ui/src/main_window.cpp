#include "main_window.h"

#include "about_dialog.h"
#include "app_hooks.h"
#include "app_log.h"
#include "connection_dialog.h"
#include "material_browser_window.h"
#include "preview_options_dialog.h"
#include "bridge/engine_view.h"
#include "ui/connection_model.h"
#include "ui/material_browser.h"
#include "ui/panel_model.h"
#include "ui/preview_options.h"
#include "ui/preview_sync.h"
#include "ui/session.h"
#include "ui/settings.h"
#include "ui/text_encoding.h"
#include "ui/validate.h"
#include "vmt/value.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui {
namespace {

constexpr wchar_t kMainClass[] = L"VMTViewerMainWindow";
constexpr wchar_t kPanelClass[] = L"VMTViewerParameterPanel";
constexpr wchar_t kViewportClass[] = L"VMTViewerViewport";
constexpr wchar_t kValidationClass[] = L"VMTViewerValidation";
constexpr wchar_t kWindowTitle[] = L"VMT Viewer";

constexpr int kLeftWidth = 215;
constexpr int kRightWidth = 350;
constexpr int kBottomHeight = 220;
constexpr int kRowHeight = 28;
constexpr int kRowEditX = 122;
constexpr int kRowEditWidth = 86;

// Rebuilding the whole panel on every keystroke causes flicker; coalesce text
// edits with a short timer instead.
constexpr UINT_PTR kPanelRebuildTimer = 1;
constexpr UINT kPanelRebuildDelayMs = 150;

// Engine preview: render at ~60 Hz and debounce device resets on resize.
constexpr UINT_PTR kTickerTimer = 2;
constexpr UINT kTickerIntervalMs = 16;
constexpr UINT_PTR kViewportResizeTimer = 3;
constexpr UINT kViewportResizeDelayMs = 200;

// Live preview sync: coalesce rapid edits into one file write + engine reload,
// comfortably inside the one-second update budget.
constexpr UINT_PTR kPreviewSyncTimer = 4;
constexpr UINT kPreviewSyncDelayMs = 250;

enum ControlId : int {
    ID_OPEN = 1001,
    ID_SAVE,
    ID_SAVE_AS,
    ID_RECENT,
    ID_VIEWPORT,
    ID_PANEL,
    ID_TEXT,
    ID_STATUS,
    ID_ADVANCED,
    ID_PREVIEW,
    ID_VALIDATE,
    IDM_EXIT = 1100,
    IDM_ABOUT,
    IDM_GAME_CONNECTION,
    IDM_BROWSER,
    IDM_PREVIEW_OPTIONS,
    IDM_MODEL,
    IDM_SHAPE_SPHERE,
    IDM_SHAPE_CUBE,
    IDM_SHAPE_CYLINDER,
    IDM_SHAPE_PLANE,
    ID_ROW_EDIT_BASE = 2000,
    ID_ROW_SLIDER_BASE = 3000,
    ID_ROW_COMBO_BASE = 4000,
    ID_ROW_CHECK_BASE = 5000,
};

struct PanelRowUi {
    std::string name;
    std::string description; // or warning for unrecognized rows
    vmt::ParamType type = vmt::ParamType::String;
    bool is_float = false;
    double range_lo = 0.0;
    double range_hi = 1.0;
    HWND edit = nullptr;
    HWND slider = nullptr;
    HWND combo = nullptr;
    HWND check = nullptr;
};

struct PanelItem {
    HWND hwnd = nullptr;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct App {
    HINSTANCE instance = nullptr;
    HWND main = nullptr;
    HACCEL accelerator = nullptr;

    HWND open_button = nullptr;
    HWND save_button = nullptr;
    HWND save_as_button = nullptr;
    HWND preview_button = nullptr;
    HWND validate_button = nullptr;
    HWND recent_label = nullptr;
    HWND recent_list = nullptr;
    HWND viewport = nullptr;
    HWND advanced_check = nullptr;
    HWND panel = nullptr;
    HWND text_edit = nullptr;
    HWND status = nullptr;

    EditorSession session;
    vmt::SchemaLibrary schemas;
    bool schemas_loaded = false;
    std::string schema_error;

    UISettings settings;
    std::filesystem::path settings_path;

    std::vector<PanelRowUi> rows;
    std::vector<PanelItem> panel_items;
    int content_height = 0;
    int scroll_y = 0;

    bool rebuilding = false;
    bool suppress_text = false;
    TextEncoding encoding = TextEncoding::Utf8;
    int dpi = 96;

    // Engine preview state.
    std::unique_ptr<bridge::EngineView> engine;
    std::filesystem::path preview_root;
    std::filesystem::path thumbnail_root;
    bool preview_mounted = false;
    bool preview_active = false;
    ui::PreviewSync preview_sync;
    int pending_viewport_width = 0;
    int pending_viewport_height = 0;
    std::wstring engine_note;
    std::string engine_probe_key;        // "<bin>|<game>" that already passed the probe
    std::string engine_probe_failed_key; // "<bin>|<game>" that already failed the probe
    std::string engine_active_key;       // "<bin>|<game>" the running engine was loaded from

    // Phase 9: game-relative name the engine previews and the preview override
    // file is written at ("brick/brickwall001a"), plus the engine's current one.
    std::string preview_material_name = ui::kPreviewMaterialName;
    std::string engine_material_name;
    ui::PreviewOptions preview_options;

    // Validation results (Phase 8). findings[i] is list row i.
    HWND validation_window = nullptr;
    HWND validation_list = nullptr;
    std::vector<ui::Finding> findings;
    std::wstring validation_summary;

    std::wstring status_hint; // shown in status part 1 while a control is focused
};

App g;

// --- Small helpers -----------------------------------------------------------

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), length);
    return wide;
}

std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::wstring window_text(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

void set_font(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HFONT stock_font() {
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// Scales a 96-DPI design unit to the current system DPI.
int dp(int value) {
    return MulDiv(value, g.dpi, 96);
}

HFONT bold_font() {
    static HFONT font = nullptr;
    if (font == nullptr) {
        LOGFONTW lf{};
        GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
        lf.lfWeight = FW_BOLD;
        font = CreateFontIndirectW(&lf);
    }
    return font;
}

HFONT mono_font() {
    static HFONT font = nullptr;
    if (font == nullptr) {
        LOGFONTW lf{};
        lf.lfHeight = -dp(13);
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        lstrcpynW(lf.lfFaceName, L"Consolas", LF_FACESIZE);
        font = CreateFontIndirectW(&lf);
    }
    return font;
}

const char kBom[] = "\xEF\xBB\xBF";

bool has_utf8_bom(const std::string& text) {
    return text.size() >= 3 && std::memcmp(text.data(), kBom, 3) == 0;
}

// Session bytes -> control text (the control needs CRLF; BOM is stripped and
// the display encoding is detected and reported back in `encoding`).
std::wstring make_control_text(const std::string& session_text, TextEncoding& encoding) {
    const std::string body =
        has_utf8_bom(session_text) ? session_text.substr(3) : session_text;
    std::wstring wide = decode_for_display(body, encoding);
    if (body.find('\r') == std::string::npos) {
        std::wstring crlf;
        crlf.reserve(wide.size() + 16);
        for (const wchar_t c : wide) {
            if (c == L'\n') {
                crlf += L"\r\n";
            } else {
                crlf += c;
            }
        }
        wide.swap(crlf);
    }
    return wide;
}

// Text control content -> session bytes (restores BOM and the original
// newline style detected in the session text before the edit).
std::string make_session_text(const std::string& session_before, TextEncoding encoding,
                              const std::wstring& control) {
    std::wstring wide = control;
    if (session_before.find('\r') == std::string::npos) {
        std::wstring lf;
        lf.reserve(wide.size());
        for (const wchar_t c : wide) {
            if (c != L'\r') {
                lf += c;
            }
        }
        wide.swap(lf);
    }
    std::string text = encode_from_display(wide, encoding);
    if (has_utf8_bom(session_before)) {
        text.insert(0, kBom);
    }
    return text;
}

std::filesystem::path compute_settings_path() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const std::filesystem::path dir = std::filesystem::path(buffer) / L"VMTViewer";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"settings.ini";
}

// --- Panel scroll ------------------------------------------------------------

void apply_panel_scroll() {
    for (const PanelItem& item : g.panel_items) {
        MoveWindow(item.hwnd, item.x, item.y - g.scroll_y, item.w, item.h, TRUE);
    }
}

void set_panel_scroll(int y) {
    RECT rc{};
    GetClientRect(g.panel, &rc);
    const int max_scroll = std::max(0, g.content_height - static_cast<int>(rc.bottom));
    g.scroll_y = std::max(0, std::min(y, max_scroll));
    apply_panel_scroll();
    SetScrollPos(g.panel, SB_VERT, g.scroll_y, TRUE);
}

void update_panel_scrollbar() {
    RECT rc{};
    GetClientRect(g.panel, &rc);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, g.content_height - 1);
    si.nPage = static_cast<UINT>(std::max(0, static_cast<int>(rc.bottom)));
    si.nPos = g.scroll_y;
    SetScrollInfo(g.panel, SB_VERT, &si, TRUE);
}

// --- Status / title ----------------------------------------------------------

void update_title() {
    std::string title = g.session.display_name();
    if (g.session.dirty()) {
        title += " *";
    }
    title += " - VMT Viewer";
    SetWindowTextW(g.main, to_wide(title).c_str());
}

void update_status() {
    std::wstring first;
    if (g.session.has_document()) {
        const vmt::Document* document = g.session.document();
        std::string text = document->name();
        if (!text.empty()) {
            text += "  |  shader: ";
            text += std::string(document->shader());
        }
        first = to_wide(text);
    } else {
        first = L"(no material loaded)";
    }
    SendMessageW(g.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(first.c_str()));

    std::wstring second = g.status_hint;
    if (second.empty()) {
        if (!g.session.text_is_valid()) {
            const vmt::Error& error = g.session.last_error();
            std::string message = error.message;
            if (error.line != 0) {
                message += " (line " + std::to_string(error.line) + ")";
            }
            second = L"syntax error: " + to_wide(message);
        } else if (g.session.has_document() &&
                   !g.session.document()->diagnostics().empty()) {
            const std::vector<vmt::Diagnostic>& diagnostics =
                g.session.document()->diagnostics();
            std::string message = std::to_string(diagnostics.size()) + " syntax issue(s)";
            if (diagnostics.front().location.line > 0) {
                message += ", first at line " +
                           std::to_string(diagnostics.front().location.line);
            }
            second = to_wide(message);
        } else if (!g.schema_error.empty()) {
            second = L"schemas not loaded: " + to_wide(g.schema_error);
        } else if (!g.validation_summary.empty()) {
            second = g.validation_summary;
        } else if (!g.engine_note.empty()) {
            second = g.engine_note;
        } else {
            second = L"ready";
        }
    }
    SendMessageW(g.status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(second.c_str()));
}

void show_error_box(const vmt::Error& error) {
    const std::wstring text = to_wide(error.message) +
                              (error.remediation.empty() ? std::wstring()
                                                         : L"\n\n" + to_wide(error.remediation));
    MessageBoxW(g.main, text.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
}

// --- Panel construction ------------------------------------------------------

std::string format_number(vmt::ParamType type, double value) {
    if (type == vmt::ParamType::Int) {
        return vmt::format_int(static_cast<int>(std::llround(value)));
    }
    return vmt::format_float(static_cast<float>(value));
}

void compute_slider_range(vmt::ParamType type, const vmt::SchemaParam* schema_param,
                          double current, double& lo, double& hi) {
    if (schema_param != nullptr && schema_param->has_range &&
        schema_param->max_value > schema_param->min_value) {
        lo = schema_param->min_value;
        hi = schema_param->max_value;
        return;
    }
    // No schema range: a convenience window around the current value. The
    // numeric field stays the authoritative input; this never invents ranges
    // in the schema data.
    const double minimum_span = type == vmt::ParamType::Int ? 10.0 : 1.0;
    const double magnitude = std::max(minimum_span, std::fabs(current) * 2.0);
    lo = std::min(0.0, current);
    hi = lo + magnitude;
}

int slider_position(double value, double lo, double hi, bool is_float) {
    if (is_float) {
        if (hi <= lo) {
            return 0;
        }
        const double fraction = (value - lo) / (hi - lo);
        const double clamped = std::max(0.0, std::min(1.0, fraction));
        return static_cast<int>(std::llround(clamped * 1000.0));
    }
    const double clamped = std::max(lo, std::min(hi, std::round(value)));
    return static_cast<int>(clamped);
}

double slider_value(int position, double lo, double hi, bool is_float) {
    if (is_float) {
        const double fraction = static_cast<double>(position) / 1000.0;
        return lo + (hi - lo) * fraction;
    }
    return static_cast<double>(position);
}

void rebuild_panel() {
    g.rebuilding = true;

    std::vector<HWND> children;
    EnumChildWindows(
        g.panel,
        [](HWND child, LPARAM param) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(param)->push_back(child);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&children));
    for (HWND child : children) {
        DestroyWindow(child);
    }
    g.rows.clear();
    g.panel_items.clear();

    RECT rc{};
    GetClientRect(g.panel, &rc);
    const int panel_width = static_cast<int>(rc.right);
    const int content_width = std::max(dp(60), panel_width - dp(18));

    const vmt::Document* document = g.session.document();
    vmt::ShaderSchema schema;
    if (g.schemas_loaded) {
        schema = g.schemas.schema_for(document != nullptr ? std::string(document->shader())
                                                          : std::string());
    }
    const std::vector<PanelGroup> groups =
        build_panel_model(document, schema, g.settings.show_advanced);

    int y = dp(6);
    for (const PanelGroup& group : groups) {
        HWND title = CreateWindowExW(0, L"STATIC", to_wide(group.name).c_str(),
                                     WS_CHILD | WS_VISIBLE, dp(8), y, content_width, dp(18),
                                     g.panel, nullptr, g.instance, nullptr);
        set_font(title, bold_font());
        g.panel_items.push_back(PanelItem{title, dp(8), y, content_width, dp(18)});
        y += dp(22);

        for (const PanelRow& row : group.rows) {
            PanelRowUi ui_row;
            ui_row.name = row.name;
            ui_row.description = row.kind == PanelRow::Kind::Unrecognized ? row.warning
                                                                          : row.description;
            ui_row.type = row.type;

            const std::wstring label_text =
                row.kind == PanelRow::Kind::Unrecognized ? to_wide("[!] " + row.name)
                                                         : to_wide(row.name);
            HWND label = CreateWindowExW(0, L"STATIC", label_text.c_str(),
                                         WS_CHILD | WS_VISIBLE, dp(8), y + dp(3), dp(108), dp(18),
                                         g.panel, nullptr, g.instance, nullptr);
            set_font(label, stock_font());
            g.panel_items.push_back(PanelItem{label, dp(8), y + dp(3), dp(108), dp(18)});

            const std::size_t index = g.rows.size();
            const int edit_id = ID_ROW_EDIT_BASE + static_cast<int>(index);
            const int slider_id = ID_ROW_SLIDER_BASE + static_cast<int>(index);
            const int combo_id = ID_ROW_COMBO_BASE + static_cast<int>(index);
            const int check_id = ID_ROW_CHECK_BASE + static_cast<int>(index);

            if (row.type == vmt::ParamType::Bool) {
                ui_row.check = CreateWindowExW(0, L"BUTTON", L"",
                                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                               dp(kRowEditX), y + dp(4), dp(22), dp(20),
                                               g.panel, reinterpret_cast<HMENU>(
                                                   static_cast<INT_PTR>(check_id)),
                                               g.instance, nullptr);
                set_font(ui_row.check, stock_font());
                g.panel_items.push_back(
                    PanelItem{ui_row.check, dp(kRowEditX), y + dp(4), dp(22), dp(20)});
                bool checked = false;
                const auto parsed = vmt::parse_bool(row.value);
                if (parsed) {
                    checked = parsed.value();
                }
                SendMessageW(ui_row.check, BM_SETCHECK,
                             checked ? BST_CHECKED : BST_UNCHECKED, 0);
            } else if (row.type == vmt::ParamType::Enum) {
                ui_row.combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                                                   WS_VSCROLL,
                                               dp(kRowEditX), y + dp(3),
                                               content_width - dp(kRowEditX) + dp(8),
                                               dp(200),
                                               g.panel, reinterpret_cast<HMENU>(
                                                   static_cast<INT_PTR>(combo_id)),
                                               g.instance, nullptr);
                set_font(ui_row.combo, stock_font());
                g.panel_items.push_back(
                    PanelItem{ui_row.combo, dp(kRowEditX), y + dp(3),
                              content_width - dp(kRowEditX) + dp(8), dp(24)});
                for (const std::string& value : row.enum_values) {
                    SendMessageW(ui_row.combo, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(to_wide(value).c_str()));
                }
                int selection = -1;
                for (std::size_t i = 0; i < row.enum_values.size(); ++i) {
                    if (row.enum_values[i] == row.value) {
                        selection = static_cast<int>(i);
                        break;
                    }
                }
                SendMessageW(ui_row.combo, CB_SETCURSEL, selection, 0);
            } else {
                const bool numeric = row.type == vmt::ParamType::Int ||
                                     row.type == vmt::ParamType::Float;
                ui_row.is_float = row.type == vmt::ParamType::Float;

                if (numeric) {
                    ui_row.edit = CreateWindowExW(0, L"EDIT", to_wide(row.value).c_str(),
                                                  WS_CHILD | WS_VISIBLE | WS_BORDER |
                                                      ES_AUTOHSCROLL,
                                                  dp(kRowEditX), y + dp(3), dp(kRowEditWidth),
                                                  dp(22),
                                                  g.panel, reinterpret_cast<HMENU>(
                                                      static_cast<INT_PTR>(edit_id)),
                                                  g.instance, nullptr);
                    set_font(ui_row.edit, stock_font());
                    g.panel_items.push_back(
                        PanelItem{ui_row.edit, dp(kRowEditX), y + dp(3), dp(kRowEditWidth),
                                  dp(22)});

                    double current = 0.0;
                    bool have_current = false;
                    if (ui_row.is_float) {
                        const auto parsed = vmt::parse_float(row.value);
                        if (parsed) {
                            current = parsed.value();
                            have_current = true;
                        }
                    } else {
                        const auto parsed = vmt::parse_int(row.value);
                        if (parsed) {
                            current = parsed.value();
                            have_current = true;
                        }
                    }
                    if (!have_current && row.has_default) {
                        if (ui_row.is_float) {
                            const auto parsed = vmt::parse_float(row.default_value);
                            if (parsed) {
                                current = parsed.value();
                                have_current = true;
                            }
                        } else {
                            const auto parsed = vmt::parse_int(row.default_value);
                            if (parsed) {
                                current = parsed.value();
                                have_current = true;
                            }
                        }
                    }

                    const vmt::SchemaParam* schema_param = schema.find_param(row.name);
                    compute_slider_range(row.type, schema_param, current,
                                         ui_row.range_lo, ui_row.range_hi);

                    const int slider_x = dp(kRowEditX) + dp(kRowEditWidth) + dp(8);
                    const int slider_width = std::max(dp(40), content_width - slider_x + dp(8));
                    ui_row.slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                                    WS_CHILD | WS_VISIBLE | TBS_HORZ |
                                                        TBS_NOTICKS,
                                                    slider_x, y + dp(5), slider_width, dp(20),
                                                    g.panel,
                                                    reinterpret_cast<HMENU>(
                                                        static_cast<INT_PTR>(slider_id)),
                                                    g.instance, nullptr);
                    g.panel_items.push_back(
                        PanelItem{ui_row.slider, slider_x, y + dp(5), slider_width, dp(20)});

                    const int lo_int = ui_row.is_float
                                           ? 0
                                           : static_cast<int>(std::floor(ui_row.range_lo));
                    const int hi_int = ui_row.is_float
                                           ? 1000
                                           : static_cast<int>(std::ceil(ui_row.range_hi));
                    SendMessageW(ui_row.slider, TBM_SETRANGEMIN, FALSE,
                                 static_cast<LPARAM>(lo_int));
                    SendMessageW(ui_row.slider, TBM_SETRANGEMAX, FALSE,
                                 static_cast<LPARAM>(hi_int));
                    SendMessageW(ui_row.slider, TBM_SETPOS, TRUE,
                                 static_cast<LPARAM>(
                                     slider_position(current, ui_row.range_lo,
                                                     ui_row.range_hi, ui_row.is_float)));
                } else {
                    ui_row.edit = CreateWindowExW(0, L"EDIT", to_wide(row.value).c_str(),
                                                  WS_CHILD | WS_VISIBLE | WS_BORDER |
                                                      ES_AUTOHSCROLL,
                                                  dp(kRowEditX), y + dp(3),
                                                  content_width - dp(kRowEditX) + dp(8), dp(22),
                                                  g.panel, reinterpret_cast<HMENU>(
                                                      static_cast<INT_PTR>(edit_id)),
                                                  g.instance, nullptr);
                    set_font(ui_row.edit, stock_font());
                    g.panel_items.push_back(
                        PanelItem{ui_row.edit, dp(kRowEditX), y + dp(3),
                                  content_width - dp(kRowEditX) + dp(8), dp(22)});
                }
            }

            g.rows.push_back(std::move(ui_row));
            y += dp(kRowHeight);
        }
        y += dp(8);
    }

    g.content_height = y + dp(6);
    if (g.content_height < 1) {
        g.content_height = 1;
    }
    update_panel_scrollbar();
    set_panel_scroll(std::min(g.scroll_y, std::max(0, g.content_height - static_cast<int>(rc.bottom))));
    g.rebuilding = false;
}

// --- Session -> UI -----------------------------------------------------------

void refresh_text_control() {
    if (GetFocus() == g.text_edit) {
        return; // never fight the user's caret
    }
    const std::wstring text = make_control_text(g.session.text(), g.encoding);
    g.suppress_text = true;
    SetWindowTextW(g.text_edit, text.c_str());
    g.suppress_text = false;
}

void refresh_recent_list() {
    SendMessageW(g.recent_list, LB_RESETCONTENT, 0, 0);
    for (const std::string& recent : g.settings.recent_files) {
        SendMessageW(g.recent_list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(to_wide(recent).c_str()));
    }
}

void save_settings() {
    if (!g.settings_path.empty()) {
        options_to_settings(g.preview_options, g.settings);
        g.settings.save(g.settings_path);
    }
}

UINT shape_menu_id(ui::PreviewShape shape) {
    switch (shape) {
    case ui::PreviewShape::Sphere:
        return IDM_SHAPE_SPHERE;
    case ui::PreviewShape::Cube:
        return IDM_SHAPE_CUBE;
    case ui::PreviewShape::Cylinder:
        return IDM_SHAPE_CYLINDER;
    case ui::PreviewShape::Plane:
        break;
    }
    return IDM_SHAPE_PLANE;
}

void sync_shape_menu() {
    HMENU menu = GetMenu(g.main);
    if (menu != nullptr) {
        CheckMenuRadioItem(menu, IDM_SHAPE_SPHERE, IDM_SHAPE_PLANE,
                           shape_menu_id(g.preview_options.shape), MF_BYCOMMAND);
    }
}

// --- Engine preview ----------------------------------------------------------

std::filesystem::path preview_root_dir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer) / L"VMTViewer" / L"preview";
}

// %LOCALAPPDATA%\VMTViewer (falls back to %TEMP%\VMTViewer): the home of the
// viewer log, the session marker and the thumbnail cache.
std::filesystem::path local_data_dir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        wchar_t temp[MAX_PATH] = {};
        const DWORD temp_length = GetTempPathW(MAX_PATH, temp);
        if (temp_length == 0 || temp_length >= MAX_PATH) {
            return {};
        }
        return std::filesystem::path(temp) / L"VMTViewer";
    }
    return std::filesystem::path(buffer) / L"VMTViewer";
}

std::filesystem::path thumbnail_root_dir() {
    return local_data_dir() / L"thumbnails";
}

bool write_preview_file(const std::string& text) {
    if (g.preview_root.empty()) {
        return false;
    }
    return ui::write_preview_material_at(g.preview_root, g.preview_material_name, text).ok();
}

bridge::PreviewShape to_bridge_shape(ui::PreviewShape shape) {
    switch (shape) {
    case ui::PreviewShape::Sphere:
        return bridge::PreviewShape::Sphere;
    case ui::PreviewShape::Cube:
        return bridge::PreviewShape::Cube;
    case ui::PreviewShape::Cylinder:
        return bridge::PreviewShape::Cylinder;
    case ui::PreviewShape::Plane:
        break;
    }
    return bridge::PreviewShape::Plane;
}

bridge::PreviewLighting to_bridge_lighting(const ui::PreviewLighting& lighting) {
    bridge::PreviewLighting out;
    for (int i = 0; i < 3; ++i) {
        out.ambient[i] = lighting.ambient[i];
        out.key_dir[i] = lighting.key_dir[i];
        out.key_color[i] = lighting.key_color[i];
        out.fill_dir[i] = lighting.fill_dir[i];
        out.fill_color[i] = lighting.fill_color[i];
        out.background[i] = lighting.background[i];
    }
    return out;
}

// Pushes shape + lighting into the running engine (no-op before it starts;
// start_preview_impl / ensure_engine_running apply them right after init).
void apply_preview_to_engine() {
    if (!g.engine || !g.engine->initialized()) {
        return;
    }
    (void)g.engine->set_preview_shape(to_bridge_shape(g.preview_options.shape));
    (void)g.engine->set_lighting(to_bridge_lighting(g.preview_options.lighting));
}

// The material name the engine should preview for the current session:
//  - a material opened from the browser keeps its game-relative name,
//  - a loose file below <game>/materials derives its name from the path,
//  - anything else previews as the dedicated vmtviewer_preview material.
std::string preview_name_for_file(const std::filesystem::path& path) {
    if (path.empty() || g.settings.last_game_dir.empty()) {
        return ui::kPreviewMaterialName;
    }
    std::error_code ec;
    const std::filesystem::path materials = std::filesystem::weakly_canonical(
        std::filesystem::path(g.settings.last_game_dir) / L"materials", ec);
    if (ec) {
        return ui::kPreviewMaterialName;
    }
    ec.clear();
    const std::filesystem::path file = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return ui::kPreviewMaterialName;
    }
    ec.clear();
    const std::filesystem::path relative = std::filesystem::relative(file, materials, ec);
    if (ec || relative.empty()) {
        return ui::kPreviewMaterialName;
    }
    // relative() returns the input unchanged when the two paths have no common
    // root (a file on another drive, for example), which is not a material
    // path at all.
    if (relative.is_absolute() || relative.has_root_name()) {
        return ui::kPreviewMaterialName;
    }
    const std::string generic = relative.generic_string();
    if (generic.rfind("..", 0) == 0) {
        return ui::kPreviewMaterialName;
    }
    const std::string name = ui::material_name_from_game_path(generic);
    if (name.empty() || name.find_first_of(":*?\"<>|") != std::string::npos) {
        // Not representable as a preview override file (or as an engine
        // material name); fall back to the dedicated preview material.
        return ui::kPreviewMaterialName;
    }
    return name;
}

// Connects the debounced sync controller to the file writer and the engine.
// Called once the main window exists; the engine hook is lazily resolved so
// the hooks survive engine restarts.
void configure_preview_sync() {
    g.preview_sync.configure(
        [](const std::string& text) { return write_preview_file(text); },
        []() { return g.engine != nullptr && g.engine->reload_material().ok(); });
}

// Records an edit in the sync controller and (re)arms or cancels the debounce
// timer. Invalid text never schedules a write: the last good render stays and
// the status bar keeps showing the parse error.
void note_preview_change() {
    if (!g.preview_sync.active()) {
        return;
    }
    const bool pending =
        g.preview_sync.document_changed(g.session.text_is_valid(), g.session.last_error());
    if (pending) {
        SetTimer(g.main, kPreviewSyncTimer, kPreviewSyncDelayMs, nullptr);
    } else {
        KillTimer(g.main, kPreviewSyncTimer);
    }
}

void stop_engine_preview() {
    KillTimer(g.main, kTickerTimer);
    KillTimer(g.main, kPreviewSyncTimer);
    g.preview_sync.set_active(false);
    if (g.engine) {
        g.engine->shutdown();
    }
    g.preview_active = false;
    g.preview_mounted = false;
    g.engine_material_name.clear();
    g.engine_active_key.clear();
    g.engine_note.clear();
}

bool engine_probe_passes(const std::filesystem::path& bin_dir,
                         const std::filesystem::path& game_dir) {
    wchar_t exe[4 * MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, 4 * MAX_PATH) == 0) {
        return false;
    }
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --engine-probe \"" +
                           bin_dir.wstring() + L"\" \"" + game_dir.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &process) == FALSE) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 120000);
    DWORD exit_code = 1;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exit_code);
    } else {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exit_code == 0;
}

void preview_report_failure(bool interactive, const std::wstring& note,
                            const wchar_t* box_text) {
    g.engine_note = note;
    if (interactive && box_text != nullptr) {
        MessageBoxW(g.main, box_text, kWindowTitle, MB_ICONWARNING | MB_OK);
    }
    update_status();
}

// Starts the engine when it is not running yet: connection check (and offer to
// open the dialog in interactive flows), the disposable compatibility probe,
// device creation and the preview search-path mount. On success the preview
// root is mounted at the highest priority so edited copies override the game's
// own files. `interactive` selects whether a missing connection pops a dialog
// and whether failures are returned in `error` for the caller's message box.
bool ensure_engine_running(std::wstring& error, bool interactive) {
    if (g.engine && g.engine->initialized()) {
        apply_preview_to_engine();
        return true;
    }

    if (g.settings.last_bin_dir.empty() || g.settings.last_game_dir.empty()) {
        if (interactive) {
            const int answer = MessageBoxW(
                g.main,
                L"The game connection is not configured yet (Game > Connection).\n\nOpen it now?",
                kWindowTitle, MB_ICONQUESTION | MB_YESNO);
            if (answer == IDYES) {
                show_connection_dialog(g.main, g.settings, g.settings_path);
            }
        }
        if (g.settings.last_bin_dir.empty() || g.settings.last_game_dir.empty()) {
            error = L"preview: configure Game > Connection to preview materials";
            return false;
        }
    }

    // Fix a stale game folder (typically the game root instead of the mod
    // folder that actually contains gameinfo.txt).
    {
        std::error_code ec;
        const std::filesystem::path game_path = std::filesystem::path(g.settings.last_game_dir);
        if (!std::filesystem::is_regular_file(game_path / L"gameinfo.txt", ec)) {
            const std::filesystem::path detected =
                detect_game_dir(std::filesystem::path(g.settings.last_bin_dir));
            if (!detected.empty()) {
                g.settings.last_game_dir = detected.string();
                save_settings();
            }
        }
    }

    // The engine binds to one game for its lifetime; when the connection now
    // points at a different /bin + game pair, shut it down so the code below
    // loads the new game instead of silently previewing with the old one.
    const std::string probe_key = g.settings.last_bin_dir + "|" + g.settings.last_game_dir;
    if (g.engine && g.engine->initialized() && g.engine_active_key != probe_key) {
        log_write("ENG ", "game connection changed; restarting the engine for " + probe_key);
        stop_engine_preview();
    }

    // Compatibility gate: run the engine once in a disposable child process.
    // Faulty game installs (e.g. the current Garry's Mod binaries fault in
    // their own filesystem DLL while mounting) then only kill that child. The
    // outcome is cached per game for the session.
    if (probe_key != g.engine_probe_key) {
        bool probe_ok = false;
        if (probe_key == g.engine_probe_failed_key) {
            probe_ok = false;
        } else if (engine_probe_passes(std::filesystem::path(g.settings.last_bin_dir),
                                       std::filesystem::path(g.settings.last_game_dir))) {
            probe_ok = true;
            g.engine_probe_key = probe_key;
        } else {
            g.engine_probe_failed_key = probe_key;
        }
        if (!probe_ok) {
            log_write("ENG ", "compatibility probe failed for " + probe_key);
            error =
                L"The engine from this game install crashed or failed the compatibility "
                L"check, so the preview was not started. The viewer itself is unaffected.\n\n"
                L"Details: %TEMP%\\VMTViewer\\engine.log\n\n"
                L"Tip: pick the game folder that contains gameinfo.txt (for example "
                L"...\\garrysmod, ...\\hl2 or ...\\tf) in Game > Connection and use "
                L"Test connection.";
            return false;
        }
    }

    if (g.preview_root.empty()) {
        g.preview_root = preview_root_dir();
    }
    if (g.preview_root.empty()) {
        error = L"preview: the preview folder is not available";
        return false;
    }
    {
        std::error_code ec;
        std::filesystem::create_directories(g.preview_root, ec);
    }

    if (!g.engine) {
        vmt::Result<std::unique_ptr<bridge::EngineView>> created = bridge::EngineView::create();
        if (!created) {
            error = L"preview: the engine could not be created: " +
                    to_wide(created.error().message);
            return false;
        }
        g.engine = std::move(created.value());
        g.preview_mounted = false;
        g.preview_active = false;
        g.engine_material_name.clear();
    }

    if (!g.engine->initialized()) {
        RECT rc{};
        GetClientRect(g.viewport, &rc);
        bridge::EngineViewConfig config;
        config.bin_dir = std::filesystem::path(g.settings.last_bin_dir);
        config.game_dir = std::filesystem::path(g.settings.last_game_dir);
        config.child_window = g.viewport;
        // The device backbuffer is created once and never resized (SetMode
        // after shaders were used faults in the 2013 binaries). Give it
        // headroom up to the desktop size so later window growth mostly
        // downscales instead of upscaling a small backbuffer.
        const int screen_w = GetSystemMetrics(SM_CXSCREEN);
        const int screen_h = GetSystemMetrics(SM_CYSCREEN);
        const int max_w = std::max(1280, std::min(screen_w, 1920));
        const int max_h = std::max(720, std::min(screen_h, 1080));
        config.width = std::min(std::max(static_cast<int>(rc.right), 1280), max_w);
        config.height = std::min(std::max(static_cast<int>(rc.bottom), 720), max_h);
        const vmt::Result<void> init = g.engine->initialize(config);
        if (!init) {
            g.engine.reset();
            log_write("ENG ", "engine initialization failed for " + probe_key + ": " +
                                  init.error().message);
            error = L"preview: engine initialization failed (see engine.log): " +
                    to_wide(init.error().message);
            if (!init.error().remediation.empty()) {
                error += L"\n\n" + to_wide(init.error().remediation);
            }
            return false;
        }
        log_write("ENG ", "engine initialized for " + probe_key);
        g.engine_active_key = probe_key;
        g.preview_mounted = false;
        g.engine_material_name.clear();
    }

    if (!g.preview_mounted) {
        const vmt::Result<void> mounted = g.engine->mount_directory(g.preview_root, true);
        if (!mounted) {
            error = L"preview: the preview search path could not be mounted: " +
                    to_wide(mounted.error().message);
            stop_engine_preview();
            return false;
        }
        g.preview_mounted = true;
    }

    apply_preview_to_engine();
    return true;
}

// Shared preview start/refresh. `interactive` is true for the Preview button
// (dialogs, offers to open the connection dialog) and false for the automatic
// path used when a material is opened (status-bar notes only).
bool start_preview_impl(bool interactive) {
    if (!g.session.has_document()) {
        if (interactive) {
            MessageBoxW(g.main, L"Open a VMT material first.", kWindowTitle,
                        MB_ICONINFORMATION | MB_OK);
        }
        return false;
    }
    if (!g.session.text_is_valid()) {
        // Never push text that does not parse: the mounted preview file still
        // holds the last good revision and the viewport keeps rendering it.
        g.engine_note = L"preview: fix the syntax error first (last good render kept)";
        if (interactive) {
            show_error_box(g.session.last_error());
        }
        update_status();
        return false;
    }

    g.preview_root = preview_root_dir();
    if (g.preview_root.empty() || !write_preview_file(g.session.text())) {
        preview_report_failure(interactive,
                               L"preview: the preview folder could not be written",
                               L"The preview folder could not be written.");
        return false;
    }

    std::wstring error;
    if (!ensure_engine_running(error, interactive)) {
        // The status bar shows the first line only; multi-line detail (engine
        // errors with remediation) goes into the dialog and the log.
        std::wstring note = error.empty() ? std::wstring(L"preview unavailable") : error;
        const std::size_t newline = note.find(L'\n');
        if (newline != std::wstring::npos) {
            note.resize(newline);
        }
        g.engine_note = note;
        if (interactive && !error.empty()) {
            MessageBoxW(g.main, error.c_str(), kWindowTitle, MB_ICONWARNING | MB_OK);
        }
        update_status();
        return false;
    }

    const bool name_changed = g.engine_material_name != g.preview_material_name;
    vmt::Result<void> applied = vmt::Ok();
    if (name_changed || !g.preview_active) {
        applied = g.engine->set_material(g.preview_material_name);
        if (applied) {
            g.engine_material_name = g.preview_material_name;
        }
    } else {
        applied = g.engine->reload_material();
    }
    if (!applied) {
        preview_report_failure(interactive, L"preview: the material could not be loaded (see engine.log)",
                               nullptr);
        if (interactive) {
            show_error_box(applied.error());
        }
        return false;
    }

    g.preview_active = true;
    g.preview_sync.set_active(true);
    g.preview_sync.note_previewed(g.session.text());
    KillTimer(g.main, kPreviewSyncTimer);
    g.engine_note = L"engine preview active (" + to_wide(g.preview_material_name) + L")";
    log_write("ENG ", "previewing material '" + g.preview_material_name + "'");
    SetTimer(g.main, kTickerTimer, kTickerIntervalMs, nullptr);
    InvalidateRect(g.viewport, nullptr, TRUE);
    update_status();
    return true;
}

void start_preview() {
    (void)start_preview_impl(true);
}

void refresh_preview_after_save() {
    if (!g.preview_sync.active() || !g.engine || !g.engine->initialized()) {
        return;
    }
    // Saving flushes immediately instead of waiting for the debounce.
    KillTimer(g.main, kPreviewSyncTimer);
    const vmt::Result<void> flushed = g.preview_sync.flush_now(
        g.session.text(), g.session.text_is_valid(), g.session.last_error());
    if (flushed) {
        g.engine_note = L"engine preview active (" +
                        to_wide(std::string(ui::kPreviewMaterialName)) + L")";
    } else {
        g.engine_note = L"preview: " + to_wide(flushed.error().message);
    }
}

// --- Validation (Phase 8) ----------------------------------------------------

const wchar_t* severity_label(ui::Severity severity) {
    switch (severity) {
    case ui::Severity::Error:
        return L"error";
    case ui::Severity::Warning:
        return L"warning";
    case ui::Severity::Info:
        break;
    }
    return L"info";
}

std::wstring format_finding(const ui::Finding& finding) {
    std::wstring row = std::wstring(L"[") + severity_label(finding.severity) + L"]";
    if (finding.line != 0) {
        row += L" line " + std::to_wstring(finding.line);
    }
    row += L": ";
    row += to_wide(finding.message);
    return row;
}

// Moves the caret to the finding's line and selects it.
void jump_to_finding(std::size_t index) {
    if (index >= g.findings.size()) {
        return;
    }
    const ui::Finding& finding = g.findings[index];
    if (finding.line == 0) {
        return;
    }
    const LRESULT start = SendMessageW(g.text_edit, EM_LINEINDEX,
                                       static_cast<WPARAM>(finding.line - 1), 0);
    if (start < 0) {
        return;
    }
    LRESULT length = SendMessageW(g.text_edit, EM_LINELENGTH, static_cast<WPARAM>(start), 0);
    if (length < 0) {
        length = 0;
    }
    SendMessageW(g.text_edit, EM_SETSEL, static_cast<WPARAM>(start),
                 static_cast<LPARAM>(start + length));
    SendMessageW(g.text_edit, EM_SCROLLCARET, 0, 0);
    SetFocus(g.text_edit);
}

void refresh_validation_results() {
    if (g.validation_window == nullptr) {
        g.validation_window = CreateWindowExW(
            WS_EX_TOOLWINDOW, kValidationClass, L"Validation results", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, dp(760), dp(360), g.main, nullptr, g.instance,
            nullptr);
        if (g.validation_window == nullptr) {
            return;
        }
        g.validation_list = CreateWindowExW(
            0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, g.validation_window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), g.instance, nullptr);
        set_font(g.validation_list, stock_font());
    }
    SendMessageW(g.validation_list, LB_RESETCONTENT, 0, 0);
    if (g.findings.empty()) {
        SendMessageW(g.validation_list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"[ok] no problems found"));
    } else {
        for (const ui::Finding& finding : g.findings) {
            const std::wstring row = format_finding(finding);
            SendMessageW(g.validation_list, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(row.c_str()));
        }
    }
    ShowWindow(g.validation_window, SW_SHOW);
    SetForegroundWindow(g.validation_window);
}

void run_validation() {
    if (!g.session.has_document()) {
        MessageBoxW(g.main, L"Open a VMT material first.", kWindowTitle,
                    MB_ICONINFORMATION | MB_OK);
        return;
    }

    std::vector<ui::Finding> findings;
    if (!g.session.text_is_valid()) {
        const vmt::Error& error = g.session.last_error();
        findings.push_back(ui::Finding{ui::Severity::Error, "parse-error", error.message,
                                       error.line, error.column});
    } else {
        ui::ValidationOptions options;
        options.schemas = g.schemas_loaded ? &g.schemas : nullptr;
        if (g.engine && g.engine->initialized()) {
            options.resolve_texture = [](const std::string& name) {
                const vmt::Result<bool> exists =
                    g.engine->file_exists("materials/" + name + ".vtf");
                if (!exists) {
                    return ui::TextureStatus::Unavailable;
                }
                return exists.value() ? ui::TextureStatus::Found
                                      : ui::TextureStatus::Missing;
            };
        }
        findings = ui::validate_document(*g.session.document(), options);
    }

    int errors = 0;
    int warnings = 0;
    int infos = 0;
    for (const ui::Finding& finding : findings) {
        switch (finding.severity) {
        case ui::Severity::Error:
            ++errors;
            break;
        case ui::Severity::Warning:
            ++warnings;
            break;
        case ui::Severity::Info:
            ++infos;
            break;
        }
    }
    g.validation_summary = L"validation: " + std::to_wstring(errors) + L" error(s), " +
                           std::to_wstring(warnings) + L" warning(s), " +
                           std::to_wstring(infos) + L" info";
    g.findings = std::move(findings);
    refresh_validation_results();
    update_status();
}

void clear_validation_summary() {
    g.validation_summary.clear();
}

void apply_panel_value(std::size_t index, const std::string& value) {
    if (g.rebuilding || index >= g.rows.size()) {
        return;
    }
    const vmt::Result<void> result = g.session.set_parameter(g.rows[index].name, value);
    if (!result) {
        update_status();
        return;
    }
    refresh_text_control();
    note_preview_change();
    clear_validation_summary();
    update_title();
    update_status();
}

bool open_path(const std::filesystem::path& path) {
    const vmt::Result<void> result = g.session.open_file(path);
    if (!result) {
        show_error_box(result.error());
        update_status();
        return false;
    }
    g.preview_material_name = preview_name_for_file(path);
    log_write("FILE", "opened " + path.string() + " (preview name '" +
                          g.preview_material_name + "')");
    KillTimer(g.main, kPanelRebuildTimer);
    refresh_text_control();
    rebuild_panel();
    clear_validation_summary();
    g.settings.remember_file(path.string());
    refresh_recent_list();
    save_settings();
    update_title();
    update_status();

    // Auto-preview: once the game connection exists, opening a material starts
    // (or refreshes) the engine view without pressing Preview material.
    if (!g.settings.last_bin_dir.empty() && !g.settings.last_game_dir.empty()) {
        (void)start_preview_impl(false);
    }
    return true;
}

void do_open_dialog() {
    wchar_t buffer[4 * MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.main;
    ofn.lpstrFilter = L"VMT materials (*.vmt)\0*.vmt\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = 4 * MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Open VMT material";
    if (GetOpenFileNameW(&ofn) != FALSE) {
        open_path(std::filesystem::path(buffer));
    }
}

void do_save(bool save_as) {
    if (!g.session.has_document() && !g.session.text_is_valid()) {
        MessageBoxW(g.main, L"There is nothing to save yet.", kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }

    std::filesystem::path target = g.session.path();
    if (save_as || target.empty()) {
        wchar_t buffer[4 * MAX_PATH] = {};
        if (!target.empty()) {
            lstrcpynW(buffer, target.wstring().c_str(), 4 * MAX_PATH);
        } else if (g.session.has_document()) {
            // The document name may be a game-relative path ("brick/foo.vmt");
            // only the file name belongs in the dialog's file field.
            const std::filesystem::path suggested(
                std::filesystem::path(g.session.document()->name()).filename());
            lstrcpynW(buffer, suggested.wstring().c_str(), 4 * MAX_PATH);
        }
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g.main;
        ofn.lpstrFilter = L"VMT materials (*.vmt)\0*.vmt\0All files (*.*)\0*.*\0\0";
        ofn.lpstrFile = buffer;
        ofn.nMaxFile = 4 * MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        ofn.lpstrDefExt = L"vmt";
        ofn.lpstrTitle = L"Save VMT material";
        if (GetSaveFileNameW(&ofn) == FALSE) {
            return;
        }
        target = std::filesystem::path(buffer);
    }

    const vmt::Result<void> result = save_as ? g.session.save_as(target) : g.session.save();
    if (!result) {
        show_error_box(result.error());
        update_status();
        return;
    }
    g.settings.remember_file(target.string());
    refresh_recent_list();
    save_settings();
    refresh_preview_after_save();
    update_title();
    update_status();
}

// --- Layout ------------------------------------------------------------------

void layout() {
    if (g.panel == nullptr || g.text_edit == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(g.main, &client);
    const int width = static_cast<int>(client.right);
    const int height = static_cast<int>(client.bottom);

    SendMessageW(g.status, WM_SIZE, 0, 0);
    RECT status_rect{};
    GetWindowRect(g.status, &status_rect);
    const int status_height = static_cast<int>(status_rect.bottom - status_rect.top);
    const int content_height = std::max(0, height - status_height);

    if (width <= 0 || content_height <= 0) {
        return;
    }

    const int left_width = dp(kLeftWidth);
    const int right_width = dp(kRightWidth);
    const int bottom_height = dp(kBottomHeight);
    const int center_width = std::max(dp(50), width - left_width - right_width);
    const int viewport_height = std::max(dp(50), content_height - bottom_height);

    MoveWindow(g.open_button, dp(8), dp(8), left_width - dp(16), dp(26), TRUE);
    MoveWindow(g.save_button, dp(8), dp(40), (left_width - dp(24)) / 2, dp(26), TRUE);
    MoveWindow(g.save_as_button, dp(12) + (left_width - dp(24)) / 2, dp(40),
               (left_width - dp(24)) / 2, dp(26), TRUE);
    MoveWindow(g.preview_button, dp(8), dp(72), left_width - dp(16), dp(26), TRUE);
    MoveWindow(g.validate_button, dp(8), dp(104), left_width - dp(16), dp(26), TRUE);
    MoveWindow(g.recent_label, dp(8), dp(138), left_width - dp(16), dp(18), TRUE);
    MoveWindow(g.recent_list, dp(8), dp(158), left_width - dp(16),
               std::max(dp(40), content_height - dp(166)), TRUE);

    MoveWindow(g.viewport, left_width, 0, center_width, viewport_height, TRUE);
    MoveWindow(g.text_edit, left_width, viewport_height, center_width,
               std::max(dp(40), content_height - viewport_height), TRUE);

    MoveWindow(g.advanced_check, left_width + center_width + dp(8), dp(6),
               right_width - dp(16), dp(22), TRUE);
    MoveWindow(g.panel, left_width + center_width, dp(30), right_width,
               std::max(dp(40), content_height - dp(30)), TRUE);

    update_panel_scrollbar();
    set_panel_scroll(g.scroll_y);
}

// --- Window procedures -------------------------------------------------------

void on_panel_command(WORD id, WORD code, HWND control) {
    if (id >= ID_ROW_EDIT_BASE && id < ID_ROW_EDIT_BASE + 1000) {
        const std::size_t index = static_cast<std::size_t>(id - ID_ROW_EDIT_BASE);
        if (code == EN_CHANGE) {
            apply_panel_value(index, to_utf8(window_text(control)));
        } else if (code == EN_SETFOCUS && index < g.rows.size()) {
            g.status_hint = to_wide(g.rows[index].description);
            update_status();
        } else if (code == EN_KILLFOCUS) {
            g.status_hint.clear();
            update_status();
        }
        return;
    }
    if (id >= ID_ROW_CHECK_BASE && id < ID_ROW_CHECK_BASE + 1000) {
        const std::size_t index = static_cast<std::size_t>(id - ID_ROW_CHECK_BASE);
        if (code == BN_CLICKED) {
            const LRESULT checked = SendMessageW(control, BM_GETCHECK, 0, 0);
            apply_panel_value(index, checked == BST_CHECKED ? "1" : "0");
        }
        return;
    }
    if (id >= ID_ROW_COMBO_BASE && id < ID_ROW_COMBO_BASE + 1000) {
        const std::size_t index = static_cast<std::size_t>(id - ID_ROW_COMBO_BASE);
        if (code == CBN_SELCHANGE && index < g.rows.size()) {
            const LRESULT selection = SendMessageW(control, CB_GETCURSEL, 0, 0);
            if (selection != CB_ERR) {
                wchar_t value[256] = {};
                SendMessageW(control, CB_GETLBTEXT, selection,
                             reinterpret_cast<LPARAM>(value));
                apply_panel_value(index, to_utf8(value));
            }
        }
        return;
    }
}

void on_panel_hscroll(HWND slider) {
    const int id = GetDlgCtrlID(slider);
    if (id < ID_ROW_SLIDER_BASE || id >= ID_ROW_SLIDER_BASE + 1000) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(id - ID_ROW_SLIDER_BASE);
    if (index >= g.rows.size()) {
        return;
    }
    PanelRowUi& row = g.rows[index];
    const LRESULT position = SendMessageW(slider, TBM_GETPOS, 0, 0);
    const double value =
        slider_value(static_cast<int>(position), row.range_lo, row.range_hi, row.is_float);
    const std::string text = format_number(row.type, value);
    g.rebuilding = true;
    if (row.edit != nullptr) {
        SetWindowTextW(row.edit, to_wide(text).c_str());
    }
    g.rebuilding = false;
    apply_panel_value(index, text);
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_VSCROLL: {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int y = g.scroll_y;
        switch (LOWORD(wparam)) {
        case SB_LINEUP: y -= dp(24); break;
        case SB_LINEDOWN: y += dp(24); break;
        case SB_PAGEUP: y -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: y += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK: y = si.nTrackPos; break;
        default: break;
        }
        set_panel_scroll(y);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        set_panel_scroll(g.scroll_y - (delta / WHEEL_DELTA) * dp(48));
        return 0;
    }
    case WM_COMMAND:
    case WM_HSCROLL:
        // Controls live in this panel; route their notifications to the app
        // window handler.
        if (g.main != nullptr) {
            return SendMessageW(g.main, message, wparam, lparam);
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK ViewportProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wparam), &rc,
                 static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (!g.engine || !g.engine->initialized()) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(200, 200, 200));
            DrawTextW(dc,
                      L"Engine viewport\n\nConfigure Game > Connection, then press "
                      L"\"Preview material\".",
                      -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            g.pending_viewport_width = LOWORD(lparam);
            g.pending_viewport_height = HIWORD(lparam);
            SetTimer(hwnd, kViewportResizeTimer, kViewportResizeDelayMs, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (wparam == kViewportResizeTimer) {
            KillTimer(hwnd, kViewportResizeTimer);
            if (g.engine && g.engine->initialized() && g.pending_viewport_width > 0 &&
                g.pending_viewport_height > 0) {
                (void)g.engine->resize(g.pending_viewport_width, g.pending_viewport_height);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

LRESULT CALLBACK ValidationProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        if (g.validation_list != nullptr) {
            MoveWindow(g.validation_list, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == 1 && HIWORD(wparam) == LBN_DBLCLK) {
            const LRESULT selection = SendMessageW(g.validation_list, LB_GETCURSEL, 0, 0);
            if (selection != LB_ERR) {
                jump_to_finding(static_cast<std::size_t>(selection));
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g.validation_window = nullptr;
        g.validation_list = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = dp(900);
        info->ptMinTrackSize.y = dp(600);
        return 0;
    }

    case WM_COMMAND: {
        const WORD id = LOWORD(wparam);
        const WORD code = HIWORD(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);

        switch (id) {
        case ID_OPEN: do_open_dialog(); return 0;
        case ID_SAVE: do_save(false); return 0;
        case ID_SAVE_AS: do_save(true); return 0;
        case IDM_EXIT: PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case IDM_ABOUT:
            show_about_dialog(hwnd);
            return 0;
        case IDM_GAME_CONNECTION:
            show_connection_dialog(hwnd, g.settings, g.settings_path);
            return 0;
        case IDM_BROWSER:
            show_material_browser(hwnd);
            return 0;
        case IDM_PREVIEW_OPTIONS:
            show_preview_options_dialog(hwnd);
            sync_shape_menu(); // the dialog may have changed the shape
            return 0;
        case IDM_MODEL:
            app::pick_and_load_model(hwnd);
            return 0;
        case IDM_SHAPE_SPHERE:
        case IDM_SHAPE_CUBE:
        case IDM_SHAPE_CYLINDER:
        case IDM_SHAPE_PLANE: {
            ui::PreviewOptions options = g.preview_options;
            switch (id) {
            case IDM_SHAPE_SPHERE:
                options.shape = ui::PreviewShape::Sphere;
                break;
            case IDM_SHAPE_CUBE:
                options.shape = ui::PreviewShape::Cube;
                break;
            case IDM_SHAPE_CYLINDER:
                options.shape = ui::PreviewShape::Cylinder;
                break;
            default:
                options.shape = ui::PreviewShape::Plane;
                break;
            }
            app::apply_preview_options(options);
            app::commit_preview_options();
            sync_shape_menu();
            return 0;
        }
        case ID_PREVIEW:
            start_preview();
            return 0;
        case ID_VALIDATE:
            run_validation();
            return 0;
        case ID_ADVANCED: {
            g.settings.show_advanced = !g.settings.show_advanced;
            CheckMenuItem(GetMenu(hwnd), ID_ADVANCED,
                          MF_BYCOMMAND |
                              (g.settings.show_advanced ? MF_CHECKED : MF_UNCHECKED));
            SendMessageW(g.advanced_check, BM_SETCHECK,
                         g.settings.show_advanced ? BST_CHECKED : BST_UNCHECKED, 0);
            rebuild_panel();
            save_settings();
            return 0;
        }
        case ID_RECENT:
            if (code == LBN_DBLCLK) {
                const LRESULT selection = SendMessageW(g.recent_list, LB_GETCURSEL, 0, 0);
                if (selection != LB_ERR) {
                    wchar_t buffer[4 * MAX_PATH] = {};
                    SendMessageW(g.recent_list, LB_GETTEXT, selection,
                                 reinterpret_cast<LPARAM>(buffer));
                    open_path(std::filesystem::path(buffer));
                }
            }
            return 0;
        case ID_TEXT:
            if (code == EN_CHANGE && !g.suppress_text && !g.rebuilding) {
                const std::string session_before = g.session.text();
                const std::string text =
                    make_session_text(session_before, g.encoding, window_text(g.text_edit));
                const vmt::Result<void> result =
                    g.session.set_text(text, g.session.display_name());
                if (result) {
                    // Coalesce rapid keystrokes into one panel rebuild.
                    SetTimer(hwnd, kPanelRebuildTimer, kPanelRebuildDelayMs, nullptr);
                }
                note_preview_change();
                clear_validation_summary();
                update_title();
                update_status();
            }
            return 0;
        default:
            break;
        }

        if (id >= ID_ROW_EDIT_BASE) {
            on_panel_command(id, code, control);
            return 0;
        }
        return 0;
    }

    case WM_HSCROLL:
        on_panel_hscroll(reinterpret_cast<HWND>(lparam));
        return 0;

    case WM_TIMER:
        if (wparam == kPanelRebuildTimer) {
            KillTimer(hwnd, kPanelRebuildTimer);
            rebuild_panel();
            update_status();
            return 0;
        }
        if (wparam == kPreviewSyncTimer) {
            KillTimer(hwnd, kPreviewSyncTimer);
            if (g.preview_sync.update_pending()) {
                if (g.engine && g.engine->initialized()) {
                    const vmt::Result<void> flushed = g.preview_sync.flush(g.session.text());
                    if (flushed) {
                        g.engine_note = L"engine preview active (" +
                                        to_wide(std::string(ui::kPreviewMaterialName)) + L")";
                    } else {
                        g.engine_note = L"preview: " + to_wide(flushed.error().message);
                    }
                } else {
                    g.preview_sync.cancel();
                }
                update_status();
            }
            return 0;
        }
        if (wparam == kTickerTimer) {
            if (g.engine && g.engine->initialized()) {
                const vmt::Result<void> rendered = g.engine->render();
                if (!rendered) {
                    KillTimer(hwnd, kTickerTimer);
                    KillTimer(hwnd, kPreviewSyncTimer);
                    g.preview_sync.set_active(false);
                    g.preview_active = false;
                    g.engine_note = L"engine render failed: " +
                                    to_wide(rendered.error().message);
                    update_status();
                }
            } else {
                KillTimer(hwnd, kTickerTimer);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_CLOSE: {
        if (g.session.dirty()) {
            const int answer = MessageBoxW(hwnd,
                                           L"The material has unsaved changes.\n\nSave before closing?",
                                           kWindowTitle,
                                           MB_ICONWARNING | MB_YESNOCANCEL);
            if (answer == IDCANCEL) {
                return 0;
            }
            if (answer == IDYES) {
                do_save(false);
                if (g.session.dirty()) {
                    return 0; // saving failed or was cancelled
                }
            }
        }
        stop_engine_preview();
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, kPanelRebuildTimer);
        KillTimer(hwnd, kTickerTimer);
        KillTimer(hwnd, kPreviewSyncTimer);
        g.preview_sync.set_active(false);
        destroy_material_browser();
        if (g.engine) {
            g.engine->shutdown();
        }
        save_settings();
        log_write("APP ", "main window destroyed; exiting");
        log_session_end();
        log_close();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool register_window_classes(HINSTANCE instance) {
    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.style = CS_HREDRAW | CS_VREDRAW;
    main_class.lpfnWndProc = MainProc;
    main_class.hInstance = instance;
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
    main_class.lpszClassName = kMainClass;
    main_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    main_class.hIconSm = main_class.hIcon;
    if (RegisterClassExW(&main_class) == 0) {
        return false;
    }

    WNDCLASSEXW panel_class{};
    panel_class.cbSize = sizeof(panel_class);
    panel_class.style = CS_HREDRAW | CS_VREDRAW;
    panel_class.lpfnWndProc = PanelProc;
    panel_class.hInstance = instance;
    panel_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    panel_class.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW) + 1);
    panel_class.lpszClassName = kPanelClass;
    if (RegisterClassExW(&panel_class) == 0) {
        return false;
    }

    WNDCLASSEXW viewport_class{};
    viewport_class.cbSize = sizeof(viewport_class);
    viewport_class.style = CS_HREDRAW | CS_VREDRAW;
    viewport_class.lpfnWndProc = ViewportProc;
    viewport_class.hInstance = instance;
    viewport_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    viewport_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    viewport_class.lpszClassName = kViewportClass;
    if (RegisterClassExW(&viewport_class) == 0) {
        return false;
    }

    WNDCLASSEXW validation_class{};
    validation_class.cbSize = sizeof(validation_class);
    validation_class.lpfnWndProc = ValidationProc;
    validation_class.hInstance = instance;
    validation_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    validation_class.hbrBackground = reinterpret_cast<HBRUSH>(
        static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
    validation_class.lpszClassName = kValidationClass;
    return RegisterClassExW(&validation_class) != 0;
}

void create_controls(HINSTANCE instance) {
    g.open_button = CreateWindowExW(0, L"BUTTON", L"Open VMT...",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, 0, 0, g.main,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OPEN)),
                                    instance, nullptr);
    g.save_button = CreateWindowExW(0, L"BUTTON", L"Save",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    0, 0, 0, 0, g.main,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE)),
                                    instance, nullptr);
    g.save_as_button = CreateWindowExW(0, L"BUTTON", L"Save As",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       0, 0, 0, 0, g.main,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE_AS)),
                                       instance, nullptr);
    g.preview_button = CreateWindowExW(0, L"BUTTON", L"Preview material",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       0, 0, 0, 0, g.main,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PREVIEW)),
                                       instance, nullptr);
    g.validate_button = CreateWindowExW(0, L"BUTTON", L"Validate",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                        0, 0, 0, 0, g.main,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VALIDATE)),
                                        instance, nullptr);
    g.recent_label = CreateWindowExW(0, L"STATIC", L"Recent files",
                                     WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g.main, nullptr,
                                     instance, nullptr);
    g.recent_list = CreateWindowExW(0, L"LISTBOX", L"",
                                    WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                                        LBS_NOTIFY,
                                    0, 0, 0, 0, g.main,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RECENT)),
                                    instance, nullptr);
    g.viewport = CreateWindowExW(0, kViewportClass, L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 0, 0, 0, 0, g.main,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEWPORT)),
                                 instance, nullptr);
    g.advanced_check = CreateWindowExW(0, L"BUTTON", L"Show advanced parameters",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       0, 0, 0, 0, g.main,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_ADVANCED)),
                                       instance, nullptr);
    g.panel = CreateWindowExW(0, kPanelClass, L"",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                              0, 0, 0, 0, g.main,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PANEL)),
                              instance, nullptr);
    g.text_edit = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                                      WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                                      ES_AUTOHSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                                  0, 0, 0, 0, g.main,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TEXT)),
                                  instance, nullptr);
    g.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                               WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                               0, 0, 0, 0, g.main,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)),
                               instance, nullptr);
    const int parts[2] = {dp(520), -1};
    SendMessageW(g.status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));

    set_font(g.open_button, stock_font());
    set_font(g.save_button, stock_font());
    set_font(g.save_as_button, stock_font());
    set_font(g.preview_button, stock_font());
    set_font(g.validate_button, stock_font());
    set_font(g.recent_label, stock_font());
    set_font(g.recent_list, stock_font());
    set_font(g.viewport, stock_font());
    set_font(g.advanced_check, stock_font());
    set_font(g.panel, stock_font());
    set_font(g.text_edit, mono_font());

    SendMessageW(g.advanced_check, BM_SETCHECK,
                 g.settings.show_advanced ? BST_CHECKED : BST_UNCHECKED, 0);
}

void create_menu() {
    HMENU bar = CreateMenu();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, ID_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(file, MF_STRING, ID_SAVE_AS, L"Save &As...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_BROWSER, L"Material &browser\tF3");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, ID_ADVANCED, L"Show &advanced parameters");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

    HMENU preview = CreatePopupMenu();
    AppendMenuW(preview, MF_STRING, IDM_PREVIEW_OPTIONS, L"Preview &options...");
    AppendMenuW(preview, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(preview, MF_STRING, IDM_SHAPE_SPHERE, L"Sphere");
    AppendMenuW(preview, MF_STRING, IDM_SHAPE_CUBE, L"Cube");
    AppendMenuW(preview, MF_STRING, IDM_SHAPE_CYLINDER, L"Cylinder");
    AppendMenuW(preview, MF_STRING, IDM_SHAPE_PLANE, L"Plane");
    AppendMenuW(preview, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(preview, MF_STRING, IDM_MODEL, L"Load model (.mdl)...");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(preview), L"&Preview");

    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, ID_VALIDATE, L"&Validate\tF7");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");

    HMENU game = CreatePopupMenu();
    AppendMenuW(game, MF_STRING, IDM_GAME_CONNECTION, L"&Connection...");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(game), L"&Game");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");

    SetMenu(g.main, bar);

    ACCEL entries[4] = {};
    entries[0].fVirt = FVIRTKEY | FCONTROL;
    entries[0].key = 'O';
    entries[0].cmd = ID_OPEN;
    entries[1].fVirt = FVIRTKEY | FCONTROL;
    entries[1].key = 'S';
    entries[1].cmd = ID_SAVE;
    entries[2].fVirt = FVIRTKEY;
    entries[2].key = VK_F7;
    entries[2].cmd = ID_VALIDATE;
    entries[3].fVirt = FVIRTKEY;
    entries[3].key = VK_F3;
    entries[3].cmd = IDM_BROWSER;
    g.accelerator = CreateAcceleratorTableW(entries, 4);
}

void load_schemas() {
#ifdef VMT_SCHEMA_DIR
    vmt::Result<vmt::SchemaLibrary> library =
        vmt::SchemaLibrary::load_dir(std::filesystem::u8path(VMT_SCHEMA_DIR));
    if (library) {
        g.schemas = std::move(library.value());
        g.schemas_loaded = true;
        g.schema_error.clear();
    } else {
        g.schema_error = library.error().message;
    }
#else
    g.schema_error = "VMT_SCHEMA_DIR was not defined at build time";
#endif
}

} // namespace

int run_app(HINSTANCE instance, int show_command) {
    // System-DPI awareness: the layout below scales its metrics with dp().
    SetProcessDPIAware();
    {
        HDC screen = GetDC(nullptr);
        if (screen != nullptr) {
            const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
            if (dpi > 0) {
                g.dpi = dpi;
            }
            ReleaseDC(nullptr, screen);
        }
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    g.instance = instance;
    g.settings_path = compute_settings_path();
    g.settings = UISettings::load(g.settings_path);
    g.preview_options = options_from_settings(g.settings);
    g.thumbnail_root = thumbnail_root_dir();

    // Crash-safe logging: open before anything can fail, report a previous
    // unclean session, then install the crash handler so its records land in
    // the already-open log.
    {
        const std::filesystem::path log_dir = local_data_dir();
        if (!log_dir.empty()) {
            log_open(log_dir);
            log_report_previous_session();
            log_session_begin();
            log_write("APP ", "VMT Viewer starting");
            install_crash_handler();
        }
    }
    load_schemas();

    if (!register_window_classes(instance)) {
        MessageBoxW(nullptr, L"window class registration failed.", kWindowTitle,
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    g.main = CreateWindowExW(0, kMainClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr,
                             instance, nullptr);
    if (g.main == nullptr) {
        MessageBoxW(nullptr, L"main window creation failed.", kWindowTitle,
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    create_controls(instance);
    create_menu();
    sync_shape_menu();
    configure_preview_sync();
    layout();
    refresh_recent_list();
    rebuild_panel();
    update_title();
    update_status();

    ShowWindow(g.main, show_command == 0 ? SW_SHOWDEFAULT : show_command);
    UpdateWindow(g.main);

    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (g.accelerator != nullptr &&
            TranslateAcceleratorW(g.main, g.accelerator, &message) != 0) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g.accelerator != nullptr) {
        DestroyAcceleratorTable(g.accelerator);
        g.accelerator = nullptr;
    }
    save_settings();

    if (result == -1) {
        return 2;
    }
    return static_cast<int>(message.wParam);
}

int run_engine_probe(const std::wstring& bin_dir, const std::wstring& game_dir) {
    const auto log_probe = [](const std::string& text) {
        wchar_t temp[MAX_PATH] = {};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (length == 0 || length >= MAX_PATH) return;
        const std::filesystem::path path =
            std::filesystem::path(temp) / L"VMTViewer" / L"engine.log";
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        FILE* file = nullptr;
        _wfopen_s(&file, path.c_str(), L"a");
        if (file != nullptr) {
            std::fprintf(file, "[PROBE] %s\n", text.c_str());
            std::fclose(file);
        }
    };

    // Hidden host window, positioned off-screen: the engine needs a real HWND
    // for SetMode, but the user must not see the compatibility check flash.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VMTViewerEngineProbe";
    RegisterClassExW(&wc);

    HWND top = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, -32000, -32000,
                               320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    if (top == nullptr) {
        return 1;
    }
    ShowWindow(top, SW_SHOWNOACTIVATE);
    HWND child = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 320, 240, top,
                                 nullptr, wc.hInstance, nullptr);
    if (child == nullptr) {
        DestroyWindow(top);
        return 1;
    }

    vmt::Result<std::unique_ptr<bridge::EngineView>> created = bridge::EngineView::create();
    if (!created) {
        DestroyWindow(top);
        return 2;
    }
    std::unique_ptr<bridge::EngineView> view = std::move(created.value());

    bridge::EngineViewConfig config;
    config.bin_dir = std::filesystem::path(bin_dir);
    config.game_dir = std::filesystem::path(game_dir);
    config.child_window = child;
    config.width = 320;
    config.height = 240;

    vmt::Result<void> step = view->initialize(config);
    if (!step) {
        log_probe("initialize failed: " + step.error().message);
        view->shutdown();
        DestroyWindow(top);
        return 3;
    }
    step = view->render();
    if (!step) {
        log_probe("render failed: " + step.error().message);
        view->shutdown();
        DestroyWindow(top);
        return 4;
    }
    view->shutdown();
    DestroyWindow(top);
    return 0;
}

// --- app hooks (Phase 9 browser / preview options) ---------------------------
// Declared in ui/src/app_hooks.h; implemented here because this TU owns the
// editor session, the engine and the settings file.

namespace app {

bool engine_ready() {
    return g.engine != nullptr && g.engine->initialized();
}

bool ensure_engine(std::wstring& error) {
    if (g.preview_root.empty()) {
        g.preview_root = preview_root_dir();
    }
    return ensure_engine_running(error, false);
}

bool game_materials(std::vector<std::string>& names, bool& truncated, std::wstring& error) {
    if (!ensure_engine(error)) {
        return false;
    }
    const vmt::Result<std::vector<std::string>> listed = g.engine->list_materials();
    if (!listed) {
        error = to_wide(listed.error().message);
        return false;
    }
    names = listed.value();
    truncated = names.size() >= MaterialIndex::kMaxEntries;
    log_write("BRWS", "engine material scan: " + std::to_string(names.size()) + " entries");
    return true;
}

bool game_material_text(const std::string& material_name, std::string& text,
                        std::wstring& error) {
    if (!ensure_engine(error)) {
        return false;
    }
    std::string path = "materials/" + material_name;
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".vmt") != 0) {
        path += ".vmt";
    }
    const vmt::Result<std::string> read = g.engine->read_text_file(path);
    if (!read) {
        error = to_wide(read.error().message);
        return false;
    }
    text = read.value();
    return true;
}

bool open_game_material(const std::string& material_name, std::wstring& error) {
    std::string name = material_name_from_game_path(material_name);
    if (name.empty()) {
        name = material_name;
    }
    if (name.empty()) {
        error = L"empty material name";
        return false;
    }

    std::string text;
    if (!game_material_text(name, text, error)) {
        return false;
    }
    const vmt::Result<void> opened = g.session.open_text(std::move(text), name + ".vmt");
    if (!opened) {
        error = to_wide(opened.error().message);
        return false;
    }

    g.preview_material_name = name;
    log_write("FILE", "opened game material '" + name + "'");
    KillTimer(g.main, kPanelRebuildTimer);
    refresh_text_control();
    rebuild_panel();
    clear_validation_summary();
    update_title();
    update_status();

    if (!g.settings.last_bin_dir.empty() && !g.settings.last_game_dir.empty()) {
        (void)start_preview_impl(false);
    }
    return true;
}

bool open_material_file(const std::filesystem::path& path) {
    return open_path(path);
}

bool render_thumbnail(const std::string& material_name, std::filesystem::path& bmp_path,
                      std::wstring& error) {
    if (g.thumbnail_root.empty()) {
        error = L"no thumbnail cache folder is available";
        return false;
    }
    bmp_path = thumbnail_cache_path(g.thumbnail_root, material_name);

    std::error_code ec;
    const bool cached = std::filesystem::is_regular_file(bmp_path, ec);
    if (!engine_ready()) {
        if (cached) {
            return true; // browsing without the engine: show the disk cache
        }
        if (!ensure_engine(error)) {
            return false;
        }
    }

    const vmt::Result<bridge::Thumbnail> rendered = g.engine->render_material_thumbnail(
        material_name, 160, g.settings.thumbnail_flip);
    if (!rendered) {
        if (cached) {
            error = L"engine render failed; showing the cached thumbnail";
            return true;
        }
        error = to_wide(rendered.error().message);
        return false;
    }

    const bridge::Thumbnail& thumbnail = rendered.value();
    if (!write_bmp_from_bgra(bmp_path, thumbnail.width, thumbnail.height,
                             thumbnail.bgra.data(), true)) {
        error = L"the thumbnail cache could not be written: " + bmp_path.wstring();
        return false;
    }
    log_write("BRWS", "thumbnail rendered and cached for '" + material_name + "'");
    return true;
}

std::filesystem::path thumbnail_root() {
    return g.thumbnail_root;
}

bool thumbnail_flip() {
    return g.settings.thumbnail_flip;
}

void set_thumbnail_flip(bool flip) {
    g.settings.thumbnail_flip = flip;
    save_settings();
}

std::string last_browser_root() {
    return g.settings.browser_scan_game ? std::string() : g.settings.browser_root;
}

void remember_browser_source(bool game_scan, const std::filesystem::path& root) {
    g.settings.browser_scan_game = game_scan;
    if (game_scan) {
        g.settings.browser_root.clear();
    } else {
        g.settings.browser_root = root.string();
    }
    save_settings();
}

PreviewOptions preview_options() {
    return g.preview_options;
}

void apply_preview_options(const PreviewOptions& options) {
    g.preview_options = options;
    apply_preview_to_engine();
    sync_shape_menu();
}

void commit_preview_options() {
    save_settings();
}

void pick_and_load_model(HWND owner) {
    wchar_t buffer[4 * MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Source models (*.mdl)\0*.mdl\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = 4 * MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Load model for preview";
    if (GetOpenFileNameW(&ofn) == FALSE) {
        return;
    }

    std::wstring error;
    if (!ensure_engine(error)) {
        MessageBoxW(owner, error.c_str(), L"Model preview", MB_ICONWARNING | MB_OK);
        return;
    }
    const vmt::Result<void> loaded = g.engine->set_model(std::filesystem::path(buffer));
    if (!loaded) {
        const std::wstring message =
            to_wide(loaded.error().message) +
            L"\n\nUntil then, materials render on the sphere/cube/cylinder/plane "
            L"primitives selected under Preview > Preview options.";
        MessageBoxW(owner, message.c_str(), L"Model preview",
                    MB_ICONINFORMATION | MB_OK);
        log_write("ENG ", "set_model rejected: " + loaded.error().message);
        return;
    }
    g.engine_note = L"model loaded (" + std::filesystem::path(buffer).filename().wstring() + L")";
    update_status();
}

} // namespace app

} // namespace ui
