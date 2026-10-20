#pragma once

// ui asset_index - headless, Qt-free index over the game's asset files.
//
// Two very different sources feed one sortable/filterable list:
//   - engine dumps (game paths, possibly inside a VPK), and
//   - folder scans (real files on disk).
// A path present in more than one source is kept once and its source bits are
// merged, because the browser shows both badges and the user may open either
// copy.
//
// The class has no dependency on engine_bridge: the Qt layer converts
// bridge::AssetRecord rows into ScannedAsset before calling
// add_scanned_assets(). That keeps this core testable without a game or the IPC
// stack.
//
// Rules worth keeping:
//   - virtual_path is the canonical game path, with '/' and its extension
//     ("materials/brick/brick001.vmt").
//   - Dedup is case-insensitive on path_lower but the first spelling seen is
//     preserved for display, so a VPK's "Materials/Foo.VMT" is not rewritten.
//   - The index is sorted by virtual_path after every ingestion, which makes
//     filter() results stable and cheap to map back into the entries vector.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

enum class AssetKind : std::uint8_t { Material = 0, Texture = 1, Model = 2, Other = 3 };

enum AssetSource : std::uint8_t {
    SourceNone = 0,
    SourceLoose = 1u << 0,  // a real file on disk (game dir or addon)
    SourcePacked = 1u << 1, // inside a VPK
    SourceFolder = 1u << 2, // a folder added by the user
};

// Neutral input row for engine dumps. `loose` mirrors what the engine reports:
// true means the file was resolved on disk rather than inside a VPK. Folder
// scans use add_directory() and do not build these rows.
struct ScannedAsset {
    std::string path;
    bool loose = false;
};

struct AssetEntry {
    std::string virtual_path;        // canonical game path: '/', extension kept
    std::string path_lower;          // precomputed for dedup and search
    AssetKind kind = AssetKind::Other;
    std::uint8_t sources = SourceNone;
    std::uint64_t size = 0;          // 0 = unknown (all VPK and cache rows)
    std::filesystem::path disk_path; // only set by a folder scan
};

struct AssetFilter {
    std::uint32_t kinds = 0;         // 1u << kind; 0 = every kind
    std::uint8_t sources = 0;        // 0 = every source
    std::string folder_prefix;       // case-insensitive prefix of virtual_path
    std::vector<std::string> tokens; // all must occur in path_lower; empty = no text filter
};

// Forward declaration for the cache loader's friendship; the complete type
// lives in asset_cache.h. Cache parsing stays out of this header so the file
// format does not leak into the index API.
struct AssetCacheLoad;

class AssetIndex {
public:
    static constexpr std::size_t kMaxEntries = 150000;

    void clear();

    // Adds rows from an engine dump (or any neutral source). A loose row is
    // marked SourceLoose because the engine resolved it on disk; otherwise
    // source_flag is applied. Returns the number of new unique paths.
    std::size_t add_scanned_assets(const std::vector<ScannedAsset>& assets,
                                   std::uint8_t source_flag);

    // Recursively scans `root` for .vmt/.vtf/.mdl and stores game-relative
    // paths. `max_entries` bounds this call; the whole index is also capped at
    // kMaxEntries and hitting either cap sets truncated(). A missing or
    // unreadable root adds nothing. Returns the number of new unique paths.
    std::size_t add_directory(const std::filesystem::path& root,
                              std::uint8_t source_flag,
                              std::size_t max_entries = kMaxEntries);

    // Indices into entries(), sorted by virtual_path. Checks run cheapest
    // first: kind, source, folder prefix, then tokens.
    std::vector<std::size_t> filter(const AssetFilter& filter) const;

    const std::vector<AssetEntry>& entries() const noexcept { return entries_; }
    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }
    bool truncated() const noexcept { return truncated_; }

private:
    friend AssetCacheLoad load_asset_cache(const std::filesystem::path& file,
                                           const std::string& expected_game_dir);
    friend AssetCacheLoad parse_asset_cache(std::string_view text,
                                            const std::string& expected_game_dir);

    // Sorts, deduplicates by path_lower (merging source bits and keeping the
    // first spelling), then sorts by virtual_path. Shared by every ingestion
    // path, including cache loads.
    void merge_entries(std::vector<AssetEntry>&& added);

    std::vector<AssetEntry> entries_;
    bool truncated_ = false;
};

// Extension-based classification. Case-insensitive; anything that is not
// .vmt/.vtf/.mdl (including a missing extension) is Other.
AssetKind asset_kind_from_path(std::string_view virtual_path);

// Display label for the Type column: "Material", "Texture", "Model", "Other".
std::string asset_type_label(AssetKind kind);

} // namespace ui
