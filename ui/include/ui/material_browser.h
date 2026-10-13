#pragma once

// ui material_browser - headless index over .vmt materials plus the thumbnail
// cache conventions, so the Win32 browser window and the unit tests share one
// implementation.
//
// Two sources feed the same index:
//   - a folder scan (disk_path set, works without the engine), and
//   - the engine file system (VPK-aware, from bridge::EngineView).
//
// Design decisions worth keeping:
//   - Names are game-relative without the extension, forward slashes
//     ("brick/brickwall001a"). That is the string the engine accepts in
//     FindMaterial() and what the preview override writes back to disk.
//   - Entries are deduplicated and sorted; the scan is capped so a mistaken
//     folder pick cannot spend minutes enumerating a disk.
//   - Thumbnails are 24-bit BMP files (no third-party image encoder); the
//     file name is a stable hash of the material name so the cache survives
//     restarts and material edits simply overwrite the same file.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

struct MaterialEntry {
    std::string name;                // game-relative, no extension, '/'
    std::string display;             // with extension and source-relative path
    std::filesystem::path disk_path; // empty for engine-only (VPK) entries
    bool from_engine = false;        // true when it came from the engine scan
};

class MaterialIndex {
public:
    static constexpr std::size_t kMaxEntries = 50000;

    void clear();

    // Recursively scans `root` for .vmt files. Returns the number of entries
    // added. A missing/unreadable root adds nothing (no error state).
    std::size_t add_directory(const std::filesystem::path& root);

    // Adds names produced by the engine scan: "materials\foo\bar.vmt" or
    // "materials/foo/bar.vmt" (extension and `materials/` prefix are
    // normalized away). Returns the number of entries added.
    std::size_t add_engine_names(const std::vector<std::string>& names);

    const std::vector<MaterialEntry>& entries() const noexcept { return entries_; }
    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }
    bool truncated() const noexcept { return truncated_; }

    // Indices of entries whose name contains `query` (case-insensitive,
    // extension optional). An empty query returns every index. The result is
    // sorted by name because the index is.
    std::vector<std::size_t> search(std::string_view query) const;

private:
    std::vector<MaterialEntry> entries_;
    bool truncated_ = false;
};

// "materials\foo\bar.vmt" -> "foo/bar"; empty when the path is not a
// .vmt under materials/. Accepts both slash styles and a redundant
// leading "materials/materials/".
std::string material_name_from_game_path(std::string_view game_path);

// `file` below `root` -> a game-style material name: a leading materials/
// folder is stripped, the extension is dropped, separators become '/'.
std::string material_name_from_disk_path(const std::filesystem::path& root,
                                         const std::filesystem::path& file);

// --- thumbnail cache ---------------------------------------------------------

// Longest edge, in logical (96 dpi) pixels, of the box that frames a
// thumbnail. The box always follows the texture's own aspect ratio, so this
// bounds how large the preview can get without ever distorting the texture.
inline constexpr int kThumbnailMaxEdge = 256;

// Aspect ratios beyond this are clamped for the box: a 1024x8 strip texture
// would otherwise get a two-pixel-tall control. The texture itself is never
// stretched, so such a texture is letterboxed inside the clamped box instead.
inline constexpr int kThumbnailMaxAspect = 4;

// Stable cache file name for a material: FNV-1a of the lowercased name plus
// ".bmp" (no user-controlled characters survive into the file name).
std::string thumbnail_file_name(std::string_view material_name);

// <cache_root>/<hash>.bmp
std::filesystem::path thumbnail_cache_path(const std::filesystem::path& cache_root,
                                           std::string_view material_name);

// Writes a 24-bit BMP from 32-bit BGRA pixels. `rows_top_down` describes the
// source order: true when bgra row 0 is the top image row. BMP storage itself
// is bottom-up, so the writer reorders when needed. Creates the parent
// directory. Returns false on any I/O failure.
bool write_bmp_from_bgra(const std::filesystem::path& path, int width, int height,
                         const unsigned char* bgra, bool rows_top_down);

// Reads the dimensions of a BMP written by write_bmp_from_bgra (or any
// uncompressed 24/32-bit BMP). Returns false when the file is not a BMP.
bool read_bmp_size(const std::filesystem::path& path, int& width, int& height);

} // namespace ui
