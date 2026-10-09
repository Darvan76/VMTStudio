#include "preview_options_dialog.h"

#include "app_hooks.h"

#include <commctrl.h>

#include <cwchar>
#include <string>

namespace ui {
namespace {

constexpr wchar_t kOptionsClass[] = L"VMTViewerPreviewOptions";
constexpr wchar_t kOptionsTitle[] = L"Preview options";

enum OptionsId : int {
    IDC_SHAPE_SPHERE = 400,
    IDC_SHAPE_CUBE,
    IDC_SHAPE_CYLINDER,
    IDC_SHAPE_PLANE,
    IDC_ENV_LABEL,
    IDC_ENV_COMBO,
    IDC_AMBIENT_LABEL,
    IDC_AMBIENT,
    IDC_AMBIENT_VALUE,
    IDC_AZIMUTH_LABEL,
    IDC_AZIMUTH,
    IDC_AZIMUTH_VALUE,
    IDC_ELEVATION_LABEL,
    IDC_ELEVATION,
    IDC_ELEVATION_VALUE,
    IDC_INTENSITY_LABEL,
    IDC_INTENSITY,
    IDC_INTENSITY_VALUE,
    IDC_BACKGROUND_LABEL,
    IDC_BACKGROUND,
    IDC_BACKGROUND_VALUE,
    IDC_MODEL,
};

struct OptionsState {
    HWND hwnd = nullptr;
    HWND sphere = nullptr;
    HWND cube = nullptr;
    HWND cylinder = nullptr;
    HWND plane = nullptr;
    HWND env_combo = nullptr;
    HWND ambient = nullptr;
    HWND azimuth = nullptr;
    HWND elevation = nullptr;
    HWND intensity = nullptr;
    HWND background = nullptr;
    HWND ambient_value = nullptr;
    HWND azimuth_value = nullptr;
    HWND elevation_value = nullptr;
    HWND intensity_value = nullptr;
    HWND background_value = nullptr;
    PreviewOptions options;
    bool running = true;
    bool updating = false;
    int dpi = 96;
};

OptionsState* g_options = nullptr;

int dp(int value) {
    return g_options != nullptr ? MulDiv(value, g_options->dpi, 96) : value;
}

// Trackbar positions for the rig sliders. Position 0..100 for levels,
// degrees for the angles.
int pos(float value) {
    return static_cast<int>(value + (value >= 0.0f ? 0.5f : -0.5f));
}

void set_value_labels() {
    if (g_options == nullptr) {
        return;
    }
    wchar_t buffer[64] = {};
    std::swprintf(buffer, 64, L"%.2f", static_cast<double>(g_options->options.rig.ambient));
    SetWindowTextW(g_options->ambient_value, buffer);
    std::swprintf(buffer, 64, L"%d deg", pos(g_options->options.rig.key_azimuth));
    SetWindowTextW(g_options->azimuth_value, buffer);
    std::swprintf(buffer, 64, L"%d deg", pos(g_options->options.rig.key_elevation));
    SetWindowTextW(g_options->elevation_value, buffer);
    std::swprintf(buffer, 64, L"%.2f", static_cast<double>(g_options->options.rig.key_intensity));
    SetWindowTextW(g_options->intensity_value, buffer);
    std::swprintf(buffer, 64, L"%.2f", static_cast<double>(g_options->options.rig.background));
    SetWindowTextW(g_options->background_value, buffer);
}

void sync_controls_from_options() {
    if (g_options == nullptr) {
        return;
    }
    g_options->updating = true;
    SendMessageW(g_options->ambient, TBM_SETPOS, TRUE,
                 pos(g_options->options.rig.ambient * 100.0f));
    SendMessageW(g_options->azimuth, TBM_SETPOS, TRUE,
                 pos(g_options->options.rig.key_azimuth));
    SendMessageW(g_options->elevation, TBM_SETPOS, TRUE,
                 pos(g_options->options.rig.key_elevation));
    SendMessageW(g_options->intensity, TBM_SETPOS, TRUE,
                 pos(g_options->options.rig.key_intensity * 100.0f));
    SendMessageW(g_options->background, TBM_SETPOS, TRUE,
                 pos(g_options->options.rig.background * 100.0f));

    SendMessageW(g_options->sphere, BM_SETCHECK,
                 g_options->options.shape == PreviewShape::Sphere ? BST_CHECKED
                                                                  : BST_UNCHECKED,
                 0);
    SendMessageW(g_options->cube, BM_SETCHECK,
                 g_options->options.shape == PreviewShape::Cube ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_options->cylinder, BM_SETCHECK,
                 g_options->options.shape == PreviewShape::Cylinder ? BST_CHECKED
                                                                    : BST_UNCHECKED,
                 0);
    SendMessageW(g_options->plane, BM_SETCHECK,
                 g_options->options.shape == PreviewShape::Plane ? BST_CHECKED : BST_UNCHECKED,
                 0);

    // Combo: 4 presets + "(custom)" when the rig was hand-edited.
    int selection = 0;
    switch (g_options->options.environment) {
    case PreviewEnvironment::StudioDark:
        selection = 0;
        break;
    case PreviewEnvironment::StudioLight:
        selection = 1;
        break;
    case PreviewEnvironment::Outdoor:
        selection = 2;
        break;
    case PreviewEnvironment::Flat:
        selection = 3;
        break;
    case PreviewEnvironment::Custom:
        selection = 4;
        break;
    }
    SendMessageW(g_options->env_combo, CB_SETCURSEL, selection, 0);

    set_value_labels();
    g_options->updating = false;
}

void apply_current() {
    if (g_options == nullptr) {
        return;
    }
    app::apply_preview_options(g_options->options);
}

void on_slider_change() {
    if (g_options == nullptr || g_options->updating) {
        return;
    }
    PreviewRig rig;
    rig.ambient = static_cast<float>(SendMessageW(g_options->ambient, TBM_GETPOS, 0, 0)) / 100.0f;
    rig.key_azimuth = static_cast<float>(SendMessageW(g_options->azimuth, TBM_GETPOS, 0, 0));
    rig.key_elevation = static_cast<float>(SendMessageW(g_options->elevation, TBM_GETPOS, 0, 0));
    rig.key_intensity =
        static_cast<float>(SendMessageW(g_options->intensity, TBM_GETPOS, 0, 0)) / 100.0f;
    rig.background =
        static_cast<float>(SendMessageW(g_options->background, TBM_GETPOS, 0, 0)) / 100.0f;

    apply_rig(g_options->options, rig);
    // Keep the combo truthful without echoing a selection change.
    g_options->updating = true;
    SendMessageW(g_options->env_combo, CB_SETCURSEL, 4, 0); // custom
    g_options->updating = false;
    set_value_labels();
    apply_current();
}

void on_environment_change() {
    if (g_options == nullptr || g_options->updating) {
        return;
    }
    const LRESULT selection = SendMessageW(g_options->env_combo, CB_GETCURSEL, 0, 0);
    switch (selection) {
    case 0:
        apply_environment(g_options->options, PreviewEnvironment::StudioDark);
        break;
    case 1:
        apply_environment(g_options->options, PreviewEnvironment::StudioLight);
        break;
    case 2:
        apply_environment(g_options->options, PreviewEnvironment::Outdoor);
        break;
    case 3:
        apply_environment(g_options->options, PreviewEnvironment::Flat);
        break;
    default:
        return;
    }
    sync_controls_from_options();
    apply_current();
}

void on_shape_click(int id) {
    if (g_options == nullptr) {
        return;
    }
    switch (id) {
    case IDC_SHAPE_SPHERE:
        g_options->options.shape = PreviewShape::Sphere;
        break;
    case IDC_SHAPE_CUBE:
        g_options->options.shape = PreviewShape::Cube;
        break;
    case IDC_SHAPE_CYLINDER:
        g_options->options.shape = PreviewShape::Cylinder;
        break;
    case IDC_SHAPE_PLANE:
        g_options->options.shape = PreviewShape::Plane;
        break;
    default:
        return;
    }
    apply_current();
}

void layout_options() {
    if (g_options == nullptr || g_options->hwnd == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(g_options->hwnd, &client);
    const int width = static_cast<int>(client.right);
    const int height = static_cast<int>(client.bottom);
    const int label_w = dp(120);
    const int value_w = dp(56);
    const int slider_x = dp(150);
    const int slider_w = std::max(dp(120), width - slider_x - value_w - dp(24));

    int y = dp(16);
    MoveWindow(g_options->sphere, dp(150), y, dp(100), dp(22), TRUE);
    MoveWindow(g_options->cube, dp(255), y, dp(90), dp(22), TRUE);
    MoveWindow(g_options->cylinder, dp(350), y, dp(90), dp(22), TRUE);
    MoveWindow(g_options->plane, dp(445), y, dp(90), dp(22), TRUE);

    y += dp(34);
    MoveWindow(GetDlgItem(g_options->hwnd, IDC_ENV_LABEL), dp(16), y + dp(3), label_w, dp(20),
               TRUE);
    MoveWindow(g_options->env_combo, slider_x, y, dp(200), dp(200), TRUE);

    const int slider_h = dp(22);
    const OptionsId label_ids[] = { IDC_AMBIENT_LABEL, IDC_AZIMUTH_LABEL, IDC_ELEVATION_LABEL,
                                    IDC_INTENSITY_LABEL, IDC_BACKGROUND_LABEL };
    const OptionsId value_ids[] = { IDC_AMBIENT_VALUE, IDC_AZIMUTH_VALUE, IDC_ELEVATION_VALUE,
                                    IDC_INTENSITY_VALUE, IDC_BACKGROUND_VALUE };
    HWND sliders[] = { g_options->ambient, g_options->azimuth, g_options->elevation,
                       g_options->intensity, g_options->background };
    HWND values[] = { g_options->ambient_value, g_options->azimuth_value,
                      g_options->elevation_value, g_options->intensity_value,
                      g_options->background_value };
    y += dp(34);
    for (int i = 0; i < 5; ++i) {
        MoveWindow(GetDlgItem(g_options->hwnd, label_ids[i]), dp(16), y + dp(4), label_w,
                   dp(20), TRUE);
        MoveWindow(sliders[i], slider_x, y, slider_w, slider_h, TRUE);
        MoveWindow(values[i], slider_x + slider_w + dp(4), y + dp(4), value_w, dp(20), TRUE);
        y += dp(30);
    }

    MoveWindow(GetDlgItem(g_options->hwnd, IDC_MODEL), dp(16), y + dp(10), dp(200), dp(28),
               TRUE);
    MoveWindow(GetDlgItem(g_options->hwnd, IDOK), width - dp(104), height - dp(44), dp(88),
               dp(28), TRUE);
}

void create_label(HWND hwnd, int id, const wchar_t* text) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
    SendMessageW(label, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

HWND create_slider(HWND hwnd, int id, int lo, int hi) {
    HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                  WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 0, 0, 0, 0,
                                  hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(slider, TBM_SETRANGEMIN, FALSE, static_cast<LPARAM>(lo));
    SendMessageW(slider, TBM_SETRANGEMAX, FALSE, static_cast<LPARAM>(hi));
    return slider;
}

LRESULT CALLBACK OptionsProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        OptionsState* state = g_options;
        state->hwnd = hwnd;
        state->options = app::preview_options();

        state->sphere = CreateWindowExW(
            0, L"BUTTON", L"Sphere", WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP |
                                        BS_AUTORADIOBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SHAPE_SPHERE)),
            GetModuleHandleW(nullptr), nullptr);
        state->cube = CreateWindowExW(
            0, L"BUTTON", L"Cube", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 0,
            0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SHAPE_CUBE)),
            GetModuleHandleW(nullptr), nullptr);
        state->cylinder = CreateWindowExW(
            0, L"BUTTON", L"Cylinder", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SHAPE_CYLINDER)),
            GetModuleHandleW(nullptr), nullptr);
        state->plane = CreateWindowExW(
            0, L"BUTTON", L"Plane", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 0,
            0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SHAPE_PLANE)),
            GetModuleHandleW(nullptr), nullptr);
        create_label(hwnd, IDC_ENV_LABEL, L"Environment:");
        state->env_combo = CreateWindowExW(
            0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_GROUP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ENV_COMBO)),
            GetModuleHandleW(nullptr), nullptr);
        for (const wchar_t* item :
             { L"Studio dark", L"Studio light", L"Outdoor", L"Flat", L"(custom)" }) {
            SendMessageW(state->env_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }

        create_label(hwnd, IDC_AMBIENT_LABEL, L"Ambient light:");
        create_label(hwnd, IDC_AZIMUTH_LABEL, L"Key light azimuth:");
        create_label(hwnd, IDC_ELEVATION_LABEL, L"Key light elevation:");
        create_label(hwnd, IDC_INTENSITY_LABEL, L"Key light intensity:");
        create_label(hwnd, IDC_BACKGROUND_LABEL, L"Background level:");
        state->ambient = create_slider(hwnd, IDC_AMBIENT, 0, 100);
        state->azimuth = create_slider(hwnd, IDC_AZIMUTH, -180, 180);
        state->elevation = create_slider(hwnd, IDC_ELEVATION, -90, 90);
        state->intensity = create_slider(hwnd, IDC_INTENSITY, 0, 200);
        state->background = create_slider(hwnd, IDC_BACKGROUND, 0, 100);
        state->ambient_value = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AMBIENT_VALUE)),
            GetModuleHandleW(nullptr), nullptr);
        state->azimuth_value = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AZIMUTH_VALUE)),
            GetModuleHandleW(nullptr), nullptr);
        state->elevation_value = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ELEVATION_VALUE)),
            GetModuleHandleW(nullptr), nullptr);
        state->intensity_value = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INTENSITY_VALUE)),
            GetModuleHandleW(nullptr), nullptr);
        state->background_value = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BACKGROUND_VALUE)),
            GetModuleHandleW(nullptr), nullptr);

        HWND model = CreateWindowExW(
            0, L"BUTTON", L"Load model (.mdl)...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MODEL)), GetModuleHandleW(nullptr),
            nullptr);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"Close",
                                  WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 0, 0,
                                  0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                  GetModuleHandleW(nullptr), nullptr);

        for (HWND control : { state->sphere, state->cube, state->cylinder, state->plane,
                              state->env_combo, state->ambient, state->azimuth, state->elevation,
                              state->intensity, state->background, state->ambient_value,
                              state->azimuth_value, state->elevation_value,
                              state->intensity_value, state->background_value, model, ok }) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }

        sync_controls_from_options();
        return 0;
    }

    case WM_SIZE:
        layout_options();
        return 0;

    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) != nullptr) {
            on_slider_change();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_ENV_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                on_environment_change();
            }
            return 0;
        case IDC_SHAPE_SPHERE:
        case IDC_SHAPE_CUBE:
        case IDC_SHAPE_CYLINDER:
        case IDC_SHAPE_PLANE:
            if (HIWORD(wparam) == BN_CLICKED) {
                on_shape_click(LOWORD(wparam));
            }
            return 0;
        case IDC_MODEL:
            app::pick_and_load_model(hwnd);
            return 0;
        case IDOK:
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            return 0;
        }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        app::commit_preview_options();
        if (g_options != nullptr) {
            g_options->running = false;
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

} // namespace

void show_preview_options_dialog(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = OptionsProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
        wc.lpszClassName = kOptionsClass;
        if (RegisterClassExW(&wc) == 0) {
            return;
        }
        registered = true;
    }

    OptionsState state;
    {
        HDC screen = GetDC(nullptr);
        if (screen != nullptr) {
            const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
            if (dpi > 0) {
                state.dpi = dpi;
            }
            ReleaseDC(nullptr, screen);
        }
    }
    g_options = &state;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kOptionsClass, kOptionsTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(620, state.dpi, 96), MulDiv(420, state.dpi, 96), owner, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        g_options = nullptr;
        return;
    }

    RECT window_rect{};
    GetWindowRect(hwnd, &window_rect);
    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) -
                                     (window_rect.right - window_rect.left)) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) -
                                    (window_rect.bottom - window_rect.top)) / 2;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    EnableWindow(owner, FALSE);
    MSG message{};
    while (state.running && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &message) == FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    g_options = nullptr;
}

} // namespace ui
