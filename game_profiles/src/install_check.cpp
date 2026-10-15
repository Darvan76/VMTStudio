#include "profiles/install_check.h"

#include "profiles/game_manager.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace profiles {

namespace {

namespace fs = std::filesystem;

bool regular_file_exists(const fs::path& path) noexcept {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
}

bool directory_exists(const fs::path& path) noexcept {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec;
}

// True when the directory holds no entries at all. An unreadable directory is
// never reported as empty.
bool directory_is_empty(const fs::path& path) noexcept {
    std::error_code ec;
    fs::directory_iterator it(path, ec);
    if (ec) {
        return false;
    }
    return it == fs::directory_iterator();
}

constexpr std::uint16_t kMachineI386 = 0x14C;   // IMAGE_FILE_MACHINE_I386
constexpr std::uint16_t kMachineAmd64 = 0x8664; // IMAGE_FILE_MACHINE_AMD64

// Engine DLLs are small; larger files still get their architecture read from
// the headers, but the (best-effort) export check is skipped.
constexpr std::size_t kMaxExportScanBytes = 8u * 1024u * 1024u;

// Same DLL set (and load order) as ui::test_connection and the Phase 3 spike.
struct DllExpectation {
    const char* name = nullptr;
    const char* export_name = nullptr; // export that must be present (best-effort check)
};

constexpr DllExpectation kRequiredDlls[] = {
    {"tier0.dll", "CommandLine_Tier0"},
    {"vstdlib.dll", "CreateInterface"},
    {"filesystem_stdio.dll", "CreateInterface"},
    {"materialsystem.dll", "CreateInterface"},
    {"shaderapidx9.dll", "CreateInterface"},
};

std::string join(const std::vector<std::string>& parts) {
    if (parts.empty()) {
        return {};
    }
    std::size_t total_size = 0;
    for (const std::string& part : parts) {
        total_size += part.size() + 2;
    }
    std::string out;
    out.reserve(total_size);
    for (const std::string& part : parts) {
        if (!out.empty()) {
            out += ", ";
        }
        out += part;
    }
    return out;
}

// --- minimal, bounds-checked PE inspection ------------------------------------

// Bounds-checked little-endian access over an in-memory image.
class ByteView {
public:
    explicit ByteView(std::string_view data) noexcept : data_(data) {}

    std::size_t size() const noexcept { return data_.size(); }

    bool u16(std::size_t offset, std::uint16_t& out) const noexcept {
        if (offset > data_.size() || data_.size() - offset < 2) {
            return false;
        }
        const auto* const bytes = reinterpret_cast<const unsigned char*>(data_.data() + offset);
        out = static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
        return true;
    }

    bool u32(std::size_t offset, std::uint32_t& out) const noexcept {
        if (offset > data_.size() || data_.size() - offset < 4) {
            return false;
        }
        const auto* const bytes = reinterpret_cast<const unsigned char*>(data_.data() + offset);
        out = static_cast<std::uint32_t>(bytes[0]) |
              (static_cast<std::uint32_t>(bytes[1]) << 8) |
              (static_cast<std::uint32_t>(bytes[2]) << 16) |
              (static_cast<std::uint32_t>(bytes[3]) << 24);
        return true;
    }

    // NUL-terminated string starting at `offset`, capped at `max_len` bytes.
    bool cstring(std::size_t offset, std::string& out, std::size_t max_len) const {
        if (offset >= data_.size()) {
            return false;
        }
        const std::size_t remaining = data_.size() - offset;
        const std::size_t check_len = std::min(remaining, max_len);
        const std::size_t limit = offset + check_len;
        for (std::size_t i = offset; i < limit; ++i) {
            if (data_[i] == '\0') {
                out.assign(data_.data() + offset, i - offset);
                return true;
            }
        }
        return false;
    }

private:
    std::string_view data_;
};

struct PeSummary {
    bool valid_pe = false;       // DOS + PE + optional headers parsed
    std::uint16_t machine = 0;   // IMAGE_FILE_MACHINE_*
    bool export_table = false;   // export directory present and fully parsed
    std::vector<std::string> exports; // names listed in the export directory
};

// Parses the DOS/PE/optional headers and (best-effort) the export directory.
// Any parse failure only degrades the corresponding part of the summary; it
// never throws and never marks a file as invalid by itself.
PeSummary summarize_pe(std::string_view bytes) {
    PeSummary summary;
    const ByteView view(bytes);

    std::uint16_t dos_magic = 0;
    if (!view.u16(0, dos_magic) || dos_magic != 0x5A4D) { // "MZ"
        return summary;
    }
    std::uint32_t pe_offset = 0;
    if (!view.u32(0x3C, pe_offset) || pe_offset == 0 ||
        pe_offset > view.size() || view.size() - pe_offset < 24) {
        return summary;
    }
    std::uint16_t pe_sig = 0;
    if (!view.u16(pe_offset, pe_sig) || pe_sig != 0x4550) { // "PE\0\0"
        return summary;
    }

    std::uint16_t machine = 0;
    std::uint16_t number_of_sections = 0;
    std::uint16_t size_of_optional = 0;
    if (!view.u16(pe_offset + 4, machine) ||
        !view.u16(pe_offset + 6, number_of_sections) ||
        !view.u16(pe_offset + 20, size_of_optional)) {
        return summary;
    }

    const std::size_t optional = pe_offset + 24;
    std::uint16_t magic = 0;
    if (optional > view.size() || view.size() - optional < 2 || !view.u16(optional, magic)) {
        return summary;
    }

    // Data directory layout differs between PE32 and PE32+.
    std::size_t number_of_rvas_offset = 0;
    std::size_t data_dirs_offset = 0;
    if (magic == 0x10B) { // PE32 (a 32-bit engine DLL)
        number_of_rvas_offset = optional + 0x5C;
        data_dirs_offset = optional + 0x60;
    } else if (magic == 0x20B) { // PE32+ (a 64-bit DLL)
        number_of_rvas_offset = optional + 0x6C;
        data_dirs_offset = optional + 0x70;
    } else {
        return summary; // unsupported optional header
    }
    if (data_dirs_offset > view.size() || view.size() - data_dirs_offset < 8) {
        return summary;
    }

    summary.valid_pe = true;
    summary.machine = machine;

    // Everything below is the best-effort export check: a failure leaves
    // export_table == false, which the caller treats as "skipped".
    std::uint32_t number_of_rvas = 0;
    if (!view.u32(number_of_rvas_offset, number_of_rvas) || number_of_rvas < 1) {
        return summary;
    }
    std::uint32_t export_rva = 0;
    if (!view.u32(data_dirs_offset, export_rva) || export_rva == 0) {
        return summary; // valid image without an export directory
    }

    // Map an RVA (covering `span` bytes) into the file via the section table.
    const std::size_t sections = optional + size_of_optional;
    if (sections > view.size()) {
        return summary;
    }

    const auto rva_to_offset = [&](std::uint32_t rva, std::size_t span,
                                   std::size_t& out) -> bool {
        for (std::size_t i = 0; i < number_of_sections; ++i) {
            if (i > (view.size() - sections) / 40) {
                return false;
            }
            const std::size_t header = sections + i * 40;
            std::uint32_t virtual_address = 0;
            std::uint32_t raw_size = 0;
            std::uint32_t raw_pointer = 0;
            if (header > view.size() || view.size() - header < 40 ||
                !view.u32(header + 12, virtual_address) ||
                !view.u32(header + 16, raw_size) ||
                !view.u32(header + 20, raw_pointer)) {
                return false;
            }
            if (raw_size == 0 || rva < virtual_address || rva - virtual_address >= raw_size) {
                continue;
            }
            if (span > raw_size - (rva - virtual_address)) {
                return false;
            }
            const std::size_t delta = static_cast<std::size_t>(rva - virtual_address);
            if (raw_pointer > view.size() || delta > view.size() - raw_pointer) {
                return false;
            }
            out = raw_pointer + delta;
            return span <= view.size() - out;
        }
        return false;
    };

    std::size_t directory = 0;
    if (!rva_to_offset(export_rva, 40, directory)) {
        return summary;
    }
    std::uint32_t number_of_names = 0;
    std::uint32_t names_rva = 0;
    if (!view.u32(directory + 0x18, number_of_names) ||
        !view.u32(directory + 0x20, names_rva)) {
        return summary;
    }
    summary.export_table = true;
    if (number_of_names == 0 || names_rva == 0) {
        return summary; // export table present but lists no names
    }

    // Sanity limit on export count to prevent excessive loops or memory usage on corrupt headers
    constexpr std::uint32_t kMaxSaneExportCount = 65536;
    if (number_of_names > kMaxSaneExportCount || number_of_names > view.size() / 4u) {
        summary.export_table = false;
        return summary;
    }

    std::size_t names = 0;
    if (!rva_to_offset(names_rva, static_cast<std::size_t>(number_of_names) * 4u, names)) {
        summary.export_table = false; // partial parse: skip the check entirely
        summary.exports.clear();
        return summary;
    }

    summary.exports.reserve(std::min<std::size_t>(number_of_names, 4096));

    for (std::uint32_t i = 0; i < number_of_names; ++i) {
        std::uint32_t name_rva = 0;
        std::size_t name_offset = 0;
        std::string name;
        if (!view.u32(names + i * 4u, name_rva) || name_rva == 0 ||
            !rva_to_offset(name_rva, 1, name_offset) || !view.cstring(name_offset, name, 256)) {
            summary.export_table = false; // partial parse: skip the check entirely
            summary.exports.clear();
            return summary;
        }
        summary.exports.push_back(std::move(name));
    }
    return summary;
}

std::string read_file_capped(const fs::path& path, std::size_t max_bytes) noexcept {
    try {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
            return {};
        }
        in.seekg(0, std::ios::end);
        const auto end_pos = in.tellg();
        if (end_pos <= 0) {
            return {};
        }
        const auto file_size = static_cast<std::size_t>(end_pos);
        const std::size_t to_read = (max_bytes > 0 && file_size > max_bytes) ? max_bytes : file_size;
        in.seekg(0, std::ios::beg);
        std::string data;
        data.resize(to_read);
        in.read(data.data(), static_cast<std::streamsize>(to_read));
        const auto actual_read = in.gcount();
        if (actual_read < 0) {
            return {};
        }
        data.resize(static_cast<std::size_t>(actual_read));
        return data;
    } catch (...) {
        return {};
    }
}

} // namespace

const char* to_string(InstallProblem problem) noexcept {
    switch (problem) {
        case InstallProblem::EmptyFolder: return "empty folder";
        case InstallProblem::NoGameInfo:  return "no gameinfo.txt";
        case InstallProblem::NoBin:       return "no bin folder";
        case InstallProblem::WrongArch:   return "wrong architecture";
        case InstallProblem::MissingDll:  return "missing DLLs";
        case InstallProblem::NoExports:   return "missing exports";
        case InstallProblem::None:        break;
    }
    return "";
}

InstallReport check_install(const GameProfile& profile) noexcept {
    InstallReport report;
    try {
        const fs::path game_dir = profile.game_dir;

        // 1. The game folder must exist and contain something.
        if (game_dir.empty()) {
            report.problem = InstallProblem::EmptyFolder;
            report.detail = "No game folder is set for this profile.";
            return report;
        }
        if (!directory_exists(game_dir)) {
            report.problem = InstallProblem::EmptyFolder;
            report.detail = "The game folder does not exist: " + game_dir.string() +
                            ". Check the game folder setting or select the game again.";
            return report;
        }
        if (directory_is_empty(game_dir)) {
            report.problem = InstallProblem::EmptyFolder;
            report.detail = "The game folder is empty: " + game_dir.string() +
                            ". The game is not installed there (it may have been moved or "
                            "uninstalled). Verify the installation in Steam or select another folder.";
            return report;
        }

        // 2. gameinfo.txt marks a real Source mod folder.
        if (!regular_file_exists(game_dir / "gameinfo.txt")) {
            report.problem = InstallProblem::NoGameInfo;
            report.detail = "The folder does not contain gameinfo.txt: " + game_dir.string() +
                            ". Select the game or mod folder that contains it.";
            return report;
        }

        // 3. Resolve the engine /bin folder: the profile's own bin_dir, then the
        // shared detection helper, then a plain "bin" folder that lacks the
        // marker DLLs (so the report can name what is missing).
        fs::path bin_dir = profile.bin_dir;
        if (!bin_dir.empty() && !directory_exists(bin_dir)) {
            bin_dir.clear();
        }
        if (bin_dir.empty()) {
            bin_dir = GameManager::detect_bin_dir(game_dir);
        }
        if (bin_dir.empty()) {
            if (directory_exists(game_dir / "bin")) {
                bin_dir = game_dir / "bin";
            } else if (directory_exists(game_dir.parent_path() / "bin")) {
                bin_dir = game_dir.parent_path() / "bin";
            }
        }
        if (bin_dir.empty()) {
            report.problem = InstallProblem::NoBin;
            report.detail = "No Source engine 'bin' folder with tier0.dll and vstdlib.dll was found "
                            "in or near: " +
                            game_dir.string() +
                            ". The installation looks incomplete; verify the game files in Steam.";
            return report;
        }

        // 4. All required engine DLLs must exist.
        std::vector<std::string> missing;
        std::vector<const DllExpectation*> present;
        present.reserve(std::size(kRequiredDlls));
        for (const DllExpectation& dll : kRequiredDlls) {
            if (regular_file_exists(bin_dir / dll.name)) {
                present.push_back(&dll);
            } else {
                missing.push_back(dll.name);
            }
        }
        if (!missing.empty()) {
            report.problem = InstallProblem::MissingDll;
            report.missing_dlls = std::move(missing);
            report.detail = "The engine folder is missing required DLLs (" + join(report.missing_dlls) +
                            "): " + bin_dir.string() + ". Verify the game installation in Steam.";
            return report;
        }

        // 5. Every DLL must be a 32-bit (x86) PE image; the application always
        // drives the 32-bit hlmv_backend.
        std::vector<std::string> bad_arch;
        std::vector<std::pair<const DllExpectation*, PeSummary>> summaries;
        summaries.reserve(present.size());
        for (const DllExpectation* const dll : present) {
            const std::string bytes = read_file_capped(bin_dir / dll->name, kMaxExportScanBytes);
            PeSummary summary = summarize_pe(bytes);
            if (!summary.valid_pe) {
                bad_arch.push_back(std::string(dll->name) + " (corrupt or not a valid Windows DLL)");
            } else if (summary.machine == kMachineAmd64) {
                bad_arch.push_back(std::string(dll->name) + " (64-bit)");
            } else if (summary.machine != kMachineI386) {
                bad_arch.push_back(std::string(dll->name) + " (unsupported architecture)");
            }
            summaries.emplace_back(dll, std::move(summary));
        }
        if (!bad_arch.empty()) {
            report.problem = InstallProblem::WrongArch;
            report.detail = "This application needs the 32-bit (x86) engine DLLs, but: " +
                            join(bad_arch) + " in " + bin_dir.string() +
                            ". Install the game with its original 32-bit engine files.";
            return report;
        }

        // 6. Best-effort export check: a DLL whose export table cannot be parsed
        // is not failed here, but one that clearly lacks the expected export is.
        std::vector<std::string> no_export;
        for (const auto& entry : summaries) {
            const DllExpectation* const dll = entry.first;
            const PeSummary& summary = entry.second;
            if (!summary.export_table) {
                continue; // unreadable export table: skip
            }
            const bool has_export =
                std::find(summary.exports.begin(), summary.exports.end(), dll->export_name) !=
                summary.exports.end();
            if (!has_export) {
                no_export.push_back(std::string(dll->name) + " (missing export " + dll->export_name +
                                    ")");
            }
        }
        if (!no_export.empty()) {
            report.problem = InstallProblem::NoExports;
            report.detail = "The engine DLLs do not export the expected interfaces: " +
                            join(no_export) +
                            ". The files may belong to an unsupported engine build; verify the "
                            "game installation.";
            return report;
        }

        report.usable = true;
        report.problem = InstallProblem::None;
        report.detail = "OK: all required engine DLLs were found in " + bin_dir.string() +
                        " as 32-bit DLLs with the expected exports.";
        return report;
    } catch (...) {
        report.usable = false;
        report.problem = InstallProblem::EmptyFolder;
        report.detail = "An internal error occurred while inspecting the game installation.";
        return report;
    }
}

} // namespace profiles
