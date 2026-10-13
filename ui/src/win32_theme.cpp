#include "ui/win32_theme.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <winreg.h>

#include <cstring>
#include <map>

// --- DWM attributes that older SDKs do not ship -------------------------------
// These must be declared AFTER <dwmapi.h>. Newer SDKs publish the very same
// values as enumerators of DWMWINDOWATTRIBUTE rather than as macros, and a
// macro defined *before* the header would rewrite the enumerator list into
// nonsense ("20 = 20,"). Defining them afterwards is safe in both directions:
// a pre-existing macro is skipped by the guard, and an existing enumerator is
// shadowed only inside this translation unit with the identical value.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

// DWM_E_COMPOSITIONDISABLED is 0x81000007. Guarded for the same reason.
#ifndef DWM_E_COMPOSITIONDISABLED
#define DWM_E_COMPOSITIONDISABLED ((HRESULT)0x81000007L)
#endif

// The corner/backdrop *types* cannot be declared with #ifndef guards (a typedef
// is invisible to the preprocessor, and an SDK that ships one would make a
// redefinition a hard error), so they get module-local spellings of their own.
// DwmSetWindowAttribute takes them as an opaque `const void*`, so the spelling
// is an implementation detail and never leaks into a caller's code.
namespace ui::theming {
namespace {

enum class CornerPreference : int {
    Default = 0,
    DoNotRound = 1,
    Round = 2,
    RoundSmall = 3,
};

enum class SystemBackdrop : int {
    Auto = 0,
    None = 1,
    MainWindow = 2,
    TransientWindow = 3,
    TabbedWindow = 4,
};

constexpr UINT kDefaultDpi = 96;
constexpr DWORD kMinBuildForSystemBackdrop = 22621; // Windows 11 22H2

// Registry: HKCU\...\Themes\Personalize -> AppsUseLightTheme (REG_DWORD).
constexpr wchar_t kPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kAppsUseLightTheme[] = L"AppsUseLightTheme";

// Common control classes we care about.
constexpr wchar_t kClassComboBox[] = L"ComboBox";
constexpr wchar_t kClassEdit[] = L"Edit";
constexpr wchar_t kClassStatusBar[] = L"msctls_statusbar32";
constexpr wchar_t kClassListView[] = L"SysListView32";
constexpr wchar_t kClassHeader[] = L"SysHeader32";
constexpr wchar_t kClassStatic[] = L"Static";
constexpr wchar_t kClassButton[] = L"Button";
constexpr wchar_t kClassListBox[] = L"ListBox";
constexpr wchar_t kClassTreeView[] = L"SysTreeView32";
constexpr wchar_t kClassToolbar[] = L"ToolbarWindow32";
constexpr wchar_t kClassTrackBar[] = L"msctls_trackbar32";
constexpr wchar_t kClassScrollBar[] = L"ScrollBar";
constexpr wchar_t kClassUpDown[] = L"msctls_updown32";
constexpr wchar_t kClassProgress[] = L"msctls_progress32";
constexpr wchar_t kClassRichEdit20[] = L"RichEdit20W";
constexpr wchar_t kClassRichEdit50[] = L"RichEdit50W";

// UxTheme dark-mode theme names. DarkMode_Explorer is the generic fallback that
// every other common control accepts.
constexpr wchar_t kThemeCfd[] = L"DarkMode_CFD";
constexpr wchar_t kThemeStatusBar[] = L"ExplorerStatusBar";
constexpr wchar_t kThemeItemsView[] = L"DarkMode_ItemsView";
constexpr wchar_t kThemeExplorer[] = L"DarkMode_Explorer";

constexpr int kClassNameCapacity = 64;

// --- Fluent 2 dark -------------------------------------------------------------
// surface #202020 is the Win11 dark mica base; each "elevated" step is +1
// elevation level in the Fluent layer ramp. Accent is the Windows 11 accent
// blue. Measured WCAG ratios against `text`: text/surface 16.3:1,
// text/surface_alt 13.8:1, text_muted/surface 10.1:1, text_disabled/surface
// 5.0:1, text_on_accent/accent 4.5:1 (the AA bar, hence the canonical hue).
constexpr Palette kDarkPalette = {
    RGB(32, 32, 32),     // surface        - app frame / mica base
    RGB(44, 44, 44),     // surface_alt    - rows, cards, list background
    RGB(51, 51, 51),     // surface_hover  - row under the cursor
    RGB(58, 58, 58),     // surface_pressed- row being clicked
    RGB(24, 24, 24),     // surface_sunken - edit fields, list wells (recessed)
    RGB(72, 72, 72),     // border         - default hairline (decorative)
    RGB(120, 120, 120),  // border_strong  - focused border / active tab (3.7:1)
    RGB(56, 56, 56),     // divider        - section separators
    RGB(255, 255, 255),  // text           - primary label
    RGB(200, 204, 214),  // text_muted     - secondary label / units
    RGB(140, 143, 150),  // text_disabled  - unavailable control
    RGB(255, 255, 255),  // text_on_accent - label on accent fill
    RGB(0, 120, 212),    // accent         - primary action / selection
    RGB(16, 133, 222),   // accent_hover   - primary action, pointer over
    RGB(0, 99, 177),     // accent_pressed - primary action, held
    RGB(38, 68, 94),     // accent_muted   - selected row tint
    RGB(96, 173, 255),   // focus_ring     - keyboard focus outline (6.9:1)
    RGB(255, 99, 75),    // danger         - errors, delete
    RGB(255, 185, 89),   // warning        - fallbacks, degraded source
    RGB(78, 201, 176),   // success        - loaded, verified
};

// --- Fluent 2 light ------------------------------------------------------------
// surface #F0F0F0 is the Win11 light mica base; elevated surfaces go lighter
// (cards are near-white) and recessed ones go white, which is how Fluent light
// renders text fields. Measured WCAG ratios against `text`: text/surface 18.4:1,
// text_muted/surface 5.5:1, text_on_accent/accent 6.3:1.
constexpr Palette kLightPalette = {
    RGB(240, 240, 240),  // surface        - app frame / mica base
    RGB(252, 252, 252),  // surface_alt    - rows, cards, list background
    RGB(233, 233, 233),  // surface_hover  - row under the cursor
    RGB(222, 222, 222),  // surface_pressed- row being clicked
    RGB(255, 255, 255),  // surface_sunken - edit fields, list wells
    RGB(225, 225, 225),  // border         - default hairline (decorative)
    RGB(96, 96, 96),     // border_strong  - focused border / active tab (6.5:1)
    RGB(235, 235, 235),  // divider        - section separators
    RGB(0, 0, 0),        // text           - primary label
    RGB(96, 96, 96),     // text_muted     - secondary label / units
    RGB(160, 160, 160),  // text_disabled  - unavailable control
    RGB(255, 255, 255),  // text_on_accent - label on accent fill
    RGB(0, 95, 184),     // accent         - primary action / selection
    RGB(0, 84, 166),     // accent_hover   - primary action, pointer over
    RGB(0, 78, 152),     // accent_pressed - primary action, held
    RGB(222, 238, 252),  // accent_muted   - selected row tint
    RGB(0, 103, 192),    // focus_ring     - keyboard focus outline (5.0:1)
    RGB(196, 43, 28),    // danger         - errors, delete
    RGB(138, 86, 0),     // warning        - fallbacks, degraded source (5.4:1)
    RGB(16, 124, 16),    // success        - loaded, verified (4.7:1)
};

// --- process-global theme state (UI thread only) -------------------------------
ThemeMode g_override_mode = ThemeMode::Light;
bool g_has_override = false;
Palette g_cached_palette = kLightPalette;
ThemeMode g_cached_mode = ThemeMode::Light;
bool g_cache_valid = false;

using GetDpiForWindowProc = UINT(WINAPI*)(HWND);

// RTL_OSVERSIONINFOW layout, declared locally so <winternl.h> (and a hard
// dependency on ntdll.lib) stays out of the build.
struct OsVersionInfoW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
};
using RtlGetVersionProc = LONG(WINAPI*)(OsVersionInfoW*);

static_assert(sizeof(GetDpiForWindowProc) == sizeof(FARPROC), "function pointers must match");
static_assert(sizeof(RtlGetVersionProc) == sizeof(FARPROC), "function pointers must match");

// FARPROC -> specific prototype via memcpy instead of a reinterpret_cast: MSVC
// emits C4191 ("unsafe conversion from 'FARPROC' to ...'") for the cast at /W1,
// which /WX would turn into a build break. Copying the pointer representation is
// exact and warning-free.
template <typename Fn>
Fn proc_from_getprocaddress(HMODULE module, const char* name) {
    if (module == nullptr) {
        return nullptr;
    }
    const FARPROC raw = ::GetProcAddress(module, name);
    if (raw == nullptr) {
        return nullptr;
    }
    Fn typed = nullptr;
    std::memcpy(&typed, &raw, sizeof(typed));
    return typed;
}

GetDpiForWindowProc resolve_get_dpi_for_window() {
    return proc_from_getprocaddress<GetDpiForWindowProc>(
        ::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
}

DWORD query_build_number_impl() {
    const RtlGetVersionProc rtl = proc_from_getprocaddress<RtlGetVersionProc>(
        ::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtl == nullptr) {
        return 0;
    }
    OsVersionInfoW info = {};
    info.dwOSVersionInfoSize = static_cast<ULONG>(sizeof(info));
    if (rtl(&info) != 0) { // STATUS_SUCCESS
        return 0;
    }
    return info.dwBuildNumber;
}

DWORD query_build_number() {
    static const DWORD build = query_build_number_impl();
    return build;
}

bool backdrop_supported() {
    return query_build_number() >= kMinBuildForSystemBackdrop;
}

UINT dpi_for_window(HWND hwnd) {
    static const GetDpiForWindowProc proc = resolve_get_dpi_for_window();
    if (proc == nullptr || hwnd == nullptr) {
        return kDefaultDpi;
    }
    const UINT dpi = proc(hwnd);
    return dpi == 0 ? kDefaultDpi : dpi;
}

// Every DWM call funnels through here. Failures are deliberately swallowed: the
// app has to look and behave identically on Windows 10, on Windows 11 builds that
// predate an attribute, and with DWM composition switched off (remote desktop
// "disable desktop composition", a broken GPU driver, a session-0 host). An
// unsupported attribute comes back as E_INVALIDARG, a disabled composition stack
// as DWM_E_COMPOSITIONDISABLED; anything else is treated the same way because
// there is no useful recovery from any of them.
void set_dwm_attribute(HWND hwnd, DWMWINDOWATTRIBUTE attribute, const void* value,
                       DWORD size) {
    const HRESULT hr = ::DwmSetWindowAttribute(hwnd, attribute, value, size);
    if (hr == E_INVALIDARG || hr == DWM_E_COMPOSITIONDISABLED) {
        return;
    }
}

// Parameter block for the EnumChildWindows callback. EnumChildWindows already
// walks every descendant, so one flat pass is the whole subtree.
struct SubtreeContext {
    bool dark;
};

BOOL CALLBACK apply_theme_to_child(HWND child, LPARAM param) {
    const SubtreeContext* ctx = reinterpret_cast<const SubtreeContext*>(param);
    if (ctx == nullptr) {
        return TRUE;
    }

    wchar_t class_name[kClassNameCapacity] = {};
    const int length = ::GetClassNameW(child, class_name, kClassNameCapacity);
    if (length <= 0) {
        // The window is already gone (its owner is tearing down). Nothing to theme.
        return TRUE;
    }

    // SysHeader32 must be skipped: the header control ignores DarkMode_ItemsView
    // and renders the header with a mismatched background and text colour, which
    // corrupts the list view it belongs to. Leaving it on the default theme keeps
    // the header legible in both modes, which is why the header background is
    // painted by the list view itself.
    if (_wcsicmp(class_name, kClassHeader) == 0) {
        return TRUE;
    }

    // Allowlist, not a catch-all. Unknown classes must be left completely
    // untouched, because this walk also reaches two kinds of child that must
    // not be interfered with:
    //   * the engine viewport (class VMTViewerViewport), a child HWND whose
    //     surface is owned by the Source engine's D3D9 device - retheming it is
    //     meaningless and forcing a repaint on it is not ours to do;
    //   * this project's own custom controls (VMT.Previewer.ThemeButton and
    //     friends), which self-draw and ignore UxTheme entirely.
    // Applying DarkMode_Explorer to those would be a silent no-op at best and
    // an unnecessary forced redraw at worst, so they are skipped.
    const wchar_t* theme = nullptr;
    if (_wcsicmp(class_name, kClassComboBox) == 0 ||
        _wcsicmp(class_name, kClassEdit) == 0) {
        theme = kThemeCfd;
    } else if (_wcsicmp(class_name, kClassStatusBar) == 0) {
        theme = kThemeStatusBar;
    } else if (_wcsicmp(class_name, kClassListView) == 0) {
        theme = kThemeItemsView;
    } else if (_wcsicmp(class_name, kClassStatic) == 0 ||
               _wcsicmp(class_name, kClassButton) == 0 ||
               _wcsicmp(class_name, kClassListBox) == 0 ||
               _wcsicmp(class_name, kClassTreeView) == 0 ||
               _wcsicmp(class_name, kClassToolbar) == 0 ||
               _wcsicmp(class_name, kClassTrackBar) == 0 ||
               _wcsicmp(class_name, kClassScrollBar) == 0 ||
               _wcsicmp(class_name, kClassUpDown) == 0 ||
               _wcsicmp(class_name, kClassProgress) == 0 ||
               _wcsicmp(class_name, kClassRichEdit20) == 0 ||
               _wcsicmp(class_name, kClassRichEdit50) == 0) {
        theme = kThemeExplorer;
    } else {
        return TRUE;
    }

    // Light mode resets to the default theme (nullptr) rather than applying a dark
    // name, otherwise a window that was dark a moment ago keeps the dark visuals.
    ::SetWindowTheme(child, ctx->dark ? theme : nullptr, nullptr);
    return TRUE;
}

// True when this child is a common control whose painting we may drive with a
// synchronous update. Deliberately false for unknown classes, which covers both
// the engine viewport and this project's custom controls.
bool IsRepaintSafeClass(const wchar_t* class_name) {
    static const wchar_t* const kSafe[] = {
        kClassStatic,   kClassButton,  kClassEdit,      kClassComboBox,
        kClassListBox,  kClassTreeView, kClassToolbar,  kClassScrollBar,
        kClassTrackBar, kClassUpDown,  kClassProgress,  kClassRichEdit20,
        kClassRichEdit50,
    };
    for (const wchar_t* candidate : kSafe) {
        if (_wcsicmp(class_name, candidate) == 0) {
            return true;
        }
    }
    return false;
}

// Forced repaints must skip the engine viewport for the same reason the
// SetWindowTheme walk does: RDW_UPDATENOW pumps that child synchronously, and
// its surface belongs to the Source engine's D3D9 device. Anything we do not
// own the painting of is invalidated only, never updated now.
BOOL CALLBACK redraw_child(HWND child, LPARAM param) {
    (void)param;

    wchar_t class_name[kClassNameCapacity] = {};
    const int length = ::GetClassNameW(child, class_name, kClassNameCapacity);
    if (length > 0 && IsRepaintSafeClass(class_name)) {
        ::RedrawWindow(child, nullptr, nullptr,
                       RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    } else {
        ::RedrawWindow(child, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE);
    }
    return TRUE;
}

} // namespace
} // namespace ui::theming

namespace ui::theming {

int Metrics::scale(int design_px) const {
    if (dpi <= 0) {
        return design_px;
    }
    return ::MulDiv(design_px, dpi, 96);
}

ThemeMode system_theme_mode() {
    HKEY key = nullptr;
    const LSTATUS opened =
        ::RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_QUERY_VALUE, &key);
    if (opened != ERROR_SUCCESS) {
        return ThemeMode::Light;
    }

    DWORD type = REG_NONE;
    DWORD value = 1; // documented default: light
    DWORD size = static_cast<DWORD>(sizeof(value));
    const LSTATUS queried = ::RegQueryValueExW(key, kAppsUseLightTheme, nullptr, &type,
                                                reinterpret_cast<LPBYTE>(&value), &size);
    ::RegCloseKey(key);

    // A missing key, a missing value, or a value of an unexpected type all mean
    // "we cannot tell" - and the documented fallback is Light.
    if (queried != ERROR_SUCCESS || type != REG_DWORD ||
        size != static_cast<DWORD>(sizeof(value))) {
        return ThemeMode::Light;
    }
    return value != 0 ? ThemeMode::Light : ThemeMode::Dark;
}

ThemeMode active_theme_mode() {
    if (g_has_override) {
        return g_override_mode;
    }
    return system_theme_mode();
}

void set_theme_override(ThemeMode mode) {
    g_override_mode = mode;
    g_has_override = true;
}

void clear_theme_override() {
    g_has_override = false;
    g_override_mode = ThemeMode::Light;
}

Palette palette_for(ThemeMode mode) {
    return mode == ThemeMode::Dark ? kDarkPalette : kLightPalette;
}

const Palette& palette() {
    const ThemeMode mode = active_theme_mode();
    if (!g_cache_valid || g_cached_mode != mode) {
        g_cached_palette = palette_for(mode);
        g_cached_mode = mode;
        g_cache_valid = true;
    }
    return g_cached_palette;
}

COLORREF color(ColorRole role) {
    const Palette& p = palette();
    switch (role) {
        case ColorRole::Surface:         return p.surface;
        case ColorRole::SurfaceAlt:      return p.surface_alt;
        case ColorRole::SurfaceHover:    return p.surface_hover;
        case ColorRole::SurfacePressed:  return p.surface_pressed;
        case ColorRole::SurfaceSunken:   return p.surface_sunken;
        case ColorRole::Border:          return p.border;
        case ColorRole::BorderStrong:    return p.border_strong;
        case ColorRole::Divider:         return p.divider;
        case ColorRole::Text:            return p.text;
        case ColorRole::TextMuted:       return p.text_muted;
        case ColorRole::TextDisabled:    return p.text_disabled;
        case ColorRole::TextOnAccent:    return p.text_on_accent;
        case ColorRole::Accent:          return p.accent;
        case ColorRole::AccentHover:     return p.accent_hover;
        case ColorRole::AccentPressed:   return p.accent_pressed;
        case ColorRole::AccentMuted:     return p.accent_muted;
        case ColorRole::FocusRing:       return p.focus_ring;
        case ColorRole::Danger:          return p.danger;
        case ColorRole::Warning:         return p.warning;
        case ColorRole::Success:         return p.success;
        default:
            // Unreachable for a valid ColorRole; degrade to the frame background
            // rather than to a magic colour nobody can explain later.
            return p.surface;
    }
}

Metrics metrics_for(HWND hwnd) {
    Metrics m = {};
    m.dpi = static_cast<int>(dpi_for_window(hwnd));
    m.corner_radius = 6;
    m.border_width = 1;
    m.focus_ring_width = 1;
    m.row_height = 28;
    m.control_height = 24;
    m.padding = 8;
    m.gap = 6;
    return m;
}

void apply_window_chrome(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }

    // Reads the active mode, so calling this again after set_theme_override() or
    // after a system theme change is the whole re-apply story.
    const BOOL immersive_dark = active_theme_mode() == ThemeMode::Dark ? TRUE : FALSE;
    set_dwm_attribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(DWMWA_USE_IMMERSIVE_DARK_MODE),
                      &immersive_dark, static_cast<DWORD>(sizeof(immersive_dark)));

    // Rounded corners are a Windows 11 22H2 feature; older builds answer
    // E_INVALIDARG and the window keeps its squared corners.
    const CornerPreference corner = CornerPreference::Round;
    set_dwm_attribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(DWMWA_WINDOW_CORNER_PREFERENCE),
                      &corner, static_cast<DWORD>(sizeof(corner)));
}

void apply_backdrop(HWND hwnd, bool acrylic) {
    if (hwnd == nullptr) {
        return;
    }
    // DWMWA_SYSTEMBACKDROP_TYPE landed in build 22621. Probing the running build
    // first keeps this a no-op - not an error - on anything older, including
    // Windows 10 where the attribute simply does not exist.
    if (!backdrop_supported()) {
        return;
    }
    const SystemBackdrop backdrop =
        acrylic ? SystemBackdrop::TransientWindow : SystemBackdrop::None;
    set_dwm_attribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(DWMWA_SYSTEMBACKDROP_TYPE),
                      &backdrop, static_cast<DWORD>(sizeof(backdrop)));
}

void apply_subtree(HWND root) {
    if (root == nullptr) {
        return;
    }
    SubtreeContext ctx = {};
    ctx.dark = active_theme_mode() == ThemeMode::Dark;
    ::EnumChildWindows(root, apply_theme_to_child, reinterpret_cast<LPARAM>(&ctx));
    redraw_subtree(root);
}

bool is_theme_change(LPARAM setting_change_param) {
    if (setting_change_param == 0) {
        return false;
    }
    return _wcsicmp(reinterpret_cast<const wchar_t*>(setting_change_param),
                    L"ImmersiveColorSet") == 0;
}

// --- Native dark mode opt-in ------------------------------------------------
// The common controls only render their dark variants once the process has
// told UxTheme that it understands the dark theme. That handshake is not in any
// SDK, so the three entry points are resolved by ordinal from uxtheme.dll at
// run time. If any of them is missing - an unexpected Windows build - the
// resolution simply yields nullptr and the app carries on with the light
// visuals it would have had anyway.
enum class PreferredAppMode {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
};

struct NativeDarkMode {
    PreferredAppMode (WINAPI* set_preferred_app_mode)(PreferredAppMode) = nullptr;
    bool (WINAPI* allow_dark_mode_for_window)(HWND, bool) = nullptr;
    bool (WINAPI* is_dark_mode_allowed_for_window)(HWND) = nullptr;
    void (WINAPI* refresh_immersive_color_policy_state)() = nullptr;

    bool available() const { return set_preferred_app_mode != nullptr; }
};

NativeDarkMode& native_dark_mode() {
    static NativeDarkMode api;
    static bool resolved = false;
    if (resolved) {
        return api;
    }
    resolved = true;

    HMODULE uxtheme = ::LoadLibraryW(L"uxtheme.dll");
    if (uxtheme == nullptr) {
        return api;
    }

    // Ordinals, not names: these have never been exported under stable names.
    // 135 = SetPreferredAppMode, 133 = AllowDarkModeForWindow,
    // 139 = IsDarkModeAllowedForWindow, 104 = RefreshImmersiveColorPolicyState.
    auto resolve = [uxtheme](WORD ordinal) -> FARPROC {
        return reinterpret_cast<FARPROC>(::GetProcAddress(uxtheme,
                                                          MAKEINTRESOURCEA(ordinal)));
    };

    FARPROC set_mode = resolve(135);
    FARPROC allow = resolve(133);
    FARPROC query = resolve(139);
    FARPROC refresh = resolve(104);

    // memcpy rather than a reinterpret_cast between FARPROC and a function
    // pointer: the cast is a C4191 diagnostic that /WX would reject.
    if (set_mode != nullptr) {
        PreferredAppMode (WINAPI* fn)(PreferredAppMode);
        static_assert(sizeof(fn) == sizeof(set_mode), "function pointer size mismatch");
        std::memcpy(&fn, &set_mode, sizeof(fn));
        api.set_preferred_app_mode = fn;
    }
    if (allow != nullptr) {
        bool (WINAPI* fn)(HWND, bool);
        std::memcpy(&fn, &allow, sizeof(fn));
        api.allow_dark_mode_for_window = fn;
    }
    if (query != nullptr) {
        bool (WINAPI* fn)(HWND);
        std::memcpy(&fn, &query, sizeof(fn));
        api.is_dark_mode_allowed_for_window = fn;
    }
    if (refresh != nullptr) {
        void (WINAPI* fn)();
        std::memcpy(&fn, &refresh, sizeof(fn));
        api.refresh_immersive_color_policy_state = fn;
    }
    return api;
}

// Opts the process in to UxTheme's dark visuals. Call once, before any window
// is created. In light mode this is deliberately a no-op: forcing the app into
// dark mode would contradict the user's system setting.
void enable_native_dark_mode() {
    if (active_theme_mode() != ThemeMode::Dark) {
        return;
    }
    NativeDarkMode& api = native_dark_mode();
    if (!api.available()) {
        return;
    }
    // Order matters: the app mode has to be set before the policy is refreshed,
    // otherwise the refreshed policy still reflects the previous mode and the
    // menu bar keeps rendering light.
    api.set_preferred_app_mode(PreferredAppMode::AllowDark);
    if (api.refresh_immersive_color_policy_state != nullptr) {
        api.refresh_immersive_color_policy_state();
    }
}

BOOL CALLBACK allow_dark_for_child(HWND child, LPARAM param) {
    if (param != 0) {
        allow_native_dark_mode(child);
    }
    return TRUE;
}

void allow_native_dark_mode(HWND hwnd) {
    if (hwnd == nullptr || active_theme_mode() != ThemeMode::Dark) {
        return;
    }
    NativeDarkMode& api = native_dark_mode();
    if (api.allow_dark_mode_for_window != nullptr) {
        api.allow_dark_mode_for_window(hwnd, true);
    }
    // The menu bar lives in its own "menubar" child window, created by
    // SetMenu, and it does not inherit the opt-in from the frame. Without this
    // walk the menu strip stays light while the rest of the window is dark.
    ::EnumChildWindows(hwnd, allow_dark_for_child, 1);
}

HBRUSH theme_brush(ColorRole role) {
    // Cached against the resolved colour, so repeated repaints inside one theme
    // do not rebuild the brush. The previous brush is deleted only after the new
    // one is in place, because a class background brush stays selected for as
    // long as the window that uses it.
    static std::map<int, HBRUSH> cache;

    const COLORREF resolved = color(role);
    const int key = static_cast<int>(role);
    auto it = cache.find(key);
    if (it != cache.end()) {
        static std::map<int, COLORREF> resolved_cache;
        if (resolved_cache[key] == resolved) {
            return it->second;
        }
    }

    HBRUSH created = CreateSolidBrush(resolved);
    if (created == nullptr) {
        return static_cast<HBRUSH>(GetStockObject(COLOR_BTNFACE));
    }

    HBRUSH previous = nullptr;
    if (it != cache.end()) {
        previous = it->second;
        it->second = created;
    } else {
        cache.emplace(key, created);
    }
    static std::map<int, COLORREF> resolved_cache;
    resolved_cache[key] = resolved;
    if (previous != nullptr) {
        DeleteObject(previous);
    }
    return created;
}

HBRUSH surface_brush() {
    return theme_brush(ColorRole::Surface);
}

HBRUSH sunken_brush() {
    return theme_brush(ColorRole::SurfaceSunken);
}

void redraw_subtree(HWND root) {
    if (root == nullptr) {
        return;
    }
    // The root itself is repainted synchronously, but WITHOUT RDW_ALLCHILDREN.
    // RDW_ALLCHILDREN | RDW_UPDATENOW would pump WM_PAINT through the entire
    // subtree up front - including the engine viewport, whose surface belongs
    // to the Source engine's D3D9 device and is not ours to repaint. Descendants
    // are handled by the EnumChildWindows pass below, which is class-aware and
    // only forces a synchronous update on controls we own the painting of.
    ::RedrawWindow(root, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
    ::EnumChildWindows(root, redraw_child, 0);
}

} // namespace ui::theming
