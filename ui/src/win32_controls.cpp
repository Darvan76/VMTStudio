#include "ui/win32_controls.h"

#include <ui/d2d_canvas.h>
#include <ui/theme_button.h>
#include <ui/theme_combo.h>
#include <ui/theme_slider.h>
#include <ui/win32_theme.h>

namespace ui::controls {
namespace {

// Mirrors main_window.cpp's behaviour: the shell wants the acrylic-flavoured
// backdrop on Win11 and simply falls back to a flat opaque window elsewhere.
constexpr bool kUseAcrylicBackdrop = true;

} // namespace

bool register_all(HINSTANCE instance) {
    if (instance == nullptr) {
        return false;
    }
    // No short-circuit: registering all three is cheap and keeps the failure
    // reporting honest if one of them ever stops registering.
    const bool button_ok = ThemeButton::register_class(instance);
    const bool slider_ok = ThemeSlider::register_class(instance);
    const bool combo_ok = ThemeCombo::register_class(instance);
    return button_ok && slider_ok && combo_ok;
}

void apply_chrome(HWND top_level) {
    if (!IsWindow(top_level)) {
        return;
    }
    theming::apply_window_chrome(top_level);
    // Menus and the remaining common controls only go dark once the window has
    // opted in; without this the menu bar stays light while everything around
    // it is dark.
    theming::allow_native_dark_mode(top_level);
    apply_backdrop(top_level, kUseAcrylicBackdrop);
}

void apply_backdrop(HWND top_level, bool acrylic) {
    if (!IsWindow(top_level)) {
        return;
    }
    theming::apply_backdrop(top_level, acrylic);
}

void refresh_theme(HWND top_level) {
    if (!IsWindow(top_level)) {
        return;
    }
    // Chrome first: the caption colour is part of the non-client area and is
    // not covered by the child walk.
    apply_chrome(top_level);
    theming::apply_subtree(top_level);
}

bool handle_theme_change(HWND top_level, LPARAM setting_change_param) {
    if (!theming::is_theme_change(setting_change_param)) {
        return false;
    }
    refresh_theme(top_level);
    return true;
}

void handle_dpi_change(HWND top_level) {
    if (!IsWindow(top_level)) {
        return;
    }
    // The custom controls re-read dpi on every paint, so they need no explicit
    // notification - but the native controls the shell still uses (STATIC,
    // LISTBOX, the status bar, the tree view) do need a font and repaint.
    theming::redraw_subtree(top_level);
}

} // namespace ui::controls