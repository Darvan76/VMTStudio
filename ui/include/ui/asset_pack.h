#pragma once

// ui asset_pack - one append-only archive per game for the Asset Browser.
//
// The browser used to keep a TSV index and thousands of loose *.bmp thumbnails.
// That is slow on cold starts and leaves a large directory behind, so both now
// live in a single `cache.pack`:
//
//   header  "VMTPACK1" + version + reserved
//   record  flags, key size, stored size, raw size, checksum
//           key bytes (tag + 0x1f + caller key)
//           payload bytes (compressed or as-is)
//
// Records are appended; a later record with the same key supersedes the older
// one. Opening scans the file and builds an in-memory offset index, so reads
// are random access. compact() rewrites only the live records, and a partially
// written trailing record is discarded on open instead of failing the file.
//
// Compression is injected: the UI supplies qCompress/qUncompress, while the
// headless tests use a tiny RLE codec (or no codec at all). This keeps the
// core Qt-free and testable. Thumbnails are already PNG, so callers store those
// uncompressed and only compress the index.
//
// Thread-safety: every public method locks an internal mutex. Asset thumbnail
// workers read and write the same pack while the UI loads or compacts it.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ui {

inline constexpr std::string_view kAssetPackTagMeta = "meta";
inline constexpr std::string_view kAssetPackTagThumb = "thumb";
inline constexpr std::string_view kAssetPackTagModel = "model";

// Loose files are identified by their write time and size, packed files by the
// index's generated stamp. The result feeds asset_pack_key().
std::uint64_t asset_signature(const std::filesystem::path& disk_path,
                              std::uint64_t generated);

// 16 lowercase hex chars: fnv1a64(path_lower + "|" + decimal signature). The
// old loose thumbnail cache used exactly this name, which lets migration import
// its *.bmp files without knowing which asset each one belongs to.
std::string asset_pack_key(std::string_view path_lower, std::uint64_t signature);

class AssetPack {
public:
    using Compressor =
        std::function<std::vector<unsigned char>(const std::vector<unsigned char>&)>;
    using Decompressor =
        std::function<bool(const std::vector<unsigned char>&, std::vector<unsigned char>&)>;

    AssetPack() = default;
    AssetPack(Compressor compressor, Decompressor decompressor);
    ~AssetPack();

    AssetPack(const AssetPack&) = delete;
    AssetPack& operator=(const AssetPack&) = delete;

    // Creates the parent directory and an empty archive when it does not exist.
    // An existing file with a foreign/corrupt header is rejected instead of
    // overwritten. A partial trailing record is truncated to the last good one.
    bool open(const std::filesystem::path& file, bool create_if_missing = true);

    bool is_open() const;
    std::filesystem::path path() const;

    // `tag` is one of the kAssetPackTag* values, `key` the caller's key.
    bool get(std::string_view tag, std::string_view key,
             std::vector<unsigned char>& out);
    bool contains(std::string_view tag, std::string_view key) const;
    bool put(std::string_view tag, std::string_view key,
             const std::vector<unsigned char>& payload, bool compress = true);

    // Drops every record with this tag from the live index and rewrites the
    // archive. Used by Refresh to invalidate thumbnails/model snapshots.
    bool remove_tag(std::string_view tag);

    // Rewrites the archive with one record per live key (drops superseded and
    // removed rows). Atomic: the new file is renamed over the old one.
    bool compact();

    std::size_t live_count() const;
    std::uint64_t file_size() const;
    bool last_put_compressed() const;

private:
    struct Entry {
        std::uint64_t offset = 0;
        std::uint32_t stored_size = 0;
        std::uint32_t raw_size = 0;
        std::uint32_t checksum = 0;
        bool compressed = false;
    };

    static std::string combined_key(std::string_view tag, std::string_view key);
    bool compact_locked();
    bool reopen_locked();
    void close_locked();

    mutable std::mutex m_mutex;
    std::fstream m_file;
    std::filesystem::path m_path;
    Compressor m_compressor;
    Decompressor m_decompressor;
    std::unordered_map<std::string, Entry> m_index;
    std::uint64_t m_live_bytes = 0;
    bool m_open = false;
    bool m_last_put_compressed = false;
};

} // namespace ui
