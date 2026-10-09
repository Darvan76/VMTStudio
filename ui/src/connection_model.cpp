#include "ui/connection_model.h"

#include "bridge/dll_loader.h"
#include "bridge/pe.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ui {
namespace {

// The same set the Phase 3 spike loads, in load order. When engine_bridge gets
// its public mounting API this table should move there.
struct RequiredDll {
    const char* name;
    bool need_create_interface;
    bool need_command_line;
};

const RequiredDll kRequiredDlls[] = {
    {"tier0.dll", false, true},
    {"vstdlib.dll", true, false},
    {"filesystem_stdio.dll", true, false},
    {"materialsystem.dll", true, false},
    {"shaderapidx9.dll", true, false},
};

std::string arch_name(bridge::PeArch arch) {
    return bridge::to_string(arch);
}

} // namespace

ConnectionCheck test_connection(const std::filesystem::path& bin_dir) {
    ConnectionCheck check;
    check.bin_dir = bin_dir;

    try {
        bridge::LoaderOptions options;
        options.bin_dir = bin_dir;
        for (const RequiredDll& dll : kRequiredDlls) {
            options.dll_names.push_back(dll.name);
        }

        const vmt::Result<std::vector<bridge::DllReport>> result =
            bridge::DllLoader::load(options);
        if (!result) {
            check.summary = "Not connected: " + result.error().message;
            return check;
        }
        const std::vector<bridge::DllReport>& reports = result.value();

        bool all_present_x86 = true;
        bool any_present = false;
        std::size_t ok_count = 0;
        for (std::size_t i = 0; i < reports.size(); ++i) {
            const bridge::DllReport& report = reports[i];
            const RequiredDll& required = kRequiredDlls[i];

            const bool export_ok = (!required.need_create_interface || report.create_interface) &&
                                   (!required.need_command_line ||
                                    report.command_line || report.command_line_tier0);
            const bool ok = report.loaded && export_ok;
            if (ok) {
                ++ok_count;
            }

            if (report.present) {
                any_present = true;
                if (report.arch != bridge::PeArch::X86) {
                    all_present_x86 = false;
                }
            }

            std::string line;
            if (ok) {
                line = "[OK] " + report.name + " (" + arch_name(report.arch);
                if (report.create_interface) {
                    line += ", CreateInterface";
                } else if (report.command_line || report.command_line_tier0) {
                    line += ", CommandLine";
                }
                line += ")";
            } else if (!report.present) {
                line = "[FAIL] " + report.name + ": missing (" + report.error + ")";
            } else if (!report.loaded) {
                line = "[FAIL] " + report.name + " (" + arch_name(report.arch) +
                       "): " + report.error;
            } else {
                line = "[FAIL] " + report.name + " (" + arch_name(report.arch) +
                       "): loaded but the expected export is missing";
            }
            check.lines.push_back(std::move(line));
        }

        check.architecture =
            any_present ? (all_present_x86 ? "x86" : "not x86 (incompatible with this 32-bit app)")
                        : "unknown";
        check.connected = ok_count == reports.size() && !reports.empty();
        check.summary = check.connected
                            ? "Connected: all " + std::to_string(reports.size()) +
                                  " required DLLs loaded (architecture " + check.architecture + ")"
                            : "Not connected: " +
                                  std::to_string(reports.size() - ok_count) + " of " +
                                  std::to_string(reports.size()) + " checks failed";
        return check;
    } catch (...) {
        check.connected = false;
        check.architecture = "unknown";
        check.summary = "Not connected: unexpected error while loading the game DLLs";
        check.lines.clear();
        return check;
    }
}

std::filesystem::path detect_game_dir(const std::filesystem::path& bin_dir) {
    if (bin_dir.empty()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path root = bin_dir.parent_path();
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        return {};
    }

    if (std::filesystem::is_regular_file(root / "gameinfo.txt", ec)) {
        return root;
    }

    std::vector<std::filesystem::path> candidates;
    std::filesystem::directory_iterator iterator(root, ec);
    if (ec) {
        return {};
    }
    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec) &&
            std::filesystem::is_regular_file(entry.path() / "gameinfo.txt", entry_ec)) {
            candidates.push_back(entry.path());
        }
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.empty() ? std::filesystem::path() : candidates.front();
}

} // namespace ui
