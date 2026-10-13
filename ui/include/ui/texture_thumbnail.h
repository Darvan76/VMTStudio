#pragma once

// ui texture_thumbnail - builds Asset Browser thumbnails by decoding the
// material's texture with the in-process VTF decoder.
//
// Why this exists: the engine bridge can only produce thumbnails by asking the
// game's own material system to render and read back a render target. That
// requires a working shaderapidx9, which is not available for every game build
// (Garry's Mod and Portal 2 ship newer interface versions than the SDK 2013
// headers this project compiles against), and the backbuffer readback returns
// all zeros even when init succeeds. Decoding the VTF ourselves sidesteps both
// problems: textures render from the actual file bytes, with no engine at all.
//
// The pipeline is: VMT text -> candidate texture names -> .vtf bytes ->
// vtf::decode_vtf_preview_from_memory -> RGBA -> BGRA -> BMP cache file.
//
// Design notes:
//   - Windows-free and engine-free, so it is unit-tested directly.
//   - `TextureSource` is injected by the caller, which is how disk-backed and
//     VPK-backed lookups share this code.
//   - A material may reference several textures ($basetexture, $texture2, ...);
//     the first one that resolves and decodes wins, so a material whose base
//     texture is missing still previews its detail map.

#include "vmt/result.h"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

// Where a texture's bytes come from. Injected so this module stays
// engine-free and testable.
class TextureSource {
public:
    virtual ~TextureSource() = default;

    // Reads a game-relative path such as "materials/brick/brick05.vtf".
    // Returns the raw bytes, or an error (NotFound when it does not exist).
    virtual vmt::Result<std::vector<unsigned char>> read(const std::string& game_path) = 0;
};

// A decoded thumbnail, ready to be written to the BMP cache.
struct TextureThumbnail {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bgra; // row 0 is the top row, BGRA order
    std::string texture_name;         // game-relative .vtf that produced this
    bool has_alpha = false;
};

// One texture a VMT references, kept together with the parameter that named it
// so the browser can show "$basetexture" next to the file it resolved to.
struct TextureReference {
    std::string parameter; // "$basetexture", "$texture2", ...
    std::string game_path; // "materials/brick/brick05.vtf"
};

// True when a parameter name is worth treating as a texture path. Recognised:
//   - $basetexture / $basetextureN / $textureN
//   - names the shader schema declares as "texture" ($bumpmap, $normalmap,
//     $detail, $envmap, $dudvmap, ...) when passed in via `schema_textures`
//   - a name ending in "map", "texture" or "textureN" as a fallback for
//     shaders with no schema (bumpmap, normalmap, specularmap, ...)
// Deliberately not recognised: $surfaceprop, $color and other non-file values,
// which `normalize_texture_reference` then filters by content anyway.
bool is_texture_parameter_name(std::string_view key);

// Extracts the texture references a VMT declares, in a stable order that puts
// the base texture first ($basetexture, then $texture2..$textureN, then any
// remaining texture parameter). Names are normalized the way the engine
// resolves them: backslashes to forward slashes, ".vtf" stripped, lowercased,
// and prefixed with "materials/" only if not already prefixed.
//
// `schema_textures` are the parameter names the shader schema marks as
// "texture"; pass an empty set to rely on the name heuristic alone.
//
// Skips _rt_ render targets and the "env_cubemap" special value, matching the
// validation rules.
std::vector<TextureReference> texture_references_from_vmt_detailed(
    const std::string& vmt_text,
    const std::set<std::string>& schema_textures = {});

// Convenience wrapper returning just the resolved paths.
std::vector<std::string> texture_references_from_vmt(const std::string& vmt_text);

// Decodes the first resolvable texture of a material into a thumbnail.
//
// `max_size` caps the longest edge; 0 keeps the decoded size.
//
// Errors: NotFound when no referenced texture could be read or decoded. The
// message names every texture that was attempted and why it failed, so the
// caller can tell a missing file from an undecodable format and decide whether
// the engine fallback is worth attempting. See the browser's status line for
// the intended presentation.
vmt::Result<TextureThumbnail> make_texture_thumbnail(const std::string& material_vmt_text,
                                                     TextureSource& source,
                                                     std::uint32_t max_size);

} // namespace ui