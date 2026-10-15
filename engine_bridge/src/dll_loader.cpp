#include "bridge/dll_loader.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bridge {

namespace {

std::string win32_error_text(DWORD code) {
    if (code == 0) return {};
    wchar_t* raw_buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&raw_buf), 0, nullptr);
    const auto local_free_deleter = [](void* ptr) noexcept {
        if (ptr != nullptr) {
            ::LocalFree(ptr);
        }
    };
    const std::unique_ptr<wchar_t, decltype(local_free_deleter)> buf(raw_buf, local_free_deleter);

    std::string out;
    if (n != 0 && buf) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf.get(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            out.resize(static_cast<std::size_t>(len));
            WideCharToMultiByte(CP_UTF8, 0, buf.get(), static_cast<int>(n), out.data(), len, nullptr, nullptr);
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

} // namespace

vmt::Result<std::vector<DllReport>> DllLoader::load(const LoaderOptions& options) {
    std::vector<DllReport> reports;
    reports.reserve(options.dll_names.size());

    // No process-wide search-path mutation here: changing the default DLL
    // directories would also affect the engine's own later loads (and makes
    // LOAD_WITH_ALTERED_SEARCH_PATH ineffective). Each DLL is loaded with
    // explicit flags instead, so its own directory is searched for
    // dependencies while the process defaults stay untouched.
    for (const std::string& name : options.dll_names) {
        DllReport report;
        report.name = name;

        const std::filesystem::path full = options.bin_dir / name;
        std::error_code ec;
        if (!std::filesystem::exists(full, ec) || ec) {
            report.error = "file not found: " + full.string();
            reports.push_back(std::move(report));
            continue;
        }
        report.present = true;
        report.arch = read_pe_arch(full);

        // Skip in-process loading for x86 DLLs when running in an x64 host.
        if constexpr (sizeof(void*) == 8) {
            if (report.arch == bridge::PeArch::X86) {
                report.error = "x86 DLL: header check only (not loaded into x64 host)";
                reports.push_back(std::move(report));
                continue;
            }
        }

        const HMODULE h = LoadLibraryExW(full.c_str(), nullptr,
                                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                             LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (h == nullptr) {
            const DWORD err = GetLastError();
            report.error = win32_error_text(err);
            if (report.error.empty()) {
                report.error = "LoadLibraryW failed (error " + std::to_string(err) + ")";
            }
            reports.push_back(std::move(report));
            continue;
        }
        report.loaded = true;
        report.create_interface = (GetProcAddress(h, "CreateInterface") != nullptr);
        report.command_line = (GetProcAddress(h, "CommandLine") != nullptr);
        report.command_line_tier0 = (GetProcAddress(h, "CommandLine_Tier0") != nullptr);
        reports.push_back(std::move(report));
    }

    return vmt::Ok(std::move(reports));
}

} // namespace bridge
