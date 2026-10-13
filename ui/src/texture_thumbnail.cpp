#include "ui/texture_thumbnail.h"

#include "vmt/document.h"
#include "vtf/vtf_reader.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>

namespace ui {
namespace {

bool starts_with_ci(const std::string& text, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (text.size() < n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

bool ends_with_ci(const std::string& text, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    if (text.size() < n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[text.size() - n + i])) != suffix[i]) {
            return false;
        }
    }
    return true;
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim_copy(std::string_view text) {
    const auto is_space = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    std::size_t begin = 0;
    while (begin < text.size() && is_space(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

// "$texture2" -> 2. Returns 0 when the key is not a numbered texture parameter.
unsigned texture_index_of(std::string_view key) {
    constexpr std::string_view kPrefix = "$texture";
    if (key.size() <= kPrefix.size()) {
        return 0;
    }
    if (key.substr(0, kPrefix.size()) != kPrefix) {
        return 0;
    }
    unsigned value = 0;
    for (std::size_t i = kPrefix.size(); i < key.size(); ++i) {
        const char c = key[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value;
}

bool ends_with_ci(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[text.size() - suffix.size() + i]);
        if (std::tolower(c) != static_cast<unsigned char>(
                                 std::tolower(static_cast<unsigned char>(suffix[i])))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool is_texture_parameter_name(std::string_view key) {
    if (key.empty()) {
        return false;
    }
    const std::string lower = to_lower(std::string(key));

    if (lower == "$basetexture" || texture_index_of(lower) != 0) {
        return true;
    }
    // $basetexture2 (LightmappedGeneric's second diffuse) is a numbered form of
    // the base parameter rather than of $textureN.
    constexpr std::string_view kBaseTexture = "$basetexture";
    if (lower.size() > kBaseTexture.size() &&
        lower.compare(0, kBaseTexture.size(), kBaseTexture) == 0) {
        bool digits = true;
        for (std::size_t i = kBaseTexture.size(); i < lower.size(); ++i) {
            if (lower[i] < '0' || lower[i] > '9') {
                digits = false;
                break;
            }
        }
        if (digits) {
            return true;
        }
    }

    // Fallback for shaders with no schema: bumpmap, normalmap, specularmap,
    // detail, envmap, dudvmap and friends all name texture files. "$detail" has
    // neither suffix, so it is named explicitly.
    return lower == "$detail" || ends_with_ci(lower, "map") || ends_with_ci(lower, "texture");
}

namespace {

// Turns a raw VMT value into a game-relative "materials/..." texture path, or
// returns false when the value is not a usable texture reference.
bool normalize_texture_reference(std::string_view value, std::string& out) {
    std::string normalized = trim_copy(value);
    if (normalized.empty()) {
        return false;
    }

    for (char& c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }

    const std::string lower = to_lower(normalized);
    // Engine render targets and the map cubemap are not files.
    if (starts_with_ci(normalized, "_rt_") || lower == "env_cubemap") {
        return false;
    }
    // Skip the remaining placeholder textures Source uses for "no texture".
    if (lower.empty() || lower == "none" || lower == "black" || lower == "white") {
        return false;
    }

    if (ends_with_ci(normalized, ".vtf")) {
        normalized.erase(normalized.size() - 4);
    }
    normalized = to_lower(normalized);
    if (normalized.empty()) {
        return false;
    }

    if (!starts_with_ci(normalized, "materials/")) {
        normalized = "materials/" + normalized;
    }
    out = normalized + ".vtf";
    return true;
}

} // namespace

std::vector<TextureReference> texture_references_from_vmt_detailed(
    const std::string& vmt_text, const std::set<std::string>& schema_textures) {
    // Document::parse is lossless and recovering: it never fails, it records
    // diagnostics. A VMT the user is still typing still yields its parameters.
    const vmt::Result<vmt::Document> parsed = vmt::Document::parse(vmt_text);
    if (!parsed) {
        return {};
    }
    const vmt::Document& document = parsed.value();

    // Priority: the base diffuse first, then the numbered $textureN forms, then
    // any other texture parameter. The thumbnail path depends on this order; the
    // info panel shows all of them in the same order for consistency.
    struct Candidate {
        int priority = 2;
        unsigned index = 0;
        TextureReference reference;
    };
    std::vector<Candidate> candidates;
    std::set<std::string> seen;

    for (const vmt::Node* node : document.parameters()) {
        if (node == nullptr || !node->is_parameter()) {
            continue;
        }
        const std::string key(node->key());
        const std::string lower_key = to_lower(key);
        const unsigned numbered = texture_index_of(lower_key);

        const bool declared = schema_textures.find(lower_key) != schema_textures.end();
        if (lower_key != "$basetexture" && numbered == 0 && !declared &&
            !is_texture_parameter_name(lower_key)) {
            continue;
        }

        std::string path;
        if (!normalize_texture_reference(node->value(), path) || !seen.insert(path).second) {
            continue;
        }
        TextureReference reference;
        reference.parameter = key;
        reference.game_path = std::move(path);

        if (lower_key == "$basetexture" ||
            (lower_key.size() > 13 && lower_key.compare(0, 13, "$basetexture") == 0)) {
            candidates.push_back({0, 0, std::move(reference)});
        } else if (numbered != 0) {
            candidates.push_back({1, numbered, std::move(reference)});
        } else {
            candidates.push_back({2, 0, std::move(reference)});
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         if (a.priority != b.priority) {
                             return a.priority < b.priority;
                         }
                         return a.index < b.index;
                     });

    std::vector<TextureReference> result;
    result.reserve(candidates.size());
    for (Candidate& candidate : candidates) {
        result.push_back(std::move(candidate.reference));
    }
    return result;
}

std::vector<std::string> texture_references_from_vmt(const std::string& vmt_text) {
    const std::vector<TextureReference> detailed = texture_references_from_vmt_detailed(vmt_text);
    std::vector<std::string> paths;
    paths.reserve(detailed.size());
    for (const TextureReference& reference : detailed) {
        paths.push_back(reference.game_path);
    }
    return paths;
}

vmt::Result<TextureThumbnail> make_texture_thumbnail(const std::string& material_vmt_text,
                                                     TextureSource& source,
                                                     std::uint32_t max_size) {
    const std::vector<std::string> references = texture_references_from_vmt(material_vmt_text);
    if (references.empty()) {
        return vmt::Err<TextureThumbnail>(vmt::make_error(
            vmt::ErrorCode::NotFound,
            "this material does not reference a texture",
            "Add a $basetexture parameter to preview it as a texture"));
    }

    // Track why each candidate failed so the browser's status line can say what
    // was actually attempted. A material whose base texture is an unsupported
    // HDR is a different situation from one whose texture is simply absent, and
    // the caller uses that distinction to decide whether the engine fallback is
    // worth trying.
    std::string attempts;
    auto note_failure = [&attempts](const std::string& game_path, const vmt::Error& error) {
        if (!attempts.empty()) {
            attempts += "; ";
        }
        attempts += game_path + " (" + error.message + ")";
    };

    vmt::Error last_error = vmt::make_error(
        vmt::ErrorCode::NotFound,
        "none of the material's textures could be read",
        "Check that the texture file exists in the game's materials folder");

    for (const std::string& game_path : references) {
        const vmt::Result<std::vector<unsigned char>> bytes = source.read(game_path);
        if (!bytes) {
            last_error = bytes.error();
            note_failure(game_path, last_error);
            continue;
        }

        const std::vector<unsigned char>& data = bytes.value();
        const vmt::Result<vtf::RgbaImage> decoded = vtf::decode_vtf_preview_from_memory(
            data.data(), data.size(), 0, max_size, std::filesystem::path(game_path));
        if (!decoded) {
            // Remember why, but keep looking: a later $textureN may be a format
            // we can actually show.
            last_error = decoded.error();
            note_failure(game_path, last_error);
            continue;
        }

        const vtf::RgbaImage& image = decoded.value();
        TextureThumbnail thumbnail;
        thumbnail.width = static_cast<int>(image.width);
        thumbnail.height = static_cast<int>(image.height);
        thumbnail.texture_name = game_path;
        thumbnail.bgra.resize(image.pixels.size());

        // vtf::RgbaImage is RGBA with row 0 first; the cache wants BGRA, also
        // top-down, so only the channel order changes.
        for (std::size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
            thumbnail.bgra[i + 0] = image.pixels[i + 2];
            thumbnail.bgra[i + 1] = image.pixels[i + 1];
            thumbnail.bgra[i + 2] = image.pixels[i + 0];
            thumbnail.bgra[i + 3] = image.pixels[i + 3];
            if (image.pixels[i + 3] != 255) {
                thumbnail.has_alpha = true;
            }
        }

        return vmt::Ok(std::move(thumbnail));
    }

    // Every candidate failed. Name them: the browser shows this verbatim, and
    // "not found" alone would hide the difference between a missing file and an
    // undecodable format (which is what makes the engine fallback worth trying).
    last_error.message = "no usable texture for this material: " + attempts;
    return vmt::Err<TextureThumbnail>(std::move(last_error));
}

} // namespace ui