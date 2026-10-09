#include "material_browser_window.h"

#include "app_hooks.h"
#include "ui/material_browser.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ui {
namespace {

constexpr wchar_t kBrowserClass[] = L"VMTViewerMaterialBrowser";
constexpr wchar_t kBrowserTitle[] = L"Material browser";
constexpr UINT kAutoScanMessage = WM_APP + 41;

// A very large tree makes the shell sluggish; extra matches stay out of the
// tree but are reported in the status line.
constexpr std::size_t kMaxTreeItems = 4000;

enum BrowserId : int {
    IDC_SEARCH = 300,
    IDC_SCAN_GAME,
    IDC_SCAN_FOLDER,
    IDC_TREE,
    IDC_THUMB,
    IDC_INFO,
    IDC_OPEN,
    IDC_FLIP,
    IDC_STATUS,
};

struct BrowserState {
    HWND hwnd = nullptr;
    HWND search = nullptr;
    HWND scan_game = nullptr;
    HWND scan_folder = nullptr;
    HWND tree = nullptr;
    HWND thumb = nullptr;
    HWND info = nullptr;
    HWND open_button = nullptr;
    HWND flip = nullptr;
    HWND status = nullptr;

    MaterialIndex index;
    std::vector<std::size_t> filtered;
    HBITMAP bitmap = nullptr;
    int dpi = 96;
    bool scanning = false;
    std::wstring source_label;
};

BrowserState* g_browser = nullptr;

int dp(int value) {
    return g_browser != nullptr ? MulDiv(value, g_browser->dpi, 96) : value;
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

std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                        length, nullptr, nullptr);
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

void set_font(HWND hwnd) {
    SendMessageW(hwnd, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

void set_status(const std::wstring& text) {
    if (g_browser != nullptr && g_browser->status != nullptr) {
        SetWindowTextW(g_browser->status, text.c_str());
    }
}

void set_info(const std::wstring& text) {
    if (g_browser != nullptr && g_browser->info != nullptr) {
        SetWindowTextW(g_browser->info, text.c_str());
    }
}

void clear_thumbnail() {
    if (g_browser == nullptr || g_browser->thumb == nullptr) {
        return;
    }
    SendMessageW(g_browser->thumb, STM_SETIMAGE, IMAGE_BITMAP, 0);
    if (g_browser->bitmap != nullptr) {
        DeleteObject(g_browser->bitmap);
        g_browser->bitmap = nullptr;
    }
}

void show_thumbnail(const std::filesystem::path& bmp_path) {
    clear_thumbnail();
    if (g_browser == nullptr || g_browser->thumb == nullptr) {
        return;
    }
    HBITMAP bitmap = static_cast<HBITMAP>(LoadImageW(
        nullptr, bmp_path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    if (bitmap == nullptr) {
        return;
    }
    g_browser->bitmap = bitmap;
    SendMessageW(g_browser->thumb, STM_SETIMAGE, IMAGE_BITMAP,
                 reinterpret_cast<LPARAM>(bitmap));
}

// Selected leaf entry; null for folder nodes or an empty selection.
const MaterialEntry* selected_entry() {
    if (g_browser == nullptr || g_browser->tree == nullptr) {
        return nullptr;
    }
    const HTREEITEM selection = TreeView_GetSelection(g_browser->tree);
    if (selection == nullptr) {
        return nullptr;
    }
    TVITEMW item{};
    item.mask = TVIF_PARAM;
    item.hItem = selection;
    if (TreeView_GetItem(g_browser->tree, &item) == FALSE || item.lParam == 0) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(item.lParam - 1);
    if (index >= g_browser->index.entries().size()) {
        return nullptr;
    }
    return &g_browser->index.entries()[index];
}

// Applies the search filter and rebuilds the tree. With an empty query the
// tree groups materials by folder; with a query it becomes one flat list of
// matches (folder grouping a filtered set is more confusing than useful).
void rebuild_tree() {
    if (g_browser == nullptr || g_browser->tree == nullptr) {
        return;
    }
    clear_thumbnail();
    TreeView_DeleteAllItems(g_browser->tree);

    const std::wstring query = window_text(g_browser->search);
    g_browser->filtered = g_browser->index.search(to_utf8(query));

    const auto& entries = g_browser->index.entries();
    if (entries.empty()) {
        set_status(L"Nothing scanned yet: use \"Scan game\" or \"Scan folder...\"");
        set_info(L"");
        return;
    }

    const std::size_t shown =
        std::min<std::size_t>(g_browser->filtered.size(), kMaxTreeItems);

    HTREEITEM root = nullptr;
    {
        std::wstring root_label = g_browser->source_label.empty() ? std::wstring(L"Materials")
                                                                  : g_browser->source_label;
        root_label += L" (" + std::to_wstring(g_browser->filtered.size()) + L")";
        TVINSERTSTRUCTW root_item{};
        root_item.hParent = TVI_ROOT;
        root_item.hInsertAfter = TVI_LAST;
        root_item.item.mask = TVIF_TEXT;
        root_item.item.pszText = root_label.data();
        root = reinterpret_cast<HTREEITEM>(SendMessageW(g_browser->tree, TVM_INSERTITEMW, 0,
                                                        reinterpret_cast<LPARAM>(&root_item)));
    }

    if (query.empty()) {
        std::map<std::wstring, HTREEITEM> folders;
        for (std::size_t i = 0; i < shown; ++i) {
            const std::size_t entry_index = g_browser->filtered[i];
            const std::string& name = entries[entry_index].name;
            const std::wstring leaf_label = to_wide(name.substr(name.find_last_of('/') + 1));

            HTREEITEM parent = root;
            std::wstring prefix;
            std::size_t start = 0;
            for (std::size_t pos = 0; pos <= name.size(); ++pos) {
                const bool at_end = pos == name.size();
                if (!at_end && name[pos] != '/') {
                    continue;
                }
                const std::string component = name.substr(start, pos - start);
                start = pos + 1;
                if (component.empty()) {
                    continue;
                }
                if (!at_end) {
                    if (!prefix.empty()) {
                        prefix += L'/';
                    }
                    prefix += to_wide(component);
                    auto found = folders.find(prefix);
                    if (found == folders.end()) {
                        std::wstring label = to_wide(component);
                        TVINSERTSTRUCTW node{};
                        node.hParent = parent;
                        node.hInsertAfter = TVI_SORT;
                        node.item.mask = TVIF_TEXT;
                        node.item.pszText = label.data();
                        HTREEITEM item = reinterpret_cast<HTREEITEM>(SendMessageW(
                            g_browser->tree, TVM_INSERTITEMW, 0,
                            reinterpret_cast<LPARAM>(&node)));
                        folders.emplace(prefix, item);
                        parent = item;
                    } else {
                        parent = found->second;
                    }
                } else {
                    std::wstring label = leaf_label;
                    TVINSERTSTRUCTW leaf{};
                    leaf.hParent = parent;
                    leaf.hInsertAfter = TVI_SORT;
                    leaf.item.mask = TVIF_TEXT | TVIF_PARAM;
                    leaf.item.pszText = label.data();
                    leaf.item.lParam = static_cast<LPARAM>(entry_index + 1);
                    SendMessageW(g_browser->tree, TVM_INSERTITEMW, 0,
                                 reinterpret_cast<LPARAM>(&leaf));
                }
            }
        }
    } else {
        for (std::size_t i = 0; i < shown; ++i) {
            const std::size_t entry_index = g_browser->filtered[i];
            std::wstring label = to_wide(entries[entry_index].display);
            TVINSERTSTRUCTW leaf{};
            leaf.hParent = root;
            leaf.hInsertAfter = TVI_SORT;
            leaf.item.mask = TVIF_TEXT | TVIF_PARAM;
            leaf.item.pszText = label.data();
            leaf.item.lParam = static_cast<LPARAM>(entry_index + 1);
            SendMessageW(g_browser->tree, TVM_INSERTITEMW, 0,
                         reinterpret_cast<LPARAM>(&leaf));
        }
    }

    TreeView_Expand(g_browser->tree, root, TVE_EXPAND);

    std::wstring status = std::to_wstring(g_browser->filtered.size()) + L" material(s)";
    if (g_browser->index.truncated()) {
        status += L", scan capped at " + std::to_wstring(MaterialIndex::kMaxEntries);
    }
    if (shown < g_browser->filtered.size()) {
        status += L", showing the first " + std::to_wstring(shown);
    }
    set_status(status);
}

void on_tree_selection() {
    const MaterialEntry* entry = selected_entry();
    if (entry == nullptr) {
        set_info(L"");
        clear_thumbnail();
        return;
    }

    const std::string& name = entry->name;
    std::wstring label = to_wide(name);
    if (!entry->disk_path.empty()) {
        label += L"\n" + entry->disk_path.wstring();
    } else if (entry->from_engine) {
        label += L"\n(from the game file system; may live inside a VPK)";
    }

    std::filesystem::path bmp;
    std::wstring error;
    set_status(L"rendering thumbnail through the engine...");
    UpdateWindow(g_browser->hwnd);

    if (app::render_thumbnail(name, bmp, error)) {
        show_thumbnail(bmp);
        set_info(label + L"\n\nDouble-click or press Open to edit this material.");
        set_status(L"thumbnail cached: " + bmp.filename().wstring());
    } else {
        clear_thumbnail();
        set_info(label + L"\n\n(no engine thumbnail: " + error + L")");
        set_status(error.empty() ? L"thumbnail unavailable" : error);
    }
}

void open_selected() {
    const MaterialEntry* entry = selected_entry();
    if (entry == nullptr) {
        set_status(L"Select a material first");
        return;
    }

    // A material found by a folder scan opens straight from disk, so the
    // browser works without any game connection. Engine entries (possibly
    // inside a VPK) go through the engine file system.
    if (!entry->disk_path.empty()) {
        if (app::open_material_file(entry->disk_path)) {
            set_status(L"opened " + to_wide(entry->name) +
                       L" - edits update the engine preview");
        } else {
            set_status(L"the material could not be opened");
        }
        return;
    }

    std::wstring error;
    if (app::open_game_material(entry->name, error)) {
        set_status(L"opened " + to_wide(entry->name) +
                   L" - edits update the engine preview");
    } else {
        set_status(error.empty() ? L"the material could not be opened" : error);
    }
}

void scan_game() {
    if (g_browser == nullptr || g_browser->scanning) {
        return;
    }
    g_browser->scanning = true;
    set_status(L"starting the engine and scanning the game file system...");
    UpdateWindow(g_browser->hwnd);

    std::vector<std::string> names;
    bool truncated = false;
    std::wstring error;
    const bool ok = app::game_materials(names, truncated, error);
    if (ok) {
        g_browser->index.clear();
        const std::size_t added = g_browser->index.add_engine_names(names);
        g_browser->source_label = L"Game materials";
        app::remember_browser_source(true, {});
        rebuild_tree();
        std::wstring status = L"game scan: " + std::to_wstring(added) + L" material(s)";
        if (truncated) {
            status += L" (file system reported more)";
        }
        set_status(status);
    } else {
        set_status(error.empty() ? L"the game file system could not be scanned" : error);
    }
    g_browser->scanning = false;
}

void scan_folder(const std::filesystem::path& root) {
    if (g_browser == nullptr || root.empty() || g_browser->scanning) {
        return;
    }
    g_browser->scanning = true;
    set_status(L"scanning " + root.wstring() + L"...");
    UpdateWindow(g_browser->hwnd);

    g_browser->index.clear();
    const std::size_t added = g_browser->index.add_directory(root);
    g_browser->source_label = L"Folder: " + root.wstring();
    app::remember_browser_source(false, root);
    rebuild_tree();
    set_status(L"folder scan: " + std::to_wstring(added) + L" material(s) in " +
               root.wstring());
    g_browser->scanning = false;
}

void scan_folder_dialog() {
    if (g_browser == nullptr || g_browser->scanning) {
        return;
    }
    BROWSEINFOW info{};
    info.hwndOwner = g_browser->hwnd;
    info.lpszTitle = L"Select a folder to scan for .vmt materials";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = SHBrowseForFolderW(&info);
    if (idl == nullptr) {
        return;
    }
    wchar_t buffer[4 * MAX_PATH] = {};
    const bool got_path = SHGetPathFromIDListW(idl, buffer) != FALSE;
    CoTaskMemFree(idl);
    if (got_path) {
        scan_folder(std::filesystem::path(buffer));
    }
}

void layout_browser() {
    if (g_browser == nullptr || g_browser->hwnd == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(g_browser->hwnd, &client);
    const int width = static_cast<int>(client.right);
    const int height = static_cast<int>(client.bottom);

    const int pad = dp(8);
    const int button_w = dp(120);
    const int button_h = dp(24);
    const int search_h = dp(22);
    const int flip_h = dp(22);
    const int status_h = dp(20);

    MoveWindow(g_browser->search, pad, pad, std::max(dp(80), width - 2 * button_w - 4 * pad),
               search_h, TRUE);
    MoveWindow(g_browser->scan_game, width - 2 * button_w - 2 * pad, pad, button_w, button_h,
               TRUE);
    MoveWindow(g_browser->scan_folder, width - button_w - pad, pad, button_w, button_h, TRUE);

    const int content_top = pad + button_h + dp(6);
    const int content_height =
        std::max(dp(80), height - content_top - status_h - flip_h - pad);
    const int tree_width = std::max(dp(160), (width - 3 * pad) * 3 / 5);
    const int right_x = pad + tree_width + pad;
    const int right_width = std::max(dp(120), width - right_x - pad);
    const int thumb_size = dp(160);

    MoveWindow(g_browser->tree, pad, content_top, tree_width, content_height, TRUE);
    MoveWindow(g_browser->thumb, right_x, content_top, thumb_size, thumb_size, TRUE);
    MoveWindow(g_browser->info, right_x, content_top + thumb_size + dp(8), right_width,
               std::max(dp(40), content_height - thumb_size - dp(8) - button_h - dp(8)), TRUE);
    MoveWindow(g_browser->open_button, right_x, content_top + content_height - button_h,
               right_width, button_h, TRUE);
    MoveWindow(g_browser->flip, pad, height - status_h - flip_h - dp(4), width - 2 * pad,
               flip_h, TRUE);
    MoveWindow(g_browser->status, pad, height - status_h - dp(2), width - 2 * pad, status_h,
               TRUE);
}

LRESULT CALLBACK BrowserProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        BrowserState* state = g_browser;
        state->hwnd = hwnd;

        state->search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                        hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH)),
                                        GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state->search, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"Search materials (name contains...)"));

        state->scan_game = CreateWindowExW(
            0, L"BUTTON", L"Scan game", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0,
            0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SCAN_GAME)),
            GetModuleHandleW(nullptr), nullptr);
        state->scan_folder = CreateWindowExW(
            0, L"BUTTON", L"Scan folder...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SCAN_FOLDER)),
            GetModuleHandleW(nullptr), nullptr);
        state->tree = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TREE)),
            GetModuleHandleW(nullptr), nullptr);
        state->thumb = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                                       WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE, 0,
                                       0, 0, 0, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_THUMB)),
                                       GetModuleHandleW(nullptr), nullptr);
        state->info = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                      0, 0, 0, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INFO)),
                                      GetModuleHandleW(nullptr), nullptr);
        state->open_button = CreateWindowExW(
            0, L"BUTTON", L"Open material", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OPEN)),
            GetModuleHandleW(nullptr), nullptr);
        state->flip = CreateWindowExW(
            0, L"BUTTON", L"Flip thumbnails vertically (engine readback orientation)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FLIP)), GetModuleHandleW(nullptr),
            nullptr);
        SendMessageW(state->flip, BM_SETCHECK, app::thumbnail_flip() ? BST_CHECKED : BST_UNCHECKED,
                     0);
        state->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                        0, 0, 0, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)),
                                        GetModuleHandleW(nullptr), nullptr);

        for (HWND control : { state->search, state->scan_game, state->scan_folder, state->tree,
                              state->thumb, state->info, state->open_button, state->flip,
                              state->status }) {
            set_font(control);
        }

        rebuild_tree();
        return 0;
    }

    case WM_SIZE:
        layout_browser();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = dp(560);
        info->ptMinTrackSize.y = dp(360);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_SEARCH:
            if (HIWORD(wparam) == EN_CHANGE) {
                rebuild_tree();
            }
            return 0;
        case IDC_SCAN_GAME:
            scan_game();
            return 0;
        case IDC_SCAN_FOLDER:
            scan_folder_dialog();
            return 0;
        case IDC_OPEN:
            open_selected();
            return 0;
        case IDC_FLIP:
            if (HIWORD(wparam) == BN_CLICKED) {
                app::set_thumbnail_flip(
                    SendMessageW(g_browser->flip, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (selected_entry() != nullptr) {
                    on_tree_selection(); // refresh with the new orientation
                }
            }
            return 0;
        default:
            return 0;
        }

    case WM_NOTIFY: {
        const NMHDR* header = reinterpret_cast<const NMHDR*>(lparam);
        if (g_browser != nullptr && header != nullptr && header->hwndFrom == g_browser->tree) {
            if (header->code == TVN_SELCHANGEDW) {
                on_tree_selection();
            } else if (header->code == NM_DBLCLK) {
                open_selected();
            }
        }
        return 0;
    }

    case kAutoScanMessage: {
        const std::string root = app::last_browser_root();
        if (!root.empty()) {
            scan_folder(std::filesystem::path(to_wide(root)));
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        clear_thumbnail();
        if (g_browser != nullptr) {
            g_browser->hwnd = nullptr;
            delete g_browser;
            g_browser = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

void show_material_browser(HWND owner) {
    if (g_browser != nullptr && g_browser->hwnd != nullptr) {
        SetForegroundWindow(g_browser->hwnd);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = BrowserProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
        wc.lpszClassName = kBrowserClass;
        if (RegisterClassExW(&wc) == 0) {
            return;
        }
        registered = true;
    }

    std::unique_ptr<BrowserState> state(new BrowserState());
    {
        HDC screen = GetDC(nullptr);
        if (screen != nullptr) {
            const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
            if (dpi > 0) {
                state->dpi = dpi;
            }
            ReleaseDC(nullptr, screen);
        }
    }
    g_browser = state.get();

    HWND hwnd = CreateWindowExW(0, kBrowserClass, kBrowserTitle,
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(760, state->dpi, 96), MulDiv(540, state->dpi, 96), owner,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        g_browser = nullptr;
        return;
    }

    // The window now owns the state (deleted in WM_DESTROY).
    state.release();
    layout_browser();

    // Restore the last folder scan (fast, no engine); game scans stay manual
    // so opening the browser never starts the engine by surprise.
    if (!g_browser->scanning && !app::last_browser_root().empty()) {
        PostMessageW(hwnd, kAutoScanMessage, 0, 0);
    }
}

void destroy_material_browser() {
    if (g_browser != nullptr && g_browser->hwnd != nullptr) {
        DestroyWindow(g_browser->hwnd);
    }
}

} // namespace ui
