#include "ui/atomic_file.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace ui {
namespace {

class TemporaryFile {
public:
    TemporaryFile(std::filesystem::path path, HANDLE handle)
        : path_(std::move(path)), handle_(handle) {}
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    ~TemporaryFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        if (!committed_) {
            DeleteFileW(path_.c_str());
        }
    }

    HANDLE handle() const noexcept { return handle_; }
    const std::filesystem::path& path() const noexcept { return path_; }

    bool close() noexcept {
        const HANDLE handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
        return CloseHandle(handle) != FALSE;
    }

    void committed() noexcept { committed_ = true; }

private:
    std::filesystem::path path_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool committed_ = false;
};

vmt::Result<void> write_error(const std::filesystem::path& path,
                              const char* operation, DWORD code) {
    vmt::Error error = vmt::make_error(
        vmt::ErrorCode::IoError, std::string(operation) + ": " + path.u8string(),
        "Check free disk space, permissions and whether another program has locked the file",
        path.u8string());
    error.detail = std::system_category().message(static_cast<int>(code));
    return vmt::Err<void>(std::move(error));
}

} // namespace

vmt::Result<void> atomic_write_file(const std::filesystem::path& target,
                                   std::string_view bytes) {
    if (target.empty() || target.filename().empty()) {
        return vmt::Err<void>(vmt::make_error(
            vmt::ErrorCode::InvalidArgument, "no destination file was specified"));
    }

    // Replacing a link must never silently change the identity of a user's
    // destination. A directory is never a writable file destination either.
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return write_error(target, "the destination must be a regular file", ERROR_ACCESS_DENIED);
    }

    static std::atomic<std::uint64_t> sequence{0};
    std::filesystem::path temporary;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        temporary = target;
        temporary += L".vmtstudio-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()) + L"-" +
                     std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
        handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return write_error(target, "cannot create the temporary file", error);
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        return write_error(target, "cannot create a unique temporary file", ERROR_FILE_EXISTS);
    }

    TemporaryFile file(std::move(temporary), handle);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD count = static_cast<DWORD>(std::min(
            bytes.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(file.handle(), bytes.data() + offset, count, &written, nullptr)) {
            return write_error(target, "cannot write the temporary file", GetLastError());
        }
        if (written == 0) {
            return write_error(target, "cannot finish writing the temporary file", ERROR_WRITE_FAULT);
        }
        offset += written;
    }
    if (!FlushFileBuffers(file.handle())) {
        return write_error(target, "cannot flush the temporary file", GetLastError());
    }
    if (!file.close()) {
        return write_error(target, "cannot close the temporary file", GetLastError());
    }

    // The temporary is a sibling, so no cross-volume copy/delete fallback is
    // needed. In particular, never remove the destination before this operation.
    if (!MoveFileExW(file.path().c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return write_error(target, "cannot replace the destination file", GetLastError());
    }
    file.committed();
    return vmt::Ok();
}

} // namespace ui
