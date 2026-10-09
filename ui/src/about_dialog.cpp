#include "about_dialog.h"

#include <string>

namespace ui {
namespace {

constexpr wchar_t kAboutClass[] = L"VMTViewerAbout";
constexpr wchar_t kAboutTitle[] = L"About VMT Viewer";

const wchar_t kAboutText[] =
    L"VMT Viewer\r\n"
    L"Viewer/editor for Source Engine VMT materials and VTF textures.\r\n"
    L"\r\n"
    L"No Valve files are redistributed.\r\n"
    L"This application ships no game content and no Valve or Steam binaries. It "
    L"reads materials, textures and engine DLLs from a game installation on this "
    L"machine, selected by the user under Game > Connection. Nothing is copied out "
    L"of the game folder; engine binaries are loaded in place at runtime, only "
    L"while a preview is running.\r\n"
    L"\r\n"
    L"VTF metadata/decoding uses VTFLib++ (LGPL-2.1, see thirdparty/vtflib), "
    L"linked as a DLL and replaceable at runtime. VMT parsing, editing, "
    L"validation, the browser, thumbnails and the preview controls are original "
    L"code with no Valve dependencies.\r\n"
    L"\r\n"
    L"Preview: sphere, cube, cylinder and plane primitives, engine-rendered "
    L"material thumbnails cached on disk, light rig with environment presets. "
    L"Model (.mdl) preview is pending the StudioRender headers.\r\n"
    L"\r\n"
    L"Logs: %LOCALAPPDATA%\\VMTViewer\\vmtviewer.log (viewer) and "
    L"%TEMP%\\VMTViewer\\engine.log (engine).";

struct AboutState {
    HWND hwnd = nullptr;
    bool running = true;
    int dpi = 96;
};

AboutState* g_about = nullptr;

int dp(int value) {
    return g_about != nullptr ? MulDiv(value, g_about->dpi, 96) : value;
}

LRESULT CALLBACK AboutProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        AboutState* state = g_about;
        state->hwnd = hwnd;
        HWND text = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", kAboutText,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0,
            0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(10)),
            GetModuleHandleW(nullptr), nullptr);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
                                  WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 0, 0,
                                  0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                  GetModuleHandleW(nullptr), nullptr);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

        RECT client{};
        GetClientRect(hwnd, &client);
        MoveWindow(text, dp(12), dp(12), client.right - dp(24), client.bottom - dp(62), TRUE);
        MoveWindow(ok, client.right - dp(100), client.bottom - dp(42), dp(88), dp(28), TRUE);
        return 0;
    }

    case WM_SIZE: {
        HWND text = GetDlgItem(hwnd, 10);
        HWND ok = GetDlgItem(hwnd, IDOK);
        if (text != nullptr && ok != nullptr) {
            RECT client{};
            GetClientRect(hwnd, &client);
            MoveWindow(text, dp(12), dp(12), client.right - dp(24), client.bottom - dp(62), TRUE);
            MoveWindow(ok, client.right - dp(100), client.bottom - dp(42), dp(88), dp(28), TRUE);
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_about != nullptr) {
            g_about->running = false;
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

} // namespace

void show_about_dialog(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = AboutProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
        wc.lpszClassName = kAboutClass;
        if (RegisterClassExW(&wc) == 0) {
            return;
        }
        registered = true;
    }

    AboutState state;
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
    g_about = &state;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kAboutClass, kAboutTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(560, state.dpi, 96), MulDiv(430, state.dpi, 96), owner, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        g_about = nullptr;
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
    g_about = nullptr;
}

} // namespace ui
