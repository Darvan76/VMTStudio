#include "ui/material_browser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

namespace ui {
namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    return to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

std::string path_to_slashes(std::string s) {
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    return s;
}

std::string strip_vmt_extension(std::string s) {
    if (ends_with_ci(s, ".vmt") || ends_with_ci(s, ".vtf")) {
        s.resize(s.size() - 4);
    }
    return s;
}

// Sorts by name and removes duplicates, keeping the first (a disk entry beats
// an engine entry only when the disk scan ran first; either way the material
// name is unique).
void sort_and_dedup(std::vector<MaterialEntry>& entries) {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const MaterialEntry& a, const MaterialEntry& b) {
                         return a.name < b.name;
                     });
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](const MaterialEntry& a, const MaterialEntry& b) {
                                  return a.name == b.name;
                              }),
                  entries.end());
}

std::uint32_t fnv1a(std::string_view text) {
    std::uint32_t hash = 2166136261u;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}

void put16(std::ofstream& stream, std::uint16_t value) {
    const unsigned char bytes[2] = { static_cast<unsigned char>(value & 0xFF),
                                     static_cast<unsigned char>((value >> 8) & 0xFF) };
    stream.write(reinterpret_cast<const char*>(bytes), 2);
}

void put32(std::ofstream& stream, std::uint32_t value) {
    const unsigned char bytes[4] = { static_cast<unsigned char>(value & 0xFF),
                                     static_cast<unsigned char>((value >> 8) & 0xFF),
                                     static_cast<unsigned char>((value >> 16) & 0xFF),
                                     static_cast<unsigned char>((value >> 24) & 0xFF) };
    stream.write(reinterpret_cast<const char*>(bytes), 4);
}

std::uint32_t get32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

} // namespace

void MaterialIndex::clear() {
    entries_.clear();
    truncated_ = false;
}

std::size_t MaterialIndex::add_directory(const std::filesystem::path& root) {
    if (root.empty() || entries_.size() >= kMaxEntries) {
        return 0;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return 0;
    }

    std::vector<std::filesystem::path> pending;
    pending.push_back(root);

    // Directory junctions and symlinks can form cycles (a game folder linking
    // back to an ancestor); canonicalizing each directory before visiting it
    // breaks those loops, and a hard cap bounds pathological trees.
    std::set<std::filesystem::path> visited;
    constexpr std::size_t kMaxDirectories = 20000;

    std::vector<MaterialEntry> added;
    while (!pending.empty() && visited.size() < kMaxDirectories) {
        const std::filesystem::path dir = pending.back();
        pending.pop_back();

        std::error_code canon_ec;
        const std::filesystem::path key = std::filesystem::weakly_canonical(dir, canon_ec);
        if (!canon_ec && !visited.insert(key).second) {
            continue; // already scanned through another link
        }

        std::filesystem::directory_iterator it(dir, ec);
        if (ec) {
            continue;
        }
        for (const std::filesystem::directory_entry& entry : it) {
            const std::string name = entry.path().filename().string();
            if (!name.empty() && name[0] == '.') {
                continue; // hidden/system folders such as .git
            }
            std::error_code type_ec;
            if (entry.is_directory(type_ec)) {
                pending.push_back(entry.path());
            } else if (entry.is_regular_file(type_ec) &&
                       ends_with_ci(name, ".vmt")) {
                MaterialEntry material;
                material.name = material_name_from_disk_path(root, entry.path());
                if (material.name.empty()) {
                    continue;
                }
                material.display = path_to_slashes(
                    std::filesystem::relative(entry.path(), root, type_ec).string());
                if (type_ec || material.display.empty()) {
                    material.display = entry.path().filename().string();
                }
                // Show the path the way the material name is addressed: a
                // leading materials/ folder is implied by the source.
                if (material.display.compare(0, 10, "materials/") == 0) {
                    material.display = material.display.substr(10);
                }
                material.disk_path = entry.path();
                material.from_engine = false;
                added.push_back(std::move(material));
            }
        }
    }

    if (added.size() > kMaxEntries) {
        added.resize(kMaxEntries);
        truncated_ = true;
    }
    const std::size_t before = entries_.size();
    entries_.insert(entries_.end(), std::make_move_iterator(added.begin()),
                    std::make_move_iterator(added.end()));
    sort_and_dedup(entries_);
    if (entries_.size() > kMaxEntries) {
        entries_.resize(kMaxEntries);
        truncated_ = true;
    }
    return entries_.size() - before;
}

std::size_t MaterialIndex::add_engine_names(const std::vector<std::string>& names) {
    std::vector<MaterialEntry> added;
    added.reserve(names.size());
    for (const std::string& raw : names) {
        const std::string name = material_name_from_game_path(raw);
        if (name.empty()) {
            continue;
        }
        MaterialEntry material;
        material.name = name;
        material.display = name + ".vmt";
        material.from_engine = true;
        added.push_back(std::move(material));
    }

    if (added.size() > kMaxEntries) {
        added.resize(kMaxEntries);
        truncated_ = true;
    }
    const std::size_t before = entries_.size();
    entries_.insert(entries_.end(), std::make_move_iterator(added.begin()),
                    std::make_move_iterator(added.end()));
    sort_and_dedup(entries_);
    if (entries_.size() > kMaxEntries) {
        entries_.resize(kMaxEntries);
        truncated_ = true;
    }
    return entries_.size() - before;
}

std::vector<std::size_t> MaterialIndex::search(std::string_view query) const {
    std::vector<std::size_t> out;
    out.reserve(entries_.size());
    std::string needle = to_lower(std::string(query));
    while (!needle.empty() && (needle.back() == ' ' || needle.back() == '\t')) {
        needle.pop_back();
    }
    if (needle.empty()) {
        out.resize(entries_.size());
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            out[i] = i;
        }
        return out;
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (to_lower(entries_[i].name).find(needle) != std::string::npos) {
            out.push_back(i);
        }
    }
    return out;
}

std::string material_name_from_game_path(std::string_view game_path) {
    std::string path = path_to_slashes(std::string(game_path));
    const std::string prefix = "materials/";
    // The prefix is matched case-insensitively: loose folders are usually
    // lowercase, but "Materials/" appears in the wild too.
    while (path.size() >= prefix.size() &&
           to_lower(path.substr(0, prefix.size())) == prefix) {
        path = path.substr(prefix.size());
    }
    if (path.empty()) {
        return std::string();
    }
    return strip_vmt_extension(path);
}

std::string material_name_from_disk_path(const std::filesystem::path& root,
                                         const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(file, root, ec);
    if (ec || relative.empty()) {
        return std::string();
    }
    std::string path = path_to_slashes(relative.string());
    const std::string prefix = "materials/";
    if (path.compare(0, prefix.size(), prefix) == 0) {
        path = path.substr(prefix.size());
    }
    if (path.empty()) {
        return std::string();
    }
    return strip_vmt_extension(path);
}

std::string thumbnail_file_name(std::string_view material_name) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%08x.bmp", fnv1a(to_lower(std::string(material_name))));
    return buffer;
}

std::filesystem::path thumbnail_cache_path(const std::filesystem::path& cache_root,
                                           std::string_view material_name) {
    return cache_root / thumbnail_file_name(material_name);
}

bool write_bmp_from_bgra(const std::filesystem::path& path, int width, int height,
                         const unsigned char* bgra, bool rows_top_down) {
    if (path.empty() || width <= 0 || height <= 0 || bgra == nullptr) {
        return false;
    }

    const int row_stride = ((width * 3) + 3) & ~3; // 4-byte aligned rows
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(row_stride) * height;
    const std::uint32_t file_size = 14 + 40 + pixel_bytes;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }

    // BITMAPFILEHEADER
    stream.write("BM", 2);
    put32(stream, file_size);
    put16(stream, 0);
    put16(stream, 0);
    put32(stream, 14 + 40);
    // BITMAPINFOHEADER (positive height = bottom-up storage)
    put32(stream, 40);
    put32(stream, static_cast<std::uint32_t>(width));
    put32(stream, static_cast<std::uint32_t>(height));
    put16(stream, 1);  // planes
    put16(stream, 24); // bpp
    put32(stream, 0);  // BI_RGB
    put32(stream, pixel_bytes);
    put32(stream, 2835); // ~72 DPI
    put32(stream, 2835);
    put32(stream, 0);
    put32(stream, 0);

    std::vector<unsigned char> row(static_cast<std::size_t>(row_stride), 0);
    // BMP stores the bottom row first. When the source is top-down, walk it
    // backwards; when it is already bottom-up, walk it forwards.
    for (int y = 0; y < height; ++y) {
        const int source_y = rows_top_down ? (height - 1 - y) : y;
        const unsigned char* src = bgra + static_cast<std::size_t>(source_y) * width * 4;
        for (int x = 0; x < width; ++x) {
            row[static_cast<std::size_t>(x) * 3 + 0] = src[x * 4 + 0]; // B
            row[static_cast<std::size_t>(x) * 3 + 1] = src[x * 4 + 1]; // G
            row[static_cast<std::size_t>(x) * 3 + 2] = src[x * 4 + 2]; // R
        }
        stream.write(reinterpret_cast<const char*>(row.data()), row_stride);
    }

    stream.close();
    if (!stream) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return false;
    }
    return true;
}

bool read_bmp_size(const std::filesystem::path& path, int& width, int& height) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    unsigned char header[26] = {};
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    if (stream.gcount() != sizeof(header) || header[0] != 'B' || header[1] != 'M') {
        return false;
    }
    width = static_cast<int>(get32(header + 18));
    height = static_cast<int>(get32(header + 22));
    return width > 0 && height != 0;
}

} // namespace ui
