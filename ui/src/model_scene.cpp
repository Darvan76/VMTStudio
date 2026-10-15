#include "ui/model_scene.h"

// Scaffold: presentation logic lands with the model materials dock work.

namespace ui {

const char* const kUnusedMaterialGroupName = "Unused";

std::vector<ModelMaterialGroup> build_material_groups(const bridge::ModelScene&) {
    return {};
}

std::vector<const ModelMaterialRow*> flatten_material_rows(
    const std::vector<ModelMaterialGroup>&) {
    return {};
}

std::string material_display_name(const std::string& game_material_name) {
    return game_material_name;
}

} // namespace ui
