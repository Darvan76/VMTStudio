#pragma once

// ui material_info - everything the Asset Browser can tell the user about a
// selected material: the VMT itself (shader, parameters, parse state) and each
// referenced texture (format, dimensions, mip chain, on-disk size).
//
// Why it exists separately from texture_thumbnail: the thumbnail pipeline only
// cares whether the *first* usable texture decodes, so it stops early and
// discards metadata. The browser needs the full picture for every texture a
// material references, including the ones that failed to decode, because that
// is exactly the information that makes a missing or wrong-format texture
// diagnosable.
//
// Windows-free and engine-free: `TextureSource` is injected, so VPK-backed
// lookups and disk-backed lookups share this code and the unit tests need no
// game install.

#include "ui/texture_thumbnail.h"
#include "vmt/result.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace ui {

// One referenced texture and what could be learned about it.
struct TextureInfo {
    std::string parameter;  // "$basetexture", "$texture2", ...
    std::string game_path;  // "materials/brick/brick05.vtf"

    bool found = false;     // the file resolved
    bool decoded = false;   // and its format can be shown as 8-bit RGBA
    std::string note;       // why it was not found / not decoded

    // VTF metadata. Only meaningful when `found`.
    std::string format_name;
    std::uint32_t format_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::uint32_t mip_count = 0;
    std::uint32_t frame_count = 0;
    std::uint32_t face_count = 0;
    std::uint32_t version_major = 0;
    std::uint32_t version_minor = 0;
    bool has_alpha = false;
    bool has_mipmaps = false;
    bool is_cubemap = false;
    bool is_normal_map = false;
    bool is_hdr = false;

    // Uncompressed footprint of mip 0 in bytes (width * height * 4), which is
    // what the texture costs once it reaches VRAM. `file_size` is what it costs
    // on disk, which for a DXT5 texture is about a quarter of that.
    std::uint64_t file_size = 0;
    std::uint64_t vram_size = 0;
};

// The material and its textures.
struct MaterialInfo {
    std::string name;          // game-relative, no extension
    std::string shader;
    bool shader_known = false; // a schema covers it
    std::size_t parameter_count = 0;
    std::size_t diagnostic_count = 0;

    std::vector<std::string> parameter_keys; // body-level, in file order
    std::vector<TextureInfo> textures;

    std::uint64_t vram_total = 0; // sum over the textures that resolved
    bool any_texture_missing = false;
    bool any_texture_undecodable = false;
};

// Collects the info for one material. Never fails: a material with no textures,
// an unreadable texture or a broken VMT still yields whatever is known, with
// the gaps recorded in the corresponding `note`.
//
// `shader_known` reflects whether a schema covers the material's shader, and
// `schema_textures` are that schema's parameters declared as "texture". The
// schema library lives in the Win32 layer (it loads data/schema at runtime), so
// the caller passes these in rather than this module taking a dependency.
MaterialInfo collect_material_info(const std::string& material_vmt_text,
                                   TextureSource& source,
                                   bool shader_known = false,
                                   const std::set<std::string>& schema_textures = {});

// Human-readable, multi-line summary for the browser's info panel. UTF-8.
std::string format_material_info(const MaterialInfo& info);

// Byte counts as "1.2 MB" / "834 KB" / "312 B".
std::string format_bytes(std::uint64_t bytes);

} // namespace ui