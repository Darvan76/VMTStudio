#pragma once

// ui shader catalog - the list of shader names the shader dropdown offers.
//
// A VMT's shader is just a name token before the opening brace, so the viewer
// can write any of them. This catalog is therefore *not* the same thing as the
// parameter schema in data/schema: the schema says which parameters a shader
// accepts (and only for the shaders somebody has documented), while the catalog
// only says which names are worth offering. Selecting a shader with no schema is
// perfectly legal - it simply yields a panel with no described parameters.
//
// Windows-free and unit tested.

#include "vmt/schema.h"

#include <string>
#include <string_view>
#include <vector>

namespace ui {

// Case-insensitive comparison of two shader names. The engine resolves shader
// names case-insensitively, so the dropdown must too.
bool shader_name_equal(std::string_view a, std::string_view b) noexcept;

// The built-in catalog, sorted case-insensitively and free of duplicates.
// Source / Source 2 shader names that actually appear in VMT files.
const std::vector<std::string>& known_shader_names();

// The list shown in the dropdown: known_shader_names() plus every shader the
// loaded schema library describes that the catalog does not already contain
// (so adding a data/schema/*.json file is enough to make its shader pickable).
// `schemas` may be null. Always sorted and free of duplicates.
std::vector<std::string> shader_picker_names(const vmt::SchemaLibrary* schemas);

// Index of `name` inside `names`, compared case-insensitively; -1 when absent.
int find_shader_picker_index(const std::vector<std::string>& names, std::string_view name);

// Returns true if the shader name is exclusive to Hammer / world brushes
// (e.g. LightmappedGeneric, WorldVertexTransition, WorldTwoTextureBlend, Lightmap, etc.).
bool is_brush_only_shader(std::string_view name) noexcept;

} // namespace ui