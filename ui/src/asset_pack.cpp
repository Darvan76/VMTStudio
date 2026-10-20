#include "ui/asset_pack.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>

namespace ui {
namespace {

constexpr std::array<unsigned char, 8> kMagic = { 'V', 'M', 'T', 'P', 'A', 'C', 'K', '1' };
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kFileHeaderSize = 16;   // magic + version + reserved
constexpr std::size_t kRecordHeaderSize = 20; // flags + 3 sizes + checksum
constexpr std::uint32_t kFlagCompressed = 1u;
constexpr std::uint32_t kMaxKeySize = 4096;
constexpr std::uint32_t kMaxPayloadSize = 128u * 1024u * 1024u;

std::uint32_t read_u32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void write_u32(unsigned char* p, std::uint32_t value) {
    p[0] = static_cast<unsigned char>(value & 0xFF);
    p[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
    p[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
    p[3] = static_cast<unsigned char>((value >> 24) & 0xFF);
}

void append_u32(std::vector<unsigned char>& out, std::uint32_t value) {
    unsigned char bytes[4];
    write_u32(bytes, value);
    out.insert(out.end(), bytes, bytes + 4);
}

std::uint32_t fnv1a32(const unsigned char* data, std::size_t size) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool write_file_header(std::ostream& out) {
    out.write(reinterpret_cast<const char*>(kMagic.data()),
              static_cast<std::streamsize>(kMagic.size()));
    unsigned char tail[8];
    write_u32(tail, kVersion);
    write_u32(tail + 4, 0);
    out.write(reinterpret_cast<const char*>(tail), 8);
    return static_cast<bool>(out);
}

bool read_file_header(std::istream& in) {
    unsigned char magic[kMagic.size()];
    in.read(reinterpret_cast<char*>(magic), static_cast<std::streamsize>(sizeof(magic)));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(magic))) {
        return false;
    }
    if (std::memcmp(magic, kMagic.data(), kMagic.size()) != 0) {
        return false;
    }
    unsigned char tail[8];
    in.read(reinterpret_cast<char*>(tail), 8);
    if (in.gcount() != 8 || read_u32(tail) != kVersion) {
        return false;
    }
    return true;
}

} // namespace

std::uint64_t asset_signature(const std::filesystem::path& disk_path,
                              std::uint64_t generated) {
    if (disk_path.empty()) {
        return generated;
    }
    std::error_code time_error;
    std::error_code size_error;
    const auto stamp = std::filesystem::last_write_time(disk_path, time_error);
    const auto size = std::filesystem::file_size(disk_path, size_error);
    const auto ticks = time_error ? 0 : static_cast<long long>(stamp.time_since_epoch().count());
    const auto bytes = size_error ? 0ull : static_cast<unsigned long long>(size);
    std::string text = std::to_string(ticks);
    text.push_back(':');
    text += std::to_string(bytes);
    return fnv1a64(text);
}

std::string asset_pack_key(std::string_view path_lower, std::uint64_t signature) {
    std::string text(path_lower);
    text.push_back('|');
    text += std::to_string(signature);
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(fnv1a64(text)));
    return std::string(buffer);
}

AssetPack::AssetPack(Compressor compressor, Decompressor decompressor)
    : m_compressor(std::move(compressor)), m_decompressor(std::move(decompressor)) {}

AssetPack::~AssetPack() {
    std::lock_guard<std::mutex> lock(m_mutex);
    close_locked();
}

bool AssetPack::open(const std::filesystem::path& file, bool create_if_missing) {
    std::lock_guard<std::mutex> lock(m_mutex);
    close_locked();
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

    const bool exists = std::filesystem::exists(file, ec);
    if (!exists) {
        if (!create_if_missing) {
            return false;
        }
        std::ofstream create(file, std::ios::binary | std::ios::trunc);
        if (!create || !write_file_header(create)) {
            return false;
        }
    } else if (std::filesystem::file_size(file, ec) < kFileHeaderSize) {
        if (!create_if_missing) {
            return false;
        }
        // An empty or truncated header is not recoverable: start over, but only
        // when the caller explicitly allowed creating the archive.
        std::ofstream create(file, std::ios::binary | std::ios::trunc);
        if (!create || !write_file_header(create)) {
            return false;
        }
    }

    m_file.open(file, std::ios::in | std::ios::out | std::ios::binary);
    if (!m_file || !read_file_header(m_file)) {
        m_file.close();
        m_file.clear();
        return false;
    }

    m_path = file;
    m_index.clear();
    m_live_bytes = 0;

    std::uint64_t offset = kFileHeaderSize;
    std::uint64_t last_good = kFileHeaderSize;
    bool partial_tail = false;

    for (;;) {
        unsigned char header[kRecordHeaderSize];
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        m_file.read(reinterpret_cast<char*>(header), kRecordHeaderSize);
        const std::streamsize got = m_file.gcount();
        if (got == 0) {
            break; // clean EOF
        }
        if (got != static_cast<std::streamsize>(kRecordHeaderSize)) {
            partial_tail = true;
            break;
        }

        const std::uint32_t flags = read_u32(header);
        const std::uint32_t key_size = read_u32(header + 4);
        const std::uint32_t stored_size = read_u32(header + 8);
        const std::uint32_t raw_size = read_u32(header + 12);
        const std::uint32_t checksum = read_u32(header + 16);

        if ((flags & ~kFlagCompressed) != 0 || key_size == 0 ||
            key_size > kMaxKeySize || stored_size > kMaxPayloadSize ||
            raw_size > kMaxPayloadSize) {
            partial_tail = true;
            break;
        }

        std::string key(key_size, '\0');
        m_file.read(key.data(), static_cast<std::streamsize>(key_size));
        if (m_file.gcount() != static_cast<std::streamsize>(key_size)) {
            partial_tail = true;
            break;
        }

        std::vector<unsigned char> payload(stored_size);
        if (stored_size != 0) {
            m_file.read(reinterpret_cast<char*>(payload.data()),
                        static_cast<std::streamsize>(stored_size));
            if (m_file.gcount() != static_cast<std::streamsize>(stored_size)) {
                partial_tail = true;
                break;
            }
        }

        Entry entry;
        entry.offset = offset;
        entry.stored_size = stored_size;
        entry.raw_size = raw_size;
        entry.checksum = checksum;
        entry.compressed = (flags & kFlagCompressed) != 0;
        m_index[std::move(key)] = entry;

        offset += kRecordHeaderSize + key_size + stored_size;
        last_good = offset;
    }

    if (partial_tail && last_good < std::filesystem::file_size(file, ec)) {
        // Tolerate a crash during an append: keep every complete record and cut
        // the half-written one. The archive is reopened because Windows cannot
        // resize a file that still has an open handle.
        m_file.close();
        m_file.clear();
        std::filesystem::resize_file(file, last_good, ec);
        if (ec) {
            return false;
        }
        m_file.open(file, std::ios::in | std::ios::out | std::ios::binary);
        if (!m_file) {
            return false;
        }
    }

    for (const auto& item : m_index) {
        m_live_bytes += kRecordHeaderSize + item.first.size() + item.second.stored_size;
    }
    m_open = true;
    return true;
}

bool AssetPack::is_open() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_open;
}

std::filesystem::path AssetPack::path() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}

std::string AssetPack::combined_key(std::string_view tag, std::string_view key) {
    std::string out;
    out.reserve(tag.size() + 1 + key.size());
    out.append(tag);
    out.push_back('\x1f');
    out.append(key);
    return out;
}

bool AssetPack::get(std::string_view tag, std::string_view key,
                    std::vector<unsigned char>& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_open) {
        return false;
    }
    const std::string index_key = combined_key(tag, key);
    const auto found = m_index.find(index_key);
    if (found == m_index.end()) {
        return false;
    }
    const Entry& entry = found->second;

    const std::uint64_t payload_offset =
        entry.offset + kRecordHeaderSize + index_key.size();
    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
    if (!m_file) {
        return false;
    }

    std::vector<unsigned char> stored(entry.stored_size);
    if (entry.stored_size != 0) {
        m_file.read(reinterpret_cast<char*>(stored.data()),
                    static_cast<std::streamsize>(entry.stored_size));
        if (m_file.gcount() != static_cast<std::streamsize>(entry.stored_size)) {
            return false;
        }
    }

    if (entry.compressed) {
        if (!m_decompressor) {
            return false;
        }
        try {
            if (!m_decompressor(stored, out)) {
                return false;
            }
        } catch (...) {
            return false;
        }
        if (out.size() != entry.raw_size) {
            return false;
        }
    } else {
        if (stored.size() != entry.raw_size) {
            return false;
        }
        out = std::move(stored);
    }

    if (!out.empty() && fnv1a32(out.data(), out.size()) != entry.checksum) {
        out.clear();
        return false;
    }
    return true;
}

bool AssetPack::contains(std::string_view tag, std::string_view key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index.find(combined_key(tag, key)) != m_index.end();
}

bool AssetPack::put(std::string_view tag, std::string_view key,
                    const std::vector<unsigned char>& payload, bool compress) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_open || key.size() > kMaxKeySize || payload.size() > kMaxPayloadSize) {
        return false;
    }

    std::string index_key = combined_key(tag, key);
    if (index_key.size() > kMaxKeySize) {
        return false;
    }

    std::vector<unsigned char> stored = payload;
    bool is_compressed = false;
    if (compress && m_compressor && !payload.empty()) {
        try {
            std::vector<unsigned char> candidate = m_compressor(payload);
            if (!candidate.empty() && candidate.size() < payload.size()) {
                stored = std::move(candidate);
                is_compressed = true;
            }
        } catch (...) {
            stored = payload;
        }
    }
    m_last_put_compressed = is_compressed;

    const std::uint32_t checksum =
        payload.empty() ? 0u : fnv1a32(payload.data(), payload.size());

    m_file.clear();
    m_file.seekp(0, std::ios::end);
    const std::streampos end = m_file.tellp();
    if (end < 0) {
        return false;
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(end);

    unsigned char header[kRecordHeaderSize];
    write_u32(header, is_compressed ? kFlagCompressed : 0u);
    write_u32(header + 4, static_cast<std::uint32_t>(index_key.size()));
    write_u32(header + 8, static_cast<std::uint32_t>(stored.size()));
    write_u32(header + 12, static_cast<std::uint32_t>(payload.size()));
    write_u32(header + 16, checksum);

    m_file.write(reinterpret_cast<const char*>(header), kRecordHeaderSize);
    m_file.write(index_key.data(), static_cast<std::streamsize>(index_key.size()));
    if (!stored.empty()) {
        m_file.write(reinterpret_cast<const char*>(stored.data()),
                     static_cast<std::streamsize>(stored.size()));
    }
    m_file.flush();

    if (!m_file) {
        // Drop the partially written row so the next open does not need to.
        m_file.close();
        m_file.clear();
        std::error_code ec;
        std::filesystem::resize_file(m_path, offset, ec);
        reopen_locked();
        return false;
    }

    Entry entry;
    entry.offset = offset;
    entry.stored_size = static_cast<std::uint32_t>(stored.size());
    entry.raw_size = static_cast<std::uint32_t>(payload.size());
    entry.checksum = checksum;
    entry.compressed = is_compressed;
    m_live_bytes += kRecordHeaderSize + index_key.size() + stored.size();
    m_index[std::move(index_key)] = entry;
    return true;
}

bool AssetPack::remove_tag(std::string_view tag) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_open) {
        return false;
    }
    std::string prefix(tag);
    prefix.push_back('\x1f');
    bool removed = false;
    for (auto it = m_index.begin(); it != m_index.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = m_index.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (!removed) {
        return true;
    }
    return compact_locked();
}

bool AssetPack::compact() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return compact_locked();
}

bool AssetPack::compact_locked() {
    if (!m_open) {
        return false;
    }

    std::filesystem::path temp = m_path;
    temp += ".compact";
    std::error_code ec;
    std::filesystem::remove(temp, ec);

    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out || !write_file_header(out)) {
        out.close();
        std::filesystem::remove(temp, ec);
        return false;
    }

    std::unordered_map<std::string, Entry> new_index;
    new_index.reserve(m_index.size());

    for (const auto& item : m_index) {
        const std::string& key = item.first;
        const Entry& old = item.second;
        if (old.offset + kRecordHeaderSize + key.size() + old.stored_size >
            std::filesystem::file_size(m_path, ec)) {
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }

        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(old.offset), std::ios::beg);
        if (!m_file) {
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }

        unsigned char header[kRecordHeaderSize];
        m_file.read(reinterpret_cast<char*>(header), kRecordHeaderSize);
        if (m_file.gcount() != static_cast<std::streamsize>(kRecordHeaderSize)) {
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }

        std::string stored_key(key.size(), '\0');
        m_file.read(stored_key.data(), static_cast<std::streamsize>(key.size()));
        if (m_file.gcount() != static_cast<std::streamsize>(key.size()) ||
            stored_key != key) {
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }

        std::vector<unsigned char> payload(old.stored_size);
        if (old.stored_size != 0) {
            m_file.read(reinterpret_cast<char*>(payload.data()),
                        static_cast<std::streamsize>(old.stored_size));
            if (m_file.gcount() != static_cast<std::streamsize>(old.stored_size)) {
                out.close();
                std::filesystem::remove(temp, ec);
                return false;
            }
        }

        const std::streampos pos = out.tellp();
        if (pos < 0) {
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }

        out.write(reinterpret_cast<const char*>(header), kRecordHeaderSize);
        out.write(key.data(), static_cast<std::streamsize>(key.size()));
        if (!payload.empty()) {
            out.write(reinterpret_cast<const char*>(payload.data()),
                      static_cast<std::streamsize>(payload.size()));
        }

        Entry updated = old;
        updated.offset = static_cast<std::uint64_t>(pos);
        new_index[key] = updated;
    }

    out.flush();
    out.close();
    if (!out) {
        std::filesystem::remove(temp, ec);
        return false;
    }

    m_file.close();
    m_file.clear();

    std::filesystem::rename(temp, m_path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
        ec.clear();
        std::filesystem::rename(temp, m_path, ec);
    }
    if (ec) {
        std::filesystem::remove(temp, ec);
        reopen_locked();
        return false;
    }

    if (!reopen_locked()) {
        m_open = false;
        return false;
    }

    m_index = std::move(new_index);
    m_live_bytes = 0;
    for (const auto& item : m_index) {
        m_live_bytes += kRecordHeaderSize + item.first.size() + item.second.stored_size;
    }
    return true;
}

std::size_t AssetPack::live_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_index.size();
}

std::uint64_t AssetPack::file_size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::error_code ec;
    const auto size = std::filesystem::file_size(m_path, ec);
    return ec ? 0ull : static_cast<std::uint64_t>(size);
}

bool AssetPack::last_put_compressed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_put_compressed;
}

bool AssetPack::reopen_locked() {
    m_file.close();
    m_file.clear();
    m_file.open(m_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!m_file) {
        m_open = false;
        return false;
    }
    m_open = true;
    return true;
}

void AssetPack::close_locked() {
    if (m_file.is_open()) {
        m_file.close();
    }
    m_file.clear();
    m_index.clear();
    m_live_bytes = 0;
    m_open = false;
    m_path.clear();
}

} // namespace ui
