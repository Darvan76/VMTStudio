#include "ui/shader_catalog.h"

#include <algorithm>
#include <cstddef>

namespace ui {
namespace {

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lower_copy(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        c = ascii_lower(c);
    }
    return result;
}

// Inserts `name` unless something matching it case-insensitively is already in
// `out`. Returns true when the name was added.
bool insert_unique(std::vector<std::string>& out, std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const std::string lowered = lower_copy(name);
    for (const std::string& existing : out) {
        if (lower_copy(existing) == lowered) {
            return false;
        }
    }
    out.emplace_back(name);
    return true;
}

// Sort key: lowercased copy, so "unlitgeneric" and "VertexLitGeneric" group
// together and the list reads case-insensitively.
void sort_names(std::vector<std::string>& names) {
    std::sort(names.begin(), names.end(),
              [](const std::string& a, const std::string& b) {
                  return lower_copy(a) < lower_copy(b);
              });
}

// The catalog is a sorted list of names, not parameter knowledge: see
// data/schema for that. Everything here is a shader name that appears in the
// first line of shipped VMT files. Following the certainty policy of
// data/schema/README.md, only names that actually exist are listed - a shader
// missing from this list is still writable through the text editor, and the
// engine reports unknown shaders itself.
const std::vector<std::string> kCatalog = [] {
    std::vector<std::string> names = {
        // Unlit textures
        "UnlitGeneric",
        "UnlitGenericDetail",
        "UnlitGenericLightmap",
        "UnlitGradient",
        "UnlitTwoTexture",
        "UnlitVerticalGradient",

        // Lit textures
        "LightmappedGeneric",
        "Overgrown",
        "VertexColorMaterial",
        "VertexLitGeneric",
        "VertexLitGenericDetail",
        "VertexLitGenericDetailAlphaBlend",
        "WorldTwoTextureBlend",
        "WorldVertexLightmap",
        "WorldVertexTransition",

        // Water and other liquids
        "Lava",
        "Slime",
        "Water",
        "WaterRefract",
        "WaterReflect",

        // Environment and sky
        "BlackSky",
        "Clouds",
        "CubeSky",
        "Earth",
        "Fire",
        "Mirror",
        "Portal",
        "Reflect",
        "Refract",
        "Sky",
        "Sky2",
        "Sprite",

        // Effects and special-purpose surfaces
        "AnimatedTexture",
        "DeferredDecal",
        "DissolveFilter",
        "DissolveProxy",
        "Laser",
        "LaserBeam",
        "Lightmap",
        "NearestSnapTex",
        "PointEntity",
        "ScreenspaceRefraction",
        "SignedDistanceField",

        // Source 2 physically based
        "PbrModel",
        "PbrPanoramaSkybox",
        "PbrSimpleMaterial",
        "PbrSkybox",

        // Source 2 post process chain
        "PostProcess",
        "PostProcessBloom",
        "PostProcessColorCorrection",
        "PostProcessDepthOfField",
        "PostProcessFXAA",
        "PostProcessFilmGrain",
        "PostProcessNearBlur",
        "PostProcessSharpen",
        "PostProcessTonemap",
    };
    std::vector<std::string> unique;
    for (const std::string& name : names) {
        insert_unique(unique, name);
    }
    sort_names(unique);
    return unique;
}();

} // namespace

bool shader_name_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

const std::vector<std::string>& known_shader_names() {
    return kCatalog;
}

std::vector<std::string> shader_picker_names(const vmt::SchemaLibrary* schemas) {
    std::vector<std::string> names = kCatalog;
    if (schemas != nullptr) {
        for (const std::string& name : schemas->shader_names()) {
            insert_unique(names, name);
        }
    }
    sort_names(names);
    return names;
}

int find_shader_picker_index(const std::vector<std::string>& names, std::string_view name) {
    if (name.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (shader_name_equal(names[i], name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool is_brush_only_shader(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower.push_back(ascii_lower(c));
    }

    // World / Hammer brush shaders
    if (lower.rfind("lightmapped", 0) == 0) return true;
    if (lower.rfind("world", 0) == 0) return true;
    if (lower.rfind("water", 0) == 0) return true;
    if (lower == "lightmap") return true;
    if (lower == "unlitgenericlightmap") return true;
    if (lower == "multiblend") return true;
    if (lower == "shatteredglass") return true;
    if (lower == "volumefog") return true;
    if (lower == "lava" || lower == "slime") return true;

    return false;
}

} // namespace ui