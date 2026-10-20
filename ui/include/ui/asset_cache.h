#pragma once

// ui asset_cache - on-disk cache for AssetIndex.
//
// Opening the asset browser must not rescan a large game every time, so the
// index is serialized as a small TSV (UTF-8, LF). The Qt layer chooses the
// file (one per game, e.g. %LOCALAPPDATA%\VMTStudio\cache\<game_key>\
// assets-v1.tsv); this module only writes and validates the format.
//
// What is stored: game paths plus their source flags (d/v/f). What is not:
//   - disk_path: it points into a user folder that may have moved and is only
//     meaningful for this session; after a load it is empty.
//   - size: only loose files have one, and it changes without the game being
//     rescanned; folder scans refresh it.
// A loaded index is therefore a fast listing, not a mounted VFS.
//
// The header carries a format version and the game directory, so a cache from
// another install can never leak into the list.

#include "ui/asset_index.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

struct AssetCacheMeta {
    std::string game_dir;
    std::int64_t generated = 0;
    std::vector<std::string> roots;
    std::vector<std::string> extensions;
};

// The TSV is the index half of the Asset Browser pack. Keeping serialization
// separate from file I/O lets the pack store/load the same bytes without ever
// materializing assets-v1.tsv.
std::string serialize_asset_cache(const AssetIndex& index, const AssetCacheMeta& meta);

// Writes atomically: a sibling ".tmp" file is completed first and then renamed
// over the target, so a crash cannot leave a half-written cache. Creates the
// parent directory. Returns false on any I/O failure (the temp file is removed).
bool save_asset_cache(const std::filesystem::path& file, const AssetIndex& index,
                      const AssetCacheMeta& meta);

struct AssetCacheLoad {
    bool loaded = false;
    AssetIndex index;
    AssetCacheMeta meta;
};

// `expected_game_dir` is compared case-insensitively, ignoring slash style and
// trailing separators. A different header version, a missing game_dir row or a
// different game all produce loaded=false with an empty index. Unknown rows are
// ignored so newer writers can add metadata safely.
AssetCacheLoad parse_asset_cache(std::string_view text,
                                 const std::string& expected_game_dir);

// File-based convenience wrappers; the pack uses the string versions.
AssetCacheLoad load_asset_cache(const std::filesystem::path& file,
                                const std::string& expected_game_dir);

} // namespace ui
