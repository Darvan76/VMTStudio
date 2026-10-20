#include "ui/asset_cache.h"

#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

namespace ui {
namespace {

constexpr const char* kCacheHeader = "vmtstudio-assets\t1";

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string path_to_slashes(std::string s) {
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    return s;
}

// Game directories arrive with either separator style from settings, the
// folder picker or a previous cache. Normalizing both sides (lowercase,
// forward slashes, no trailing separator) keeps the comparison portable.
std::string normalize_game_dir(std::string_view dir) {
    std::string out;
    out.reserve(dir.size());
    for (const char raw : dir) {
        char c = raw == '\\' ? '/' : raw;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

bool has_utf8_bom(const std::string& text) {
    return text.size() >= 3 &&
           static_cast<unsigned char>(text[0]) == 0xEF &&
           static_cast<unsigned char>(text[1]) == 0xBB &&
           static_cast<unsigned char>(text[2]) == 0xBF;
}

void strip_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

std::vector<std::string> split_semicolon(std::string_view value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find(';', start);
        const std::size_t end = separator == std::string_view::npos ? value.size() : separator;
        if (end > start) {
            parts.emplace_back(value.substr(start, end - start));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return parts;
}

std::string join_semicolon(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.push_back(';');
        }
        out += parts[i];
    }
    return out;
}

std::string flags_to_string(std::uint8_t sources) {
    std::string flags;
    if ((sources & SourceLoose) != 0) {
        flags.push_back('d');
    }
    if ((sources & SourcePacked) != 0) {
        flags.push_back('v');
    }
    if ((sources & SourceFolder) != 0) {
        flags.push_back('f');
    }
    return flags;
}

std::uint8_t flags_from_string(std::string_view flags) {
    std::uint8_t sources = SourceNone;
    for (const char c : flags) {
        if (c == 'd') {
            sources = static_cast<std::uint8_t>(sources | static_cast<std::uint8_t>(SourceLoose));
        } else if (c == 'v') {
            sources = static_cast<std::uint8_t>(sources | static_cast<std::uint8_t>(SourcePacked));
        } else if (c == 'f') {
            sources = static_cast<std::uint8_t>(sources | static_cast<std::uint8_t>(SourceFolder));
        }
        // Unknown flag letters are ignored: the row is still a valid asset.
    }
    return sources;
}

std::int64_t parse_int64(std::string_view text) {
    std::int64_t value = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc() || parsed.ptr != last) {
        return 0;
    }
    return value;
}

} // namespace

std::string serialize_asset_cache(const AssetIndex& index, const AssetCacheMeta& meta) {
    std::string out;
    out.reserve(64 + index.size() * 48);
    out += kCacheHeader;
    out.push_back('\n');
    out += "game_dir\t";
    out += meta.game_dir;
    out.push_back('\n');
    out += "generated\t";
    out += std::to_string(meta.generated);
    out.push_back('\n');
    out += "roots\t";
    out += join_semicolon(meta.roots);
    out.push_back('\n');
    out += "ext\t";
    out += join_semicolon(meta.extensions);
    out.push_back('\n');
    out += "count\t";
    out += std::to_string(index.size());
    out.push_back('\n');
    for (const AssetEntry& entry : index.entries()) {
        out += "asset\t";
        out += flags_to_string(entry.sources);
        out.push_back('\t');
        out += entry.virtual_path;
        out.push_back('\n');
    }
    return out;
}

bool save_asset_cache(const std::filesystem::path& file, const AssetIndex& index,
                      const AssetCacheMeta& meta) {
    if (file.empty()) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path parent = file.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    std::filesystem::path temp = file;
    temp += ".tmp";

    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }

        // Binary mode + explicit '\n' keeps the file LF-only on Windows, as
        // documented; the reader strips stray '\r' anyway.
        const std::string data = serialize_asset_cache(index, meta);
        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        stream.close();
        if (!stream) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
    }

    // Rename over the target so a crash cannot truncate the live file. On
    // Windows rename is not guaranteed to replace an existing target, hence
    // the remove + retry (same pattern as UISettings::save).
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        ec.clear();
        std::filesystem::rename(temp, file, ec);
    }
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }
    return true;
}

AssetCacheLoad parse_asset_cache(std::string_view text,
                                 const std::string& expected_game_dir) {
    AssetCacheLoad result;
    if (text.empty()) {
        return result;
    }

    AssetCacheMeta meta;
    bool have_game_dir = false;
    std::vector<std::pair<std::string, std::uint8_t>> rows;

    std::size_t position = 0;
    bool first_line = true;
    while (position <= text.size()) {
        const std::size_t newline = text.find('\n', position);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::string line(text.substr(position, end - position));
        position = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        strip_cr(line);

        if (first_line) {
            first_line = false;
            if (has_utf8_bom(line)) {
                line.erase(0, 3);
            }
            if (line != kCacheHeader) {
                return result; // not our format, or a newer/older version
            }
            continue;
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue; // a bare key is not something this reader understands
        }
        const std::string key = line.substr(0, tab);
        const std::string value = line.substr(tab + 1);

        if (key == "game_dir") {
            meta.game_dir = value;
            have_game_dir = true;
        } else if (key == "generated") {
            meta.generated = parse_int64(value);
        } else if (key == "roots") {
            meta.roots = split_semicolon(value);
        } else if (key == "ext") {
            meta.extensions = split_semicolon(value);
        } else if (key == "asset") {
            const std::size_t flags_end = value.find('\t');
            if (flags_end == std::string::npos) {
                continue; // malformed asset row
            }
            std::string path = path_to_slashes(value.substr(flags_end + 1));
            if (path.empty()) {
                continue;
            }
            const std::string_view flags(value.data(), flags_end);
            rows.emplace_back(std::move(path), flags_from_string(flags));
        }
        // Unknown metadata keys (count included) are ignored on purpose: a
        // newer writer may add rows this reader does not need.
    }

    // Reject before touching the index so a foreign cache cannot partially
    // populate the browser list.
    if (!have_game_dir ||
        normalize_game_dir(meta.game_dir) != normalize_game_dir(expected_game_dir)) {
        return result;
    }

    std::vector<AssetEntry> entries;
    entries.reserve(rows.size());
    for (auto& row : rows) {
        AssetEntry entry;
        entry.virtual_path = std::move(row.first);
        entry.path_lower = to_lower(entry.virtual_path);
        entry.kind = asset_kind_from_path(entry.virtual_path);
        entry.sources = row.second;
        // size stays 0 and disk_path stays empty: the cache deliberately does
        // not persist them (see the header comment).
        entries.push_back(std::move(entry));
    }

    result.index.merge_entries(std::move(entries));
    result.meta = std::move(meta);
    result.loaded = true;
    return result;
}

AssetCacheLoad load_asset_cache(const std::filesystem::path& file,
                                const std::string& expected_game_dir) {
    if (file.empty()) {
        return {};
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return {};
    }
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    return parse_asset_cache(text, expected_game_dir);
}

} // namespace ui
