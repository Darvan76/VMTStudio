#include "connection_dialog.h"

#include "ui/connection_model.h"

#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <vector>

namespace ui {
namespace {

constexpr wchar_t kDialogClass[] = L"VMTViewerConnectionDialog";
constexpr wchar_t kTitle[] = L"Game connection";

enum DialogId : int {
    IDC_PATH = 200,
    IDC_BROWSE,
    IDC_GAME_EDIT,
    IDC_GAME_BROWSE,
    IDC_TEST,
    IDC_ARCH_VALUE,
    IDC_GAME_VALUE,
    IDC_RESULTS,
};

struct DialogState {
    HWND hwnd = nullptr;
    HWND path_edit = nullptr;
    HWND browse_button = nullptr;
    HWND game_edit = nullptr;
    HWND game_browse_button = nullptr;
    HWND test_button = nullptr;
    HWND arch_value = nullptr;
    HWND game_value = nullptr;
    HWND results = nullptr;
    HWND close_button = nullptr;
    UISettings* settings = nullptr;
    std::filesystem::path settings_path;
    bool running = true;
    int dpi = 96;
};

DialogState* g_dialog = nullptr;

int dp(int value) {
    return g_dialog != nullptr ? MulDiv(value, g_dialog->dpi, 96) : value;
}

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        length);
    return wide;
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

void remember_path() {
    if (g_dialog == nullptr || g_dialog->settings == nullptr) {
        return;
    }
    const std::wstring bin = window_text(g_dialog->path_edit);
    const std::wstring game = window_text(g_dialog->game_edit);
    g_dialog->settings->last_bin_dir = std::filesystem::path(bin).string();
    g_dialog->settings->last_game_dir = std::filesystem::path(game).string();
    if (!g_dialog->settings_path.empty()) {
        g_dialog->settings->save(g_dialog->settings_path);
    }
}

void browse_folder(std::wstring& out) {
    BROWSEINFOW info{};
    info.hwndOwner = g_dialog->hwnd;
    info.lpszTitle = L"Select the folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = SHBrowseForFolderW(&info);
    if (idl == nullptr) {
        return;
    }
    wchar_t path[4 * MAX_PATH] = {};
    if (SHGetPathFromIDListW(idl, path) != FALSE) {
        out = path;
    }
    CoTaskMemFree(idl);
}

void browse_bin() {
    std::wstring path;
    browse_folder(path);
    if (path.empty()) {
        return;
    }
    SetWindowTextW(g_dialog->path_edit, path.c_str());

    // Auto-fill the game folder when it is still empty.
    if (window_text(g_dialog->game_edit).empty()) {
        const std::filesystem::path game = detect_game_dir(std::filesystem::path(path));
        if (!game.empty()) {
            SetWindowTextW(g_dialog->game_edit, game.wstring().c_str());
        }
    }
    remember_path();
}

void browse_game() {
    std::wstring path;
    browse_folder(path);
    if (path.empty()) {
        return;
    }
    SetWindowTextW(g_dialog->game_edit, path.c_str());
    remember_path();
}

void run_connection_test() {
    const std::wstring wide = window_text(g_dialog->path_edit);
    if (wide.empty()) {
        MessageBoxW(g_dialog->hwnd, L"Choose the game's /bin folder first.", kTitle,
                    MB_ICONINFORMATION | MB_OK);
        return;
    }

    // Best-effort game folder fill if the user never set it.
    if (window_text(g_dialog->game_edit).empty()) {
        const std::filesystem::path game = detect_game_dir(std::filesystem::path(wide));
        if (!game.empty()) {
            SetWindowTextW(g_dialog->game_edit, game.wstring().c_str());
        }
    }

    // A game folder without gameinfo.txt cannot be mounted; auto-correct from
    // /bin when possible (common mistake: picking the game root).
    bool gameinfo_ok = false;
    {
        std::error_code ec;
        const std::filesystem::path game(std::wstring(window_text(g_dialog->game_edit)));
        gameinfo_ok = !game.empty() && std::filesystem::is_regular_file(game / L"gameinfo.txt", ec);
        if (!gameinfo_ok) {
            const std::filesystem::path detected = detect_game_dir(std::filesystem::path(wide));
            if (!detected.empty()) {
                SetWindowTextW(g_dialog->game_edit, detected.wstring().c_str());
                gameinfo_ok = true;
            }
        }
    }

    remember_path();
    const ConnectionCheck check = test_connection(std::filesystem::path(wide));

    SetWindowTextW(g_dialog->arch_value, to_wide(check.architecture).c_str());

    std::string report = check.summary + "\r\n\r\n";
    for (const std::string& line : check.lines) {
        report += line + "\r\n";
    }
    if (!gameinfo_ok) {
        report += "\r\n[WARN] the game folder has no gameinfo.txt; the engine preview "
                  "needs the mod folder (for example ...\\garrysmod or ...\\hl2).\r\n";
    }
    SetWindowTextW(g_dialog->results, to_wide(report).c_str());
}

void layout_dialog() {
    RECT client{};
    GetClientRect(g_dialog->hwnd, &client);
    const int width = static_cast<int>(client.right);
    const int height = static_cast<int>(client.bottom);

    MoveWindow(g_dialog->path_edit, dp(100), dp(14), std::max(dp(100), width - dp(230)), dp(24),
               TRUE);
    MoveWindow(g_dialog->browse_button, width - dp(122), dp(14), dp(104), dp(24), TRUE);
    MoveWindow(g_dialog->game_edit, dp(100), dp(46), std::max(dp(100), width - dp(230)), dp(24),
               TRUE);
    MoveWindow(g_dialog->game_browse_button, width - dp(122), dp(46), dp(104), dp(24), TRUE);
    MoveWindow(g_dialog->test_button, dp(100), dp(78), dp(150), dp(26), TRUE);
    MoveWindow(g_dialog->arch_value, dp(100), dp(112), width - dp(116), dp(20), TRUE);
    MoveWindow(g_dialog->game_value, dp(100), dp(136), width - dp(116), dp(20), TRUE);
    MoveWindow(g_dialog->results, dp(100), dp(164), std::max(dp(100), width - dp(116)),
               std::max(dp(60), height - dp(220)), TRUE);
    MoveWindow(g_dialog->close_button, width - dp(104), height - dp(44), dp(88), dp(28), TRUE);
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        DialogState* state = g_dialog;
        state->hwnd = hwnd;

        HWND label_path = CreateWindowExW(0, L"STATIC", L"/bin folder:", WS_CHILD | WS_VISIBLE,
                                          dp(12), dp(18), dp(84), dp(20), hwnd, nullptr,
                                          GetModuleHandleW(nullptr), nullptr);
        state->path_edit = CreateWindowExW(0, L"EDIT", L"",
                                           WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                           0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PATH)),
                                           GetModuleHandleW(nullptr), nullptr);
        state->browse_button = CreateWindowExW(0, L"BUTTON", L"Browse...",
                                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                               0, 0, 0, 0, hwnd,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE)),
                                               GetModuleHandleW(nullptr), nullptr);
        HWND label_game_edit = CreateWindowExW(0, L"STATIC", L"Game folder:",
                                               WS_CHILD | WS_VISIBLE, dp(12), dp(50), dp(84),
                                               dp(20), hwnd, nullptr, GetModuleHandleW(nullptr),
                                               nullptr);
        state->game_edit = CreateWindowExW(0, L"EDIT", L"",
                                           WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                           0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(
                                               static_cast<INT_PTR>(IDC_GAME_EDIT)),
                                           GetModuleHandleW(nullptr), nullptr);
        state->game_browse_button = CreateWindowExW(
            0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0,
            0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_GAME_BROWSE)),
            GetModuleHandleW(nullptr), nullptr);
        state->test_button = CreateWindowExW(
            0, L"BUTTON", L"Test connection",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEST)), GetModuleHandleW(nullptr),
            nullptr);
        HWND label_arch = CreateWindowExW(0, L"STATIC", L"Architecture:", WS_CHILD | WS_VISIBLE,
                                          dp(12), dp(114), dp(84), dp(18), hwnd, nullptr,
                                          GetModuleHandleW(nullptr), nullptr);
        state->arch_value = CreateWindowExW(0, L"STATIC", L"(not tested yet)",
                                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                            reinterpret_cast<HMENU>(
                                                static_cast<INT_PTR>(IDC_ARCH_VALUE)),
                                            GetModuleHandleW(nullptr), nullptr);
        HWND label_game = CreateWindowExW(0, L"STATIC", L"Game name:", WS_CHILD | WS_VISIBLE,
                                          dp(12), dp(138), dp(84), dp(18), hwnd, nullptr,
                                          GetModuleHandleW(nullptr), nullptr);
        state->game_value = CreateWindowExW(
            0, L"STATIC", L"pending (game_profiles API not delivered)", WS_CHILD | WS_VISIBLE, 0, 0,
            0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_GAME_VALUE)),
            GetModuleHandleW(nullptr), nullptr);
        state->results = CreateWindowExW(0, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                         0, 0, 0, 0, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESULTS)),
                                         GetModuleHandleW(nullptr), nullptr);
        state->close_button = CreateWindowExW(
            0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandleW(nullptr),
            nullptr);

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND control : {label_path, state->path_edit, state->browse_button,
                             label_game_edit, state->game_edit, state->game_browse_button,
                             state->test_button, label_arch, state->arch_value, label_game,
                             state->game_value, state->results, state->close_button}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }

        if (state->settings != nullptr && !state->settings->last_bin_dir.empty()) {
            SetWindowTextW(state->path_edit, std::filesystem::path(state->settings->last_bin_dir)
                                                 .wstring()
                                                 .c_str());
        }
        if (state->settings != nullptr && !state->settings->last_game_dir.empty()) {
            SetWindowTextW(state->game_edit, std::filesystem::path(state->settings->last_game_dir)
                                                 .wstring()
                                                 .c_str());
        }
        return 0;
    }

    case WM_SIZE:
        layout_dialog();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_BROWSE:
            browse_bin();
            return 0;
        case IDC_GAME_BROWSE:
            browse_game();
            return 0;
        case IDC_TEST:
            run_connection_test();
            return 0;
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
        remember_path();
        if (g_dialog != nullptr) {
            g_dialog->running = false;
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

} // namespace

void show_connection_dialog(HWND owner, UISettings& settings,
                            const std::filesystem::path& settings_path) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
        wc.lpszClassName = kDialogClass;
        if (RegisterClassExW(&wc) == 0) {
            return;
        }
        registered = true;
    }

    DialogState state;
    state.settings = &settings;
    state.settings_path = settings_path;
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
    g_dialog = &state;

    const bool com_ready = SUCCEEDED(OleInitialize(nullptr));

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kDialogClass, kTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(620, state.dpi, 96), MulDiv(460, state.dpi, 96), owner, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        g_dialog = nullptr;
        if (com_ready) {
            OleUninitialize();
        }
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
    g_dialog = nullptr;
    if (com_ready) {
        OleUninitialize();
    }
}

} // namespace ui
