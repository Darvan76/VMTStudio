#include "ui/model_scene.h"

// Presentation model behind the model materials dock: turns the raw model
// scene dump into the material rows grouped by bodypart, provides the flat
// walk-through order of the "next/previous material" buttons and the short
// display names shown in the tree. Kept free of Windows and Qt headers so the
// whole module is unit tested by model_view_tests.

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ui {

namespace {

// One distinct bodypart/submodel pair that references a material. The dump may
// repeat a pair; only distinct pairs count for `used_by` and `shared`.
struct MaterialUseSite {
    int bodypart = -1;
    int submodel = -1;
};

bool use_site_less(const MaterialUseSite& left, const MaterialUseSite& right) {
    if (left.bodypart != right.bodypart) {
        return left.bodypart < right.bodypart;
    }
    return left.submodel < right.submodel;
}

bool use_site_equal(const MaterialUseSite& left, const MaterialUseSite& right) {
    return left.bodypart == right.bodypart && left.submodel == right.submodel;
}

// Name shown for a bodypart: the bodyparts table is the canonical list the
// rest of the UI shows, so it wins over the copy each use row carries. The
// numeric index is the last resort so a group always has a label.
std::string bodypart_display_name(const bridge::ModelScene& scene, int bodypart) {
    for (const bridge::ModelBodypart& entry : scene.bodyparts) {
        if (entry.index == bodypart && !entry.name.empty()) {
            return entry.name;
        }
    }
    for (const bridge::ModelMaterialUse& use : scene.material_uses) {
        if (use.bodypart == bodypart && !use.bodypart_name.empty()) {
            return use.bodypart_name;
        }
    }
    return std::to_string(bodypart);
}

} // namespace

const char* const kUnusedMaterialGroupName = "Unused";

std::vector<ModelMaterialGroup> build_material_groups(const bridge::ModelScene& scene) {
    // Uses that point at a texture index the material table does not contain
    // name no material, so they are ignored everywhere below. A texture index
    // listed twice keeps its first table position.
    std::unordered_map<int, std::size_t> material_positions;
    material_positions.reserve(scene.materials.size());
    for (std::size_t position = 0; position < scene.materials.size(); ++position) {
        material_positions.emplace(scene.materials[position].texture_index, position);
    }

    // Distinct bodypart indices that reference a known material, ascending:
    // the order the groups are listed in.
    std::vector<int> bodypart_order;
    for (const bridge::ModelMaterialUse& use : scene.material_uses) {
        if (material_positions.find(use.texture_index) == material_positions.end()) {
            continue;
        }
        if (std::find(bodypart_order.begin(), bodypart_order.end(), use.bodypart) ==
            bodypart_order.end()) {
            bodypart_order.push_back(use.bodypart);
        }
    }
    std::sort(bodypart_order.begin(), bodypart_order.end());

    // Distinct use sites per material, sorted by bodypart and submodel so
    // `used_by` never depends on the order of the dump rows themselves.
    std::vector<std::vector<MaterialUseSite>> sites_by_material(scene.materials.size());
    for (const bridge::ModelMaterialUse& use : scene.material_uses) {
        const auto found = material_positions.find(use.texture_index);
        if (found == material_positions.end()) {
            continue;
        }
        sites_by_material[found->second].push_back(MaterialUseSite{use.bodypart, use.submodel});
    }
    for (std::vector<MaterialUseSite>& sites : sites_by_material) {
        std::sort(sites.begin(), sites.end(), use_site_less);
        sites.erase(std::unique(sites.begin(), sites.end(), use_site_equal), sites.end());
    }

    const auto make_row = [&scene, &sites_by_material](std::size_t position) {
        const bridge::ModelMaterial& material = scene.materials[position];
        const std::vector<MaterialUseSite>& sites = sites_by_material[position];

        ModelMaterialRow row;
        row.texture_index = material.texture_index;
        row.name = material.name;
        row.display_name = material_display_name(material.name);
        row.shader = material.shader;
        for (const MaterialUseSite& site : sites) {
            if (!row.used_by.empty()) {
                row.used_by += ", ";
            }
            row.used_by += bodypart_display_name(scene, site.bodypart);
            row.used_by += '/';
            row.used_by += std::to_string(site.submodel);
        }
        row.shared = sites.size() > 1;
        return row;
    };

    // One group per distinct bodypart that uses materials, in bodypart index
    // order; rows keep the engine texture-table order and a material used by
    // several bodyparts shows up once in each of them.
    std::vector<ModelMaterialGroup> groups;
    groups.reserve(bodypart_order.size() + 1);
    for (const int bodypart : bodypart_order) {
        ModelMaterialGroup group;
        group.name = bodypart_display_name(scene, bodypart);
        for (std::size_t position = 0; position < scene.materials.size(); ++position) {
            const std::vector<MaterialUseSite>& sites = sites_by_material[position];
            const bool used_here =
                std::any_of(sites.begin(), sites.end(), [bodypart](const MaterialUseSite& site) {
                    return site.bodypart == bodypart;
                });
            if (used_here) {
                group.rows.push_back(make_row(position));
            }
        }
        groups.push_back(std::move(group));
    }

    // Materials no bodypart references land in one synthetic group at the
    // end; the group is left out entirely when every material is attributed.
    ModelMaterialGroup unused_group;
    unused_group.name = kUnusedMaterialGroupName;
    for (std::size_t position = 0; position < scene.materials.size(); ++position) {
        if (sites_by_material[position].empty()) {
            unused_group.rows.push_back(make_row(position));
        }
    }
    if (!unused_group.rows.empty()) {
        groups.push_back(std::move(unused_group));
    }

    return groups;
}

std::vector<const ModelMaterialRow*> flatten_material_rows(
    const std::vector<ModelMaterialGroup>& groups) {
    // Walk order: the rows of the first group, then the rows of the second,
    // and so on. Wrapping around at the ends is left to the widget that drives
    // the next/previous buttons.
    std::size_t total = 0;
    for (const ModelMaterialGroup& group : groups) {
        total += group.rows.size();
    }

    std::vector<const ModelMaterialRow*> rows;
    rows.reserve(total);
    for (const ModelMaterialGroup& group : groups) {
        for (const ModelMaterialRow& row : group.rows) {
            rows.push_back(&row);
        }
    }
    return rows;
}

std::string material_display_name(const std::string& game_material_name) {
    // Everything after the final slash; a trailing slash leaves an empty last
    // segment, so the last non-empty one is taken. A name without slashes is
    // already its own display name.
    std::size_t end = game_material_name.size();
    while (end > 0 && game_material_name[end - 1] == '/') {
        --end;
    }
    if (end == 0) {
        return std::string();
    }
    const std::size_t slash = game_material_name.rfind('/', end - 1);
    const std::size_t start = (slash == std::string::npos) ? 0 : slash + 1;
    return game_material_name.substr(start, end - start);
}

} // namespace ui
