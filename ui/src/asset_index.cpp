#include "ui/asset_index.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>
#include <utility>

namespace ui {
namespace {

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

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

std::string_view extension_of(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) {
        return std::string_view();
    }
    if (slash != std::string_view::npos && dot < slash) {
        return std::string_view();
    }
    return path.substr(dot);
}

// Canonical game path: '/' separators, no "." components and no duplicate
// separators. ".." is deliberately left alone: resolving it could escape the
// game root and would make two different engine paths collide.
std::string normalize_asset_path(std::string_view raw) {
    const std::string slashed = path_to_slashes(std::string(raw));
    std::string out;
    out.reserve(slashed.size());

    std::size_t start = 0;
    while (start <= slashed.size()) {
        const std::size_t slash = slashed.find('/', start);
        const std::size_t end = slash == std::string::npos ? slashed.size() : slash;
        const std::string_view part(slashed.data() + start, end - start);
        if (!part.empty() && part != ".") {
            if (!out.empty()) {
                out.push_back('/');
            }
            out.append(part.data(), part.size());
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return out;
}

} // namespace

AssetKind asset_kind_from_path(std::string_view virtual_path) {
    const std::string_view extension = extension_of(virtual_path);
    if (iequals_ascii(extension, ".vmt")) {
        return AssetKind::Material;
    }
    if (iequals_ascii(extension, ".vtf")) {
        return AssetKind::Texture;
    }
    if (iequals_ascii(extension, ".mdl")) {
        return AssetKind::Model;
    }
    return AssetKind::Other;
}

std::string asset_type_label(AssetKind kind) {
    switch (kind) {
    case AssetKind::Material:
        return "Material";
    case AssetKind::Texture:
        return "Texture";
    case AssetKind::Model:
        return "Model";
    case AssetKind::Other:
        break;
    }
    return "Other";
}

void AssetIndex::clear() {
    entries_.clear();
    truncated_ = false;
}

void AssetIndex::merge_entries(std::vector<AssetEntry>&& added) {
    if (added.empty()) {
        return;
    }
    entries_.insert(entries_.end(), std::make_move_iterator(added.begin()),
                    std::make_move_iterator(added.end()));

    // Stable ordering by the dedup key keeps the first spelling of a path
    // (existing entries precede the newly appended batch for equal keys).
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const AssetEntry& a, const AssetEntry& b) {
                         return a.path_lower < b.path_lower;
                     });

    std::size_t out = 0;
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        if (entries_[i].path_lower == entries_[out].path_lower) {
            AssetEntry& kept = entries_[out];
            AssetEntry& duplicate = entries_[i];
            kept.sources = static_cast<std::uint8_t>(kept.sources | duplicate.sources);
            if (kept.size == 0) {
                kept.size = duplicate.size;
            }
            if (kept.disk_path.empty()) {
                kept.disk_path = std::move(duplicate.disk_path);
            }
            continue;
        }
        ++out;
        if (out != i) {
            entries_[out] = std::move(entries_[i]);
        }
    }
    entries_.resize(out + 1);

    std::sort(entries_.begin(), entries_.end(),
              [](const AssetEntry& a, const AssetEntry& b) {
                  return a.virtual_path < b.virtual_path;
              });

    if (entries_.size() > kMaxEntries) {
        entries_.resize(kMaxEntries);
        truncated_ = true;
    }
}

std::size_t AssetIndex::add_scanned_assets(const std::vector<ScannedAsset>& assets,
                                           std::uint8_t source_flag) {
    if (assets.empty()) {
        return 0;
    }
    if (entries_.size() >= kMaxEntries) {
        truncated_ = true;
        return 0;
    }

    std::vector<AssetEntry> added;
    added.reserve(std::min(assets.size(), kMaxEntries));
    for (const ScannedAsset& asset : assets) {
        if (added.size() >= kMaxEntries) {
            truncated_ = true;
            break;
        }
        std::string path = normalize_asset_path(asset.path);
        if (path.empty()) {
            continue;
        }
        AssetEntry entry;
        entry.virtual_path = std::move(path);
        entry.path_lower = to_lower(entry.virtual_path);
        entry.kind = asset_kind_from_path(entry.virtual_path);
        entry.sources = asset.loose ? static_cast<std::uint8_t>(SourceLoose) : source_flag;
        added.push_back(std::move(entry));
    }

    const std::size_t before = entries_.size();
    merge_entries(std::move(added));
    return entries_.size() - before;
}

std::size_t AssetIndex::add_directory(const std::filesystem::path& root,
                                      std::uint8_t source_flag,
                                      std::size_t max_entries) {
    if (root.empty() || max_entries == 0 || entries_.size() >= kMaxEntries) {
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
    // breaks those loops, and a hard cap bounds pathological trees. This is
    // the same protection MaterialIndex::add_directory uses.
    std::set<std::filesystem::path> visited;
    constexpr std::size_t kMaxDirectories = 20000;

    std::vector<AssetEntry> added;
    bool hit_entry_cap = false;

    while (!pending.empty() && visited.size() < kMaxDirectories) {
        const std::filesystem::path dir = pending.back();
        pending.pop_back();

        std::error_code canon_ec;
        const std::filesystem::path key = std::filesystem::weakly_canonical(dir, canon_ec);
        if (!canon_ec && !visited.insert(key).second) {
            continue; // already scanned through another link
        }

        std::error_code dir_ec;
        std::filesystem::directory_iterator it(dir, dir_ec);
        if (dir_ec) {
            continue;
        }
        for (const std::filesystem::directory_entry& item : it) {
            const std::string name = item.path().filename().string();
            if (!name.empty() && name[0] == '.') {
                continue; // hidden/system folders such as .git
            }

            std::error_code type_ec;
            if (item.is_directory(type_ec)) {
                pending.push_back(item.path());
                continue;
            }
            if (!item.is_regular_file(type_ec)) {
                continue;
            }
            const AssetKind kind = asset_kind_from_path(name);
            if (kind == AssetKind::Other) {
                continue; // this version only indexes .vmt/.vtf/.mdl
            }
            if (added.size() >= max_entries) {
                hit_entry_cap = true;
                break;
            }

            std::error_code rel_ec;
            std::filesystem::path relative =
                std::filesystem::relative(item.path(), root, rel_ec);
            if (rel_ec || relative.empty()) {
                relative = item.path().filename();
            }
            std::string path = normalize_asset_path(relative.string());
            if (path.empty()) {
                continue;
            }

            AssetEntry entry;
            entry.virtual_path = std::move(path);
            entry.path_lower = to_lower(entry.virtual_path);
            entry.kind = kind;
            entry.sources = source_flag;
            std::error_code size_ec;
            const std::uintmax_t file_size = item.file_size(size_ec);
            entry.size = size_ec ? 0 : static_cast<std::uint64_t>(file_size);
            entry.disk_path = item.path();
            added.push_back(std::move(entry));
        }
        if (hit_entry_cap) {
            break;
        }
    }

    // Both caps mean the listing is incomplete; the browser surfaces this as
    // "truncated" instead of silently pretending the index is exhaustive.
    if (hit_entry_cap || (!pending.empty() && visited.size() >= kMaxDirectories)) {
        truncated_ = true;
    }

    const std::size_t before = entries_.size();
    merge_entries(std::move(added));
    return entries_.size() - before;
}

std::vector<std::size_t> AssetIndex::filter(const AssetFilter& filter) const {
    std::vector<std::size_t> out;
    out.reserve(entries_.size());

    const std::string prefix = to_lower(path_to_slashes(filter.folder_prefix));
    std::vector<std::string> tokens;
    tokens.reserve(filter.tokens.size());
    for (const std::string& token : filter.tokens) {
        std::string lower = to_lower(token);
        if (!lower.empty()) {
            tokens.push_back(std::move(lower));
        }
    }

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const AssetEntry& entry = entries_[i];
        if (filter.kinds != 0) {
            const std::uint32_t kind_bit = 1u << static_cast<std::uint32_t>(entry.kind);
            if ((filter.kinds & kind_bit) == 0) {
                continue;
            }
        }
        if (filter.sources != 0 && (entry.sources & filter.sources) == 0) {
            continue;
        }
        if (!prefix.empty() &&
            entry.path_lower.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        bool matches_tokens = true;
        for (const std::string& token : tokens) {
            if (entry.path_lower.find(token) == std::string::npos) {
                matches_tokens = false;
                break;
            }
        }
        if (!matches_tokens) {
            continue;
        }
        out.push_back(i);
    }
    return out;
}

} // namespace ui
