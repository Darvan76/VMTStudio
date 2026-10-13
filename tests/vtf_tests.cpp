#include "ut.h"

#include "vtf/vtf_reader.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifndef VTF_TEST_DATA_DIR
#error "VTF_TEST_DATA_DIR must be defined by the build system (tests/CMakeLists.txt)"
#endif

// Fixture layout expected under tests/data/vtf/:
//
//   expected.json         - manifest describing every valid fixture (optional:
//                           without it the fixture checks report PENDING)
//   *.vtf                 - the real VTF files
//   truncated.vtf         - optional: a truncated file; it must fail cleanly
//   wrong_signature.vtf   - optional: a file whose "VTF" signature is corrupt
//
// The committed fixture set is regenerated with the maintenance tool in
// vtf_reader/tools/make_vtf_fixtures.cpp (see vtf_reader/CMakeLists.txt).
//
// expected.json schema (extra keys are ignored):
//
//   {
//     "textures": [
//       {
//         "file": "dxt1.vtf",
//         "width": 256,
//         "height": 256,
//         "format": "DXT1",
//         "frames": 1,
//         "mips": 9,          // optional
//         "alpha": false      // optional
//       }
//     ]
//   }

namespace {

namespace fs = std::filesystem;

// --- Small helpers ----------------------------------------------------------

fs::path fixture_dir() {
    return fs::path(VTF_TEST_DATA_DIR);
}

fs::path fresh_temp_dir() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "vmtviewer_vtf_tests";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(stream);
}

// Builds the 16-byte VTF file header: signature, version[2], header size.
std::vector<std::uint8_t> make_file_header(std::uint32_t major,
                                           std::uint32_t minor,
                                           std::uint32_t header_size) {
    std::vector<std::uint8_t> bytes(16, 0);
    bytes[0] = 'V';
    bytes[1] = 'T';
    bytes[2] = 'F';
    bytes[3] = '\0';
    const auto put_u32 = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset + 0] = static_cast<std::uint8_t>(value & 0xFFu);
        bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
        bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
        bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
    };
    put_u32(4, major);
    put_u32(8, minor);
    put_u32(12, header_size);
    return bytes;
}

double seconds_between(std::chrono::steady_clock::time_point start,
                       std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double>(end - start).count();
}

// --- Minimal expected.json reader -------------------------------------------
//
// The manifest is a tiny, fixed-shape document, so a full JSON library would
// be overkill. Unknown keys are skipped; malformed input fails the test with
// a readable message.

struct ExpectedTexture {
    std::string file;
    std::string format;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frames = 0;
    bool has_mips = false;
    std::uint32_t mips = 0;
    bool has_alpha = false;
    bool alpha = false;
};

class ManifestReader {
public:
    explicit ManifestReader(std::string text) : text_(std::move(text)) {}

    bool parse(std::vector<ExpectedTexture>& textures) {
        textures.clear();
        skip_ws();
        if (!consume('{')) {
            error_ = "expected '{' at the start of expected.json";
            return false;
        }
        bool saw_textures = false;
        for (;;) {
            skip_ws();
            if (consume('}')) {
                break;
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (!consume(':')) {
                error_ = "expected ':' after key '" + key + "'";
                return false;
            }
            skip_ws();
            if (key == "textures") {
                if (!parse_texture_array(textures)) {
                    return false;
                }
                saw_textures = true;
            } else if (!skip_value()) {
                return false;
            }
            skip_ws();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                break;
            }
            error_ = "expected ',' or '}' at top level";
            return false;
        }
        if (!saw_textures) {
            error_ = "expected.json has no \"textures\" array";
            return false;
        }
        return true;
    }

    const std::string& error() const {
        return error_;
    }

private:
    bool parse_texture_array(std::vector<ExpectedTexture>& out) {
        if (!consume('[')) {
            error_ = "\"textures\" must be an array";
            return false;
        }
        skip_ws();
        if (consume(']')) {
            return true;
        }
        for (;;) {
            skip_ws();
            ExpectedTexture texture;
            if (!parse_texture(texture)) {
                return false;
            }
            out.push_back(std::move(texture));
            skip_ws();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                return true;
            }
            error_ = "expected ',' or ']' inside \"textures\"";
            return false;
        }
    }

    bool parse_texture(ExpectedTexture& texture) {
        if (!consume('{')) {
            error_ = "each \"textures\" entry must be an object";
            return false;
        }
        for (;;) {
            skip_ws();
            if (consume('}')) {
                break;
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (!consume(':')) {
                error_ = "expected ':' inside a texture entry";
                return false;
            }
            skip_ws();
            bool ok = true;
            if (key == "file") {
                ok = parse_string(texture.file);
            } else if (key == "format") {
                ok = parse_string(texture.format);
            } else if (key == "width") {
                ok = parse_uint(texture.width);
            } else if (key == "height") {
                ok = parse_uint(texture.height);
            } else if (key == "frames") {
                ok = parse_uint(texture.frames);
            } else if (key == "mips") {
                texture.has_mips = true;
                ok = parse_uint(texture.mips);
            } else if (key == "alpha") {
                texture.has_alpha = true;
                ok = parse_bool(texture.alpha);
            } else {
                ok = skip_value();
            }
            if (!ok) {
                if (error_.empty()) {
                    error_ = "invalid value for key '" + key + "'";
                }
                return false;
            }
            skip_ws();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                break;
            }
            error_ = "expected ',' or '}' inside a texture entry";
            return false;
        }
        if (texture.file.empty() || texture.format.empty() ||
            texture.width == 0 || texture.height == 0 || texture.frames == 0) {
            error_ = "each texture entry needs \"file\", \"format\" and positive \"width\", \"height\", \"frames\"";
            return false;
        }
        return true;
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) {
            error_ = "expected a string";
            return false;
        }
        out.clear();
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (position_ >= text_.size()) {
                    break;
                }
                const char escaped = text_[position_++];
                switch (escaped) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'u':
                        // The schema is ASCII-only; keep it simple but skip the escape.
                        for (int i = 0; i < 4 && position_ < text_.size(); ++i) {
                            ++position_;
                        }
                        out.push_back('?');
                        break;
                    default: out.push_back(escaped); break;
                }
                continue;
            }
            out.push_back(c);
        }
        error_ = "unterminated string";
        return false;
    }

    bool parse_uint(std::uint32_t& out) {
        if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
            error_ = "expected a non-negative integer";
            return false;
        }
        std::uint64_t value = 0;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            value = value * 10u + static_cast<std::uint64_t>(text_[position_] - '0');
            if (value > 0xFFFFFFFFull) {
                error_ = "integer does not fit in 32 bits";
                return false;
            }
            ++position_;
        }
        out = static_cast<std::uint32_t>(value);
        return true;
    }

    bool parse_bool(bool& out) {
        if (text_.compare(position_, 4, "true") == 0) {
            out = true;
            position_ += 4;
            return true;
        }
        if (text_.compare(position_, 5, "false") == 0) {
            out = false;
            position_ += 5;
            return true;
        }
        error_ = "expected true or false";
        return false;
    }

    bool skip_value() {
        skip_ws();
        if (position_ >= text_.size()) {
            error_ = "unexpected end of file";
            return false;
        }
        const char c = text_[position_];
        if (c == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (c == '{' || c == '[') {
            int depth = 0;
            bool in_string = false;
            while (position_ < text_.size()) {
                const char ch = text_[position_++];
                if (in_string) {
                    if (ch == '\\') {
                        ++position_;
                    } else if (ch == '"') {
                        in_string = false;
                    }
                    continue;
                }
                if (ch == '"') {
                    in_string = true;
                } else if (ch == '{' || ch == '[') {
                    ++depth;
                } else if (ch == '}' || ch == ']') {
                    --depth;
                    if (depth == 0) {
                        return true;
                    }
                }
            }
            error_ = "unterminated object or array";
            return false;
        }
        // Number, boolean or null: consume up to the next delimiter.
        while (position_ < text_.size()) {
            const char ch = text_[position_];
            if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
                ch == '\r' || ch == '\n') {
                break;
            }
            ++position_;
        }
        return true;
    }

    void skip_ws() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++position_;
            } else {
                break;
            }
        }
    }

    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    std::string text_;
    std::size_t position_ = 0;
    std::string error_;
};

bool read_text_file(const fs::path& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    out = std::move(content);
    return true;
}

} // namespace

// --- Tests ------------------------------------------------------------------

UT_TEST_CASE("vtf: missing file and directory report errors, not crashes") {
    const fs::path missing = fresh_temp_dir() / "missing.vtf";
    const auto info = vtf::open_vtf_info(missing);
    UT_CHECK(!info.ok());
    UT_CHECK(info.error().code == vmt::ErrorCode::NotFound);
    UT_CHECK(!info.error().message.empty());

    const auto preview = vtf::decode_vtf_preview(missing, 0, 64);
    UT_CHECK(!preview.ok());
    UT_CHECK(preview.error().code == vmt::ErrorCode::NotFound);

    const auto directory = vtf::open_vtf_info(fresh_temp_dir());
    UT_CHECK(!directory.ok());
}

UT_TEST_CASE("vtf: wrong signature reports VtfInvalidHeader") {
    const fs::path dir = fresh_temp_dir();
    std::vector<std::uint8_t> bytes = make_file_header(7, 6, 16);
    bytes[0] = 'X'; // corrupt the "VTF" signature
    bytes.resize(64, 0);
    const fs::path file = dir / "wrong_signature.vtf";
    UT_CHECK(write_bytes(file, bytes));

    const auto info = vtf::open_vtf_info(file);
    UT_CHECK(!info.ok());
    UT_CHECK(info.error().code == vmt::ErrorCode::VtfInvalidHeader);

    const auto preview = vtf::decode_vtf_preview(file, 0, 64);
    UT_CHECK(!preview.ok());
    UT_CHECK(preview.error().code == vmt::ErrorCode::VtfInvalidHeader);

    const fs::path fixture = fixture_dir() / "wrong_signature.vtf";
    if (fs::exists(fixture)) {
        const auto fixture_info = vtf::open_vtf_info(fixture);
        UT_CHECK(!fixture_info.ok());
        const auto fixture_preview = vtf::decode_vtf_preview(fixture, 0, 64);
        UT_CHECK(!fixture_preview.ok());
    }
}

UT_TEST_CASE("vtf: truncated input reports VtfTruncated") {
    const fs::path dir = fresh_temp_dir();

    // Case A: fewer bytes than a file header.
    {
        std::vector<std::uint8_t> bytes = {'V', 'T', 'F', 0};
        const fs::path file = dir / "tiny.vtf";
        UT_CHECK(write_bytes(file, bytes));
        const auto info = vtf::open_vtf_info(file);
        UT_CHECK(!info.ok());
        UT_CHECK(info.error().code == vmt::ErrorCode::VtfTruncated);
    }

    // Case B: valid file header, but the declared header is cut off.
    {
        std::vector<std::uint8_t> bytes = make_file_header(7, 6, 64);
        bytes.resize(20, 0);
        const fs::path file = dir / "header_cut.vtf";
        UT_CHECK(write_bytes(file, bytes));
        const auto info = vtf::open_vtf_info(file);
        UT_CHECK(!info.ok());
        UT_CHECK(info.error().code == vmt::ErrorCode::VtfTruncated);
        const auto preview = vtf::decode_vtf_preview(file, 0, 64);
        UT_CHECK(!preview.ok());
        UT_CHECK(preview.error().code == vmt::ErrorCode::VtfTruncated);
    }

    // Case C: a sub-minimum declared header size is corrupt, not truncated.
    {
        std::vector<std::uint8_t> bytes = make_file_header(7, 6, 8);
        bytes.resize(64, 0);
        const fs::path file = dir / "bad_header_size.vtf";
        UT_CHECK(write_bytes(file, bytes));
        const auto info = vtf::open_vtf_info(file);
        UT_CHECK(!info.ok());
        UT_CHECK(info.error().code == vmt::ErrorCode::VtfInvalidHeader);
    }

    // Provided fixture if present.
    const fs::path fixture = fixture_dir() / "truncated.vtf";
    if (fs::exists(fixture)) {
        const auto info = vtf::open_vtf_info(fixture);
        UT_CHECK(!info.ok());
        UT_CHECK(!info.error().message.empty());
        const auto preview = vtf::decode_vtf_preview(fixture, 0, 64);
        UT_CHECK(!preview.ok());
    }
}

UT_TEST_CASE("vtf: unsupported versions report VtfUnsupportedVersion") {
    const fs::path dir = fresh_temp_dir();

    // 7.7 is newer than the pinned VTFLib++ supports (7.0 - 7.6).
    {
        std::vector<std::uint8_t> bytes = make_file_header(7, 7, 16);
        bytes.resize(64, 0);
        const fs::path file = dir / "future.vtf";
        UT_CHECK(write_bytes(file, bytes));
        const auto info = vtf::open_vtf_info(file);
        UT_CHECK(!info.ok());
        UT_CHECK(info.error().code == vmt::ErrorCode::VtfUnsupportedVersion);
    }

    // 6.x is a different major version.
    {
        std::vector<std::uint8_t> bytes = make_file_header(6, 4, 16);
        bytes.resize(64, 0);
        const fs::path file = dir / "old.vtf";
        UT_CHECK(write_bytes(file, bytes));
        const auto info = vtf::open_vtf_info(file);
        UT_CHECK(!info.ok());
        UT_CHECK(info.error().code == vmt::ErrorCode::VtfUnsupportedVersion);
    }
}

UT_TEST_CASE("vtf: fixture files described by expected.json") {
    const fs::path manifest = fixture_dir() / "expected.json";
    if (!fs::exists(manifest)) {
        std::printf("  [PENDING] %s not found; add it to enable the fixture checks "
                    "(schema documented at the top of tests/vtf_tests.cpp)\n",
                    manifest.string().c_str());
        return;
    }

    std::string text;
    if (!read_text_file(manifest, text)) {
        UT_CHECK(false);
        return;
    }

    std::vector<ExpectedTexture> textures;
    ManifestReader reader(std::move(text));
    const bool parsed = reader.parse(textures);
    UT_CHECK(parsed);
    if (!parsed) {
        std::printf("  expected.json: %s\n", reader.error().c_str());
        return;
    }
    UT_CHECK(!textures.empty());

    bool saw_dxt1 = false;
    bool saw_dxt5 = false;
    bool saw_rgba8888 = false;
    bool saw_mipmaps = false;
    bool saw_alpha = false;
    bool saw_multiframe = false;

    for (const ExpectedTexture& expected : textures) {
        const fs::path file = fixture_dir() / expected.file;
        UT_CHECK(fs::exists(file));
        if (!fs::exists(file)) {
            continue;
        }

        const auto info = vtf::open_vtf_info(file);
        UT_CHECK(info.ok());
        if (!info.ok()) {
            std::printf("  %s: %s\n", expected.file.c_str(), info.error().message.c_str());
            continue;
        }

        UT_CHECK_EQ(info.value().width, expected.width);
        UT_CHECK_EQ(info.value().height, expected.height);
        UT_CHECK_EQ(info.value().format_name, expected.format);
        UT_CHECK_EQ(info.value().frame_count, expected.frames);
        if (expected.has_mips) {
            UT_CHECK_EQ(info.value().mip_count, expected.mips);
        }
        if (expected.has_alpha) {
            UT_CHECK_EQ(info.value().has_alpha, expected.alpha);
        }
        UT_CHECK(info.value().preview_supported);

        saw_dxt1 = saw_dxt1 || expected.format == "DXT1";
        saw_dxt5 = saw_dxt5 || expected.format == "DXT5";
        saw_rgba8888 = saw_rgba8888 || expected.format == "RGBA8888";
        saw_mipmaps = saw_mipmaps || info.value().mip_count > 1;
        saw_alpha = saw_alpha || info.value().has_alpha;
        saw_multiframe = saw_multiframe || expected.frames > 1;

        if (!info.value().preview_supported) {
            continue;
        }

        // Full-size decode must match the header exactly.
        const auto full = vtf::decode_vtf_preview(file, 0, 0);
        UT_CHECK(full.ok());
        if (full.ok()) {
            UT_CHECK_EQ(full.value().width, expected.width);
            UT_CHECK_EQ(full.value().height, expected.height);
            UT_CHECK_EQ(full.value().pixels.size(),
                        static_cast<std::size_t>(expected.width) * expected.height * 4);
        }

        // Downscaled decode must fit the box and keep the aspect ratio.
        const auto preview = vtf::decode_vtf_preview(file, 0, 96);
        UT_CHECK(preview.ok());
        if (preview.ok()) {
            const std::uint32_t longest = std::max(preview.value().width, preview.value().height);
            UT_CHECK(longest <= 96u);
            UT_CHECK(longest >= 1u);
            UT_CHECK_EQ(preview.value().pixels.size(),
                        static_cast<std::size_t>(preview.value().width) * preview.value().height * 4);
            const double source_ratio =
                static_cast<double>(expected.width) / static_cast<double>(expected.height);
            const double preview_ratio =
                static_cast<double>(preview.value().width) / static_cast<double>(preview.value().height);
            UT_CHECK(preview_ratio > source_ratio * 0.98 && preview_ratio < source_ratio * 1.02);
        }

        // Extra frames must be decodable when the file has them.
        if (expected.frames > 1) {
            const auto second = vtf::decode_vtf_preview(file, 1, 64);
            UT_CHECK(second.ok());
        }

        // Out-of-range frames must be rejected as bad arguments.
        const auto beyond = vtf::decode_vtf_preview(file, expected.frames, 64);
        UT_CHECK(!beyond.ok());
        UT_CHECK(beyond.error().code == vmt::ErrorCode::InvalidArgument);
    }

    // The committed fixture set is required to be complete: the manifest must
    // cover the preview-relevant format matrix and the frame paths.
    if (!(saw_dxt1 && saw_dxt5 && saw_rgba8888 && saw_mipmaps && saw_alpha)) {
        std::printf("  fixture coverage is incomplete; missing:");
        if (!saw_dxt1) {
            std::printf(" DXT1");
        }
        if (!saw_dxt5) {
            std::printf(" DXT5");
        }
        if (!saw_rgba8888) {
            std::printf(" RGBA8888");
        }
        if (!saw_mipmaps) {
            std::printf(" mipmaps");
        }
        if (!saw_alpha) {
            std::printf(" alpha");
        }
        std::printf("\n");
    }
    UT_CHECK(saw_dxt1);
    UT_CHECK(saw_dxt5);
    UT_CHECK(saw_rgba8888);
    UT_CHECK(saw_mipmaps);
    UT_CHECK(saw_alpha);
    UT_CHECK(saw_multiframe);
}

UT_TEST_CASE("vtf: format support list is coherent") {
    const std::vector<vtf::FormatSupport> formats = vtf::list_formats();
    UT_CHECK(!formats.empty());

    std::uint32_t previous_id = 0;
    bool first = true;
    for (const vtf::FormatSupport& entry : formats) {
        UT_CHECK(!entry.name.empty());
        UT_CHECK(entry.preview_supported == entry.reason.empty());
        if (!first) {
            UT_CHECK(entry.format_id > previous_id);
        }
        previous_id = entry.format_id;
        first = false;
    }

    const auto find = [&formats](const char* name) -> const vtf::FormatSupport* {
        for (const vtf::FormatSupport& entry : formats) {
            if (entry.name == name) {
                return &entry;
            }
        }
        return nullptr;
    };

    const vtf::FormatSupport* dxt1 = find("DXT1");
    UT_CHECK(dxt1 != nullptr);
    if (dxt1 != nullptr) {
        UT_CHECK(dxt1->preview_supported);
    }
    const vtf::FormatSupport* dxt5 = find("DXT5");
    UT_CHECK(dxt5 != nullptr);
    if (dxt5 != nullptr) {
        UT_CHECK(dxt5->preview_supported);
    }
    const vtf::FormatSupport* rgba8888 = find("RGBA8888");
    UT_CHECK(rgba8888 != nullptr);
    if (rgba8888 != nullptr) {
        UT_CHECK(rgba8888->preview_supported);
    }

    const vtf::FormatSupport* rgba16f = find("RGBA16161616F");
    UT_CHECK(rgba16f != nullptr);
    if (rgba16f != nullptr) {
        UT_CHECK(!rgba16f->preview_supported);
        UT_CHECK(!rgba16f->reason.empty());
    }
    const vtf::FormatSupport* r32f = find("R32F");
    UT_CHECK(r32f != nullptr);
    if (r32f != nullptr) {
        UT_CHECK(!r32f->preview_supported);
    }
    const vtf::FormatSupport* p8 = find("P8");
    UT_CHECK(p8 != nullptr);
    if (p8 != nullptr) {
        UT_CHECK(!p8->preview_supported);
    }
}

UT_TEST_CASE("vtf: fuzz - arbitrary bytes never crash or hang") {
    const fs::path dir = fresh_temp_dir();
    std::mt19937 rng(0x5EED1234u);
    std::uniform_int_distribution<std::uint32_t> size_dist(0u, 4096u);

    const auto started = std::chrono::steady_clock::now();
    constexpr int kSamples = 150;
    int processed = 0;

    for (int sample = 0; sample < kSamples; ++sample) {
        std::vector<std::uint8_t> bytes(size_dist(rng));
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(rng() & 0xFFu);
        }

        // Every third sample gets a valid signature and version so that it
        // reaches the real VTFLib++ parser instead of the pre-check.
        if (sample % 3 == 0) {
            if (bytes.size() < 32) {
                bytes.resize(32);
            }
            bytes[0] = 'V';
            bytes[1] = 'T';
            bytes[2] = 'F';
            bytes[3] = '\0';
            bytes[4] = 7;
            bytes[5] = 0;
            bytes[6] = 0;
            bytes[7] = 0;
            bytes[8] = static_cast<std::uint8_t>(rng() % 8u);
            bytes[9] = 0;
            bytes[10] = 0;
            bytes[11] = 0;
        }

        const fs::path file = dir / ("sample_" + std::to_string(sample) + ".vtf");
        if (!write_bytes(file, bytes)) {
            UT_CHECK(false);
            continue;
        }

        const auto info_start = std::chrono::steady_clock::now();
        const auto info = vtf::open_vtf_info(file);
        const auto info_end = std::chrono::steady_clock::now();
        UT_CHECK(seconds_between(info_start, info_end) < 2.0);
        if (!info.ok()) {
            UT_CHECK(!info.error().message.empty());
        }

        const auto preview_start = std::chrono::steady_clock::now();
        const auto preview = vtf::decode_vtf_preview(file, 0, 64);
        const auto preview_end = std::chrono::steady_clock::now();
        UT_CHECK(seconds_between(preview_start, preview_end) < 2.0);
        if (!preview.ok()) {
            UT_CHECK(!preview.error().message.empty());
        } else {
            UT_CHECK_EQ(preview.value().pixels.size(),
                        static_cast<std::size_t>(preview.value().width) * preview.value().height * 4);
        }
        ++processed;
    }
    UT_CHECK_EQ(processed, kSamples);

    // Truncations of a valid-shaped header at every length.
    const std::vector<std::uint8_t> full = make_file_header(7, 6, 32);
    for (std::size_t length = 0; length <= 48; ++length) {
        std::vector<std::uint8_t> bytes(full.begin(),
                                        full.begin() + static_cast<std::ptrdiff_t>(
                                                            std::min(length, full.size())));
        bytes.resize(length, 0);
        const fs::path file = dir / ("cut_" + std::to_string(length) + ".vtf");
        if (!write_bytes(file, bytes)) {
            UT_CHECK(false);
            continue;
        }
        const auto info = vtf::open_vtf_info(file);
        if (!info.ok()) {
            UT_CHECK(!info.error().message.empty());
        }
        const auto preview = vtf::decode_vtf_preview(file, 0, 32);
        if (!preview.ok()) {
            UT_CHECK(!preview.error().message.empty());
        }
    }

    const double total_seconds = seconds_between(started, std::chrono::steady_clock::now());
    UT_CHECK(total_seconds < 60.0);
}

// --- memory-backed entry points ---------------------------------------------
//
// The Asset Browser decodes textures that live inside VPK archives, so the
// decoder has to work on a buffer rather than a path. These tests pin the
// equivalence with the file-based API and the input-validation contract.

UT_TEST_CASE("vtf memory: decoding a buffer matches decoding the same file") {
    const fs::path fixture = fixture_dir() / "brick05.vtf";
    if (!fs::exists(fixture)) {
        return;
    }

    std::ifstream stream(fixture, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
    UT_CHECK(!bytes.empty());

    const auto from_file = vtf::decode_vtf_preview(fixture, 0, 64);
    const auto from_memory =
        vtf::decode_vtf_preview_from_memory(bytes.data(), bytes.size(), 0, 64, fixture);
    UT_CHECK(from_file.ok());
    UT_CHECK(from_memory.ok());
    if (!from_file.ok() || !from_memory.ok()) {
        return;
    }

    UT_CHECK_EQ(from_memory.value().width, from_file.value().width);
    UT_CHECK_EQ(from_memory.value().height, from_file.value().height);
    // Identical pixels, not merely plausible ones: the buffer path must not
    // differ from the file path in any way the browser would notice.
    UT_CHECK(from_memory.value().pixels == from_file.value().pixels);
}

UT_TEST_CASE("vtf memory: metadata matches the file-based read") {
    const fs::path fixture = fixture_dir() / "brick05.vtf";
    if (!fs::exists(fixture)) {
        return;
    }

    std::ifstream stream(fixture, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());

    const auto from_file = vtf::open_vtf_info(fixture);
    const auto from_memory = vtf::open_vtf_info_from_memory(bytes.data(), bytes.size(), fixture);
    UT_CHECK(from_file.ok());
    UT_CHECK(from_memory.ok());
    if (!from_file.ok() || !from_memory.ok()) {
        return;
    }

    const vtf::VtfInfo& a = from_file.value();
    const vtf::VtfInfo& b = from_memory.value();
    UT_CHECK_EQ(b.width, a.width);
    UT_CHECK_EQ(b.height, a.height);
    UT_CHECK_EQ(b.mip_count, a.mip_count);
    UT_CHECK_EQ(b.frame_count, a.frame_count);
    UT_CHECK_EQ(b.face_count, a.face_count);
    UT_CHECK_EQ(b.format_name, a.format_name);
    UT_CHECK_EQ(b.has_alpha, a.has_alpha);
    UT_CHECK_EQ(b.preview_supported, a.preview_supported);
}

UT_TEST_CASE("vtf memory: a null buffer is rejected, not dereferenced") {
    const auto info = vtf::open_vtf_info_from_memory(nullptr, 0, "materials/none.vtf");
    UT_CHECK(!info.ok());
    if (!info.ok()) {
        UT_CHECK_EQ(static_cast<int>(info.error().code),
                    static_cast<int>(vmt::ErrorCode::InvalidArgument));
        UT_CHECK(!info.error().message.empty());
    }

    const auto preview = vtf::decode_vtf_preview_from_memory(nullptr, 0, 0, 64);
    UT_CHECK(!preview.ok());
    if (!preview.ok()) {
        UT_CHECK_EQ(static_cast<int>(preview.error().code),
                    static_cast<int>(vmt::ErrorCode::InvalidArgument));
    }
}

UT_TEST_CASE("vtf memory: header errors are classified the same as from a file") {
    const fs::path dir = fresh_temp_dir();

    // Wrong signature.
    std::vector<std::uint8_t> bad_signature = make_file_header(7, 6, 32);
    bad_signature[0] = 'X';
    UT_CHECK_EQ(static_cast<int>(
                    vtf::open_vtf_info_from_memory(bad_signature.data(), bad_signature.size())
                        .error().code),
                static_cast<int>(vmt::ErrorCode::VtfInvalidHeader));

    // Unsupported version.
    const std::vector<std::uint8_t> future = make_file_header(7, 7, 32);
    const auto future_info = vtf::open_vtf_info_from_memory(future.data(), future.size());
    UT_CHECK(!future_info.ok());
    if (!future_info.ok()) {
        UT_CHECK_EQ(static_cast<int>(future_info.error().code),
                    static_cast<int>(vmt::ErrorCode::VtfUnsupportedVersion));
    }

    // Shorter than a header.
    const std::vector<std::uint8_t> stub = { 'V', 'T', 'F', 0 };
    const auto stub_info = vtf::open_vtf_info_from_memory(stub.data(), stub.size());
    UT_CHECK(!stub_info.ok());
    if (!stub_info.ok()) {
        UT_CHECK_EQ(static_cast<int>(stub_info.error().code),
                    static_cast<int>(vmt::ErrorCode::VtfTruncated));
    }

    UT_CHECK(!dir.empty());
}

UT_TEST_CASE("vtf memory: the origin label reaches the error") {
    // The origin is only diagnostics, but the browser shows it in its status
    // line, so a bad buffer must be attributable to the texture it came from.
    // The message text is the decoder's to choose; the structured `file` field
    // is the contract, so that is what is asserted.
    const std::vector<std::uint8_t> stub = { 'V', 'T', 'F', 0 };
    const auto info =
        vtf::open_vtf_info_from_memory(stub.data(), stub.size(), "materials/brick/brick05.vtf");
    UT_CHECK(!info.ok());
    if (!info.ok()) {
        UT_CHECK(info.error().file.find("brick05") != std::string::npos);
        UT_CHECK(!info.error().message.empty());
    }
}

UT_TEST_CASE("vtf memory: a frame index out of range is still InvalidArgument") {
    const fs::path fixture = fixture_dir() / "brick05.vtf";
    if (!fs::exists(fixture)) {
        return;
    }

    std::ifstream stream(fixture, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());

    const auto preview = vtf::decode_vtf_preview_from_memory(bytes.data(), bytes.size(), 99, 64,
                                                            fixture);
    UT_CHECK(!preview.ok());
    if (!preview.ok()) {
        UT_CHECK_EQ(static_cast<int>(preview.error().code),
                    static_cast<int>(vmt::ErrorCode::InvalidArgument));
    }
}
