#pragma once

// ui model_scene - presentation model for the materials of the loaded model.
// Windows-free and unit testable: the Qt dock renders these rows and the engine
// query lives in bridge::EngineView.
//
// Rows keep the engine order of the model's texture table, which is the order
// the artist sees in the QC file, grouped by the bodypart that uses them.

#include "bridge/engine_view.h"

#include <string>
#include <vector>

namespace ui {

// One material of the model.
struct ModelMaterialRow {
    int texture_index = -1;
    std::string name;         // game-relative, no extension
    std::string display_name; // last path segment, for the tree ("oildrum001")
    std::string shader;       // empty when unresolved
    std::string used_by;      // "bodypart/submodel" list, comma separated; empty when unknown
    bool shared = false;      // referenced by more than one submodel
};

// Materials grouped under the bodypart that uses them; materials the engine
// could not attribute to a bodypart land in the "Unused" group.
struct ModelMaterialGroup {
    std::string name;
    std::vector<ModelMaterialRow> rows;
};

// Name of the synthetic group for materials without a known bodypart.
extern const char* const kUnusedMaterialGroupName;

// Builds the groups from a model scene dump (engine order, bodypart order).
std::vector<ModelMaterialGroup> build_material_groups(const bridge::ModelScene& scene);

// Flattens the groups in walk order (the order the "next/previous material"
// buttons follow).
std::vector<const ModelMaterialRow*> flatten_material_rows(
    const std::vector<ModelMaterialGroup>& groups);

// "oildrum001" from "models/props_c17/oildrum001"; the whole name when it has
// no slashes.
std::string material_display_name(const std::string& game_material_name);

} // namespace ui
