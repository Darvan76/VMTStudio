#include "profiles/steam_library.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace profiles {

namespace {

namespace fs = std::filesystem;

bool directory_exists(const fs::path& path) noexcept {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

// Case-insensitive identity of a path (Windows-style): normalized, preferred
// separators, lower-cased. Only used for de-duplication, never for display.
std::wstring path_key(const fs::path& path) {
    fs::path normalized = path.lexically_normal();
    normalized.make_preferred();
    std::wstring key = normalized.wstring();
    // Collapse trailing separators so "C:\Steam\" and "C:\Steam" collide, but
    // keep drive roots ("C:\") intact.
    while (key.size() > 3 && (key.back() == L'\\' || key.back() == L'/')) {
        key.pop_back();
    }
    for (wchar_t& ch : key) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return key;
}

std::string lowercase_ascii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

// --- libraryfolders.vdf parsing ----------------------------------------------

// Un-escapes a KeyValues token: \\ -> \, \" -> ", \n -> LF, \t -> TAB.
std::string unescape_vdf(const std::string& token) {
    std::string out;
    out.reserve(token.size());
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '\\' || i + 1 >= token.size()) {
            out.push_back(token[i]);
            continue;
        }
        const char next = token[++i];
        switch (next) {
            case '\\': out.push_back('\\'); break;
            case '"':  out.push_back('"'); break;
            case 'n':  out.push_back('\n'); break;
            case 't':  out.push_back('\t'); break;
            default:   out.push_back('\\'); out.push_back(next); break;
        }
    }
    return out;
}

// All double-quoted tokens on one line, in order.
std::vector<std::string> quoted_tokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::size_t cursor = 0;
    while (true) {
        const std::size_t open = line.find('"', cursor);
        if (open == std::string::npos) {
            break;
        }
        const std::size_t close = line.find('"', open + 1);
        if (close == std::string::npos) {
            break;
        }
        tokens.push_back(line.substr(open + 1, close - open - 1));
        cursor = close + 1;
    }
    return tokens;
}

bool is_numeric_key(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    for (const char ch : token) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
    }
    return true;
}

bool is_comment_line(std::string_view line) noexcept {
    const std::size_t first = line.find_first_not_of(" \t");
    return first != std::string_view::npos &&
           line.compare(first, 2, "//") == 0;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    // libraryfolders.vdf is written as ANSI/UTF-8 without a BOM; skip a BOM
    // when one is present so it cannot corrupt the first token.
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }
    return content;
}

// --- find_steam_root probes ---------------------------------------------------

// Process environment lookup; empty string when the variable is unset.
std::wstring environment_variable(const wchar_t* name) {
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::wstring buffer(required + 1, L'\0');
    const DWORD written =
        ::GetEnvironmentVariableW(name, buffer.data(), required + 1);
    if (written == 0 || written > required) {
        return {};
    }
    buffer.resize(written);
    return buffer;
}

// HKCU\Software\Valve\Steam\SteamPath - the value the installer writes; it
// frequently uses forward slashes, which std::filesystem handles natively.
std::wstring registry_steam_path() {
    const wchar_t* const kSubKey = L"Software\\Valve\\Steam";
    const wchar_t* const kValueName = L"SteamPath";

    // First call with pvData = nullptr returns the required size in bytes.
    DWORD bytes = 0;
    LSTATUS status = ::RegGetValueW(HKEY_CURRENT_USER, kSubKey, kValueName,
                                    RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes == 0) {
        return {};
    }

    std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    status = ::RegGetValueW(HKEY_CURRENT_USER, kSubKey, kValueName,
                            RRF_RT_REG_SZ, nullptr, buffer.data(), &size);
    if (status != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return {};
    }
    buffer.resize(size / sizeof(wchar_t));
    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

fs::path as_existing_directory(const std::wstring& raw) {
    fs::path candidate(raw);
    candidate.make_preferred();
    return directory_exists(candidate) ? candidate : fs::path();
}

} // namespace

std::optional<fs::path> SteamDetector::find_steam_root() noexcept {
    try {
        // 1. STEAMPATH environment variable (preferred override).
        if (std::wstring found = environment_variable(L"STEAMPATH");
            !found.empty()) {
            if (fs::path candidate = as_existing_directory(found);
                !candidate.empty()) {
                return candidate;
            }
        }

        // 2. Registry value written by the Steam installer.
        if (std::wstring found = registry_steam_path(); !found.empty()) {
            if (fs::path candidate = as_existing_directory(found);
                !candidate.empty()) {
                return candidate;
            }
        }

        // 3. Well-known install locations.
        for (const wchar_t* const common :
             {L"C:\\Program Files (x86)\\Steam", L"C:\\Program Files\\Steam"}) {
            const fs::path candidate(common);
            if (directory_exists(candidate)) {
                return candidate;
            }
        }
    } catch (...) {
        // A failed probe must never escape; fall through to "not found".
    }
    return std::nullopt;
}

std::vector<fs::path> SteamDetector::parse_library_folders(
    const fs::path& steam_root) noexcept {
    std::vector<fs::path> libraries;
    try {
        const std::string content =
            read_text_file(steam_root / "steamapps" / "libraryfolders.vdf");

        std::size_t start = 0;
        while (start < content.size()) {
            const std::size_t newline = content.find('\n', start);
            const std::size_t end =
                (newline == std::string::npos) ? content.size() : newline;
            std::string line = content.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (is_comment_line(line)) {
                continue;
            }

            const std::vector<std::string> tokens = quoted_tokens(line);
            if (tokens.size() < 2) {
                continue; // headers, '{', '}', stray single tokens
            }

            // Current format: "path" "D:\\SteamLibrary" inside each folder
            // block. Legacy format: "1" "D:\\SteamLibrary" flat entries.
            const std::string& key = tokens.front();
            if (lowercase_ascii(key) == "path" || is_numeric_key(key)) {
                const fs::path library(unescape_vdf(tokens[1]));
                if (!library.empty()) {
                    libraries.push_back(library);
                }
            }
        }
    } catch (...) {
        // Degrade to the fallback below.
    }

    if (libraries.empty()) {
        libraries.push_back(steam_root);
    }

    // De-duplicate while preserving declaration order.
    std::vector<fs::path> unique;
    std::vector<std::wstring> seen;
    unique.reserve(libraries.size());
    for (const fs::path& library : libraries) {
        const std::wstring key = path_key(library);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            continue;
        }
        seen.push_back(key);
        unique.push_back(library);
    }
    return unique;
}

std::vector<GameProfile> SteamDetector::discover_all_in(
    const fs::path& steam_root, int max_depth_per_library) noexcept {
    std::vector<GameProfile> merged;
    try {
        std::vector<std::wstring> seen;
        for (const fs::path& library : parse_library_folders(steam_root)) {
            if (!directory_exists(library)) {
                continue; // stale libraryfolders.vdf entry
            }
            // discover() resets the manager, so each library gets its own
            // instance and the union is merged here.
            GameManager manager;
            manager.discover(library, max_depth_per_library);
            for (GameProfile& profile : manager.profiles()) {
                const std::wstring key = path_key(profile.game_dir);
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                    continue;
                }
                seen.push_back(key);
                merged.push_back(std::move(profile));
            }
        }

        // Deterministic, name-first order across all libraries.
        std::sort(merged.begin(), merged.end(),
                  [](const GameProfile& left, const GameProfile& right) {
                      const std::string left_name = lowercase_ascii(left.name);
                      const std::string right_name =
                          lowercase_ascii(right.name);
                      if (left_name != right_name) {
                          return left_name < right_name;
                      }
                      return path_key(left.game_dir) < path_key(right.game_dir);
                  });
    } catch (...) {
        // Return whatever was collected; discovery never throws.
    }
    return merged;
}

std::vector<GameProfile> SteamDetector::discover_all(
    int max_depth_per_library) noexcept {
    const std::optional<fs::path> steam_root = find_steam_root();
    if (!steam_root.has_value()) {
        return {};
    }
    return discover_all_in(*steam_root, max_depth_per_library);
}

} // namespace profiles
