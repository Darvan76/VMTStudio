#include "ui/material_info.h"

#include "vmt/document.h"
#include "vtf/vtf_reader.h"

#include <algorithm>
#include <cctype>

namespace ui {
namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// VRAM footprint of mip 0: every decodable format reaches the GPU as 8-bit
// RGBA, so this is what the texture actually costs once uploaded.
std::uint64_t vram_footprint(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(width) * height * 4u;
}

} // namespace

std::string format_bytes(std::uint64_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }
    if (bytes < 1024ull * 1024) {
        // One decimal keeps a 834 KB texture readable without being noisy.
        const double kb = static_cast<double>(bytes) / 1024.0;
        const int rounded = static_cast<int>(kb + 0.5);
        return std::to_string(rounded) + " KB";
    }
    // One decimal below 10 MB, none above: "1.5 MB" reads better than "2 MB" for
    // a texture that matters, while "128 MB" does not need ".0".
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 10.0) {
        return std::to_string(static_cast<long long>(mb + 0.5)) + " MB";
    }
    const long long tenths = static_cast<long long>(mb * 10.0 + 0.5);
    const std::string text = std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
    return text + " MB";
}

MaterialInfo collect_material_info(const std::string& material_vmt_text,
                                   TextureSource& source,
                                   bool shader_known,
                                   const std::set<std::string>& schema_textures) {
    MaterialInfo info;
    info.shader_known = shader_known;

    const vmt::Result<vmt::Document> parsed = vmt::Document::parse(material_vmt_text);
    if (!parsed) {
        info.shader = "(the material could not be parsed)";
        return info;
    }

    const vmt::Document& document = parsed.value();
    info.shader = std::string(document.shader());
    if (info.shader.empty()) {
        info.shader = "(none)";
    }
    info.diagnostic_count = document.diagnostics().size();

    for (const vmt::Node* node : document.parameters()) {
        if (node == nullptr || !node->is_parameter()) {
            continue;
        }
        info.parameter_keys.emplace_back(node->key());
    }
    info.parameter_count = info.parameter_keys.size();

    // Every referenced texture is inspected, not just the first usable one: the
    // panel's job is to explain the whole material.
    for (const TextureReference& reference :
         texture_references_from_vmt_detailed(material_vmt_text, schema_textures)) {
        TextureInfo texture;
        texture.parameter = reference.parameter;
        texture.game_path = reference.game_path;

        const vmt::Result<std::vector<unsigned char>> bytes = source.read(reference.game_path);
        if (!bytes) {
            texture.note = bytes.error().message;
            info.any_texture_missing = true;
            info.textures.push_back(std::move(texture));
            continue;
        }

        const std::vector<unsigned char>& data = bytes.value();
        texture.found = true;
        texture.file_size = data.size();

        const vmt::Result<vtf::VtfInfo> meta =
            vtf::open_vtf_info_from_memory(data.data(), data.size(), reference.game_path);
        if (!meta) {
            texture.note = meta.error().message;
            info.any_texture_undecodable = true;
            info.textures.push_back(std::move(texture));
            continue;
        }

        const vtf::VtfInfo& m = meta.value();
        texture.decoded = m.preview_supported;
        texture.format_name = m.format_name;
        texture.format_id = m.format_id;
        texture.width = m.width;
        texture.height = m.height;
        texture.depth = m.depth;
        texture.mip_count = m.mip_count;
        texture.frame_count = m.frame_count;
        texture.face_count = m.face_count;
        texture.version_major = m.version_major;
        texture.version_minor = m.version_minor;
        texture.has_alpha = m.has_alpha;
        texture.has_mipmaps = m.has_mipmaps;
        texture.is_cubemap = m.is_cubemap;
        texture.is_normal_map = m.is_normal_map;
        texture.is_hdr = m.is_hdr;
        texture.vram_size = vram_footprint(m.width, m.height);
        info.vram_total += texture.vram_size;

        if (!m.preview_supported) {
            // HDR and depth/stencil reach this branch: the file is real and its
            // metadata is good, but it cannot be shown as 8-bit RGBA.
            texture.note = m.is_hdr ? "HDR: no 8-bit preview"
                                    : "this format has no 8-bit preview";
            info.any_texture_undecodable = true;
        }
        info.textures.push_back(std::move(texture));
    }

    return info;
}

std::string format_material_info(const MaterialInfo& info) {
    std::string out;
    const auto line = [&out](const std::string& text) {
        out += text;
        out += '\n';
    };

    line("Shader: " + info.shader + (info.shader_known ? "" : "  (no schema)"));
    line("Parameters: " + std::to_string(info.parameter_count));
    if (info.diagnostic_count != 0) {
        line("Parse problems: " + std::to_string(info.diagnostic_count));
    }

    if (info.textures.empty()) {
        line("");
        line("Textures: none referenced");
        return out;
    }

    line("Textures: " + std::to_string(info.textures.size()));
    for (const TextureInfo& texture : info.textures) {
        line("");
        line("  " + texture.parameter + " -> " + texture.game_path);

        if (!texture.found) {
            line("    missing: " + texture.note);
            continue;
        }

        std::string dimensions = std::to_string(texture.width) + "x" +
                                 std::to_string(texture.height);
        if (texture.face_count == 6 || texture.face_count == 7) {
            dimensions += " cubemap (" + std::to_string(texture.face_count) + " faces)";
        }
        if (texture.depth > 1) {
            dimensions += ", volume depth " + std::to_string(texture.depth);
        }
        line("    Size: " + dimensions + "   file " + format_bytes(texture.file_size) +
             "   vram " + format_bytes(texture.vram_size));

        std::string format = texture.format_name;
        if (texture.format_id != 0 || !texture.format_name.empty()) {
            format += " (id " + std::to_string(texture.format_id) + ")";
        }
        line("    Format: " + format + "   VTF " + std::to_string(texture.version_major) +
             "." + std::to_string(texture.version_minor));

        std::string features = texture.has_alpha ? "alpha" : "no alpha";
        features += texture.has_mipmaps ? ", " + std::to_string(texture.mip_count) + " mips"
                                       : ", no mips";
        if (texture.frame_count > 1) {
            features += ", " + std::to_string(texture.frame_count) + " frames";
        }
        if (texture.is_cubemap) {
            features += ", cubemap";
        }
        if (texture.is_normal_map) {
            features += ", normal map";
        }
        if (texture.is_hdr) {
            features += ", HDR";
        }
        line("    " + features);

        if (!texture.decoded) {
            line("    " + texture.note);
        }
    }

    if (info.vram_total != 0) {
        line("");
        line("Total VRAM (top mip): " + format_bytes(info.vram_total));
    }
    return out;
}

} // namespace ui