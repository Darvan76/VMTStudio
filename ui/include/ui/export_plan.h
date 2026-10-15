#pragma once

// ui export_plan - file plan for "export every material of the model to one
// folder". Windows-free and unit testable: it only computes paths, names and
// conflicts; reading the .vmt text through the engine and writing the files is
// the caller's job.

#include "vmt/result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ui {

enum class ExportLayout {
    Flat,           // every file directly inside the destination folder
    MaterialsTree,  // materials/<game path>/<name>.vmt, ready to copy into a game
};

struct ExportOptions {
    ExportLayout layout = ExportLayout::Flat;
    std::filesystem::path destination;
};

// One file to write.
struct ExportItem {
    std::string material;           // game-relative name, no extension
    std::string file_name;          // base name with extension ("a_b_c.vmt")
    std::filesystem::path relative; // path under `destination`
    std::filesystem::path absolute; // destination / relative
};

struct ExportPlan {
    std::vector<ExportItem> items;
    std::vector<std::string> duplicates; // materials that mapped to the same file
};

// Flat layout: "models_props_c17_oildrum001.vmt" in the destination root.
// MaterialsTree layout: "materials/models/props_c17/oildrum001.vmt".
// Fails with InvalidArgument when the destination is empty or a material name
// cannot produce a safe file name.
vmt::Result<ExportPlan> plan_material_export(const std::vector<std::string>& materials,
                                             const ExportOptions& options);

// "models_props_c17_oildrum001.vmt" for "models/props_c17/oildrum001":
// separators and any character outside [A-Za-z0-9._-] become '_'.
std::string flat_material_file_name(const std::string& material);

} // namespace ui
