#include "folder_picker.h"

#include <shobjidl.h>

#include <string>

namespace ui {
namespace {

// Minimal COM smart pointer: the project has no ATL, and the picker only ever
// owns two interfaces.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    // Address for IID_PPV_ARGS: releases whatever was held first.
    T** put() {
        reset();
        return &ptr_;
    }
    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    void reset() noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }
    T* ptr_ = nullptr;
};

// COM apartment lifetime for one call. CoInitializeEx is per-thread and
// reference counted: it must be balanced with exactly one CoUninitialize per
// successful (S_OK or S_FALSE) call, and none at all on RPC_E_CHANGED_MODE,
// which means the thread was already initialised in another mode by someone
// else - COM is usable, we just do not own it.
class ComApartment {
public:
    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                    COINIT_DISABLE_OLE1DDE);
        owned_ = SUCCEEDED(hr);
    }
    ~ComApartment() {
        if (owned_) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool owned_ = false;
};

// The folder the dialog should open in: the requested path when it exists,
// its nearest existing ancestor otherwise, nothing for the shell default.
std::filesystem::path resolve_start(const std::filesystem::path& start_in) {
    if (start_in.empty()) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path current = start_in;
    for (int depth = 0; depth < 16; ++depth) {
        ec.clear();
        if (std::filesystem::is_directory(current, ec)) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }
    return {};
}

// IFileDialog shows its own shell picker; every failure (COM unavailable, the
// shell view refusing to start) is reported as "cancelled", which is what the
// callers already treat as "leave the current value alone".
std::filesystem::path show_picker(HWND owner, const wchar_t* title,
                                  const std::filesystem::path& start_in) {
    ComApartment apartment;

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.put()));
    if (FAILED(hr) || !dialog) {
        return {};
    }

    DWORD options = 0;
    hr = dialog->GetOptions(&options);
    if (FAILED(hr)) {
        return {};
    }
    // PICKFOLDERS is what makes this a folder chooser rather than a file one.
    // FORCEFILESYSTEM keeps the result a real path instead of a virtual item
    // (a library or a network share placeholder), which is what the callers
    // feed into the engine and the settings file.
    hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                            FOS_PATHMUSTEXIST);
    if (FAILED(hr)) {
        return {};
    }
    if (title != nullptr && *title != L'\0') {
        (void)dialog->SetTitle(title);
    }

    const std::filesystem::path start = resolve_start(start_in);
    if (!start.empty()) {
        ComPtr<IShellItem> folder;
        hr = SHCreateItemFromParsingName(start.c_str(), nullptr,
                                         IID_PPV_ARGS(folder.put()));
        if (SUCCEEDED(hr) && folder) {
            (void)dialog->SetFolder(folder.get());
        }
    }

    hr = dialog->Show(owner);
    if (hr != S_OK) { // cancel, or an error the shell already reported
        return {};
    }

    ComPtr<IShellItem> chosen;
    hr = dialog->GetResult(chosen.put());
    if (FAILED(hr) || !chosen) {
        return {};
    }

    PWSTR raw_path = nullptr;
    hr = chosen->GetDisplayName(SIGDN_FILESYSPATH, &raw_path);
    if (FAILED(hr) || raw_path == nullptr) {
        return {};
    }
    const std::filesystem::path result(raw_path);
    CoTaskMemFree(raw_path);
    return result;
}

} // namespace

std::filesystem::path pick_folder(HWND owner, const wchar_t* title,
                                  const std::filesystem::path& start_in) {
    return show_picker(owner, title, start_in);
}

} // namespace ui