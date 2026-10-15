#include "ui/export_plan.h"

// ui export_plan implementation. See the header for the contract.
//
// Layouts:
//   Flat          - every material lands directly inside the destination
//                   folder; the game path is flattened into the file name
//                   ("models/props_c17/oildrum001" ->
//                   "models_props_c17_oildrum001.vmt").
//   MaterialsTree - the file keeps its game-relative path under a "materials"
//                   folder ("materials/models/props_c17/oildrum001.vmt"), so
//                   the export can be copied straight over a game/mod folder.
//
// Safety rules: the destination must be set, blank material names fail, and a
// material name may never contribute ".." elements or absolute components that
// would place a file outside the destination folder. Two different materials
// that map to the same relative path are reported in ExportPlan::duplicates;
// the first one keeps the slot in ExportPlan::items. Nothing here touches the
// filesystem, which is what makes the planner unit testable.

#include <cctype>
#include <set>
#include <string>
#include <utility>

namespace ui {
namespace {

bool is_blank(const std::string& text) {
    for (const char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
    }
    return true;
}

bool is_safe_flat_char(const char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
}

vmt::Error invalid_argument_error(std::string message, std::string remediation) {
    return vmt::make_error(vmt::ErrorCode::InvalidArgument, std::move(message),
                           std::move(remediation));
}

// A material may describe a nested game path ("models/props_c17/..."), but it
// must not carry ".." elements or an absolute component: those are the only
// ways a material name could reach outside the destination folder.
bool is_relative_game_path(const std::string& material) {
    const std::filesystem::path path(material);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const std::filesystem::path& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

// Final guard: the produced relative path must be relative and free of "..".
bool stays_inside_parent(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) {
        return false;
    }
    for (const std::filesystem::path& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

} // namespace

std::string flat_material_file_name(const std::string& material) {
    std::string stem;
    stem.reserve(material.size());
    bool has_readable_char = false;
    for (const char ch : material) {
        if (is_safe_flat_char(ch)) {
            stem.push_back(ch);
            if (ch != '_') {
                has_readable_char = true;
            }
        } else {
            stem.push_back('_');
        }
    }
    if (!has_readable_char) {
        // Blank names and names made only of underscores ("///") leave nothing
        // to name the file after.
        return {};
    }
    stem += ".vmt";
    return stem;
}

vmt::Result<ExportPlan> plan_material_export(const std::vector<std::string>& materials,
                                             const ExportOptions& options) {
    if (options.destination.empty()) {
        return vmt::Err<ExportPlan>(invalid_argument_error(
            "no destination folder", "Choose a folder for the exported materials"));
    }

    ExportPlan plan;
    std::set<std::string> handled; // exact material names already planned
    std::set<std::string> taken;   // relative paths already planned

    for (const std::string& material : materials) {
        if (is_blank(material)) {
            return vmt::Err<ExportPlan>(invalid_argument_error(
                "empty material name", "Fix the material list and try again"));
        }
        // The same material listed twice is exported once, without noise.
        if (!handled.insert(material).second) {
            continue;
        }

        ExportItem item;
        item.material = material;
        if (options.layout == ExportLayout::Flat) {
            item.file_name = flat_material_file_name(material);
            if (item.file_name.empty()) {
                return vmt::Err<ExportPlan>(invalid_argument_error(
                    "material \"" + material + "\" cannot produce a safe file name",
                    "Rename the material and try again"));
            }
            item.relative = std::filesystem::path(item.file_name);
        } else {
            if (!is_relative_game_path(material)) {
                return vmt::Err<ExportPlan>(invalid_argument_error(
                    "material \"" + material + "\" would write outside the destination folder",
                    "Remove '..' and absolute path components from the material name"));
            }
            std::string raw = "materials/";
            raw += material;
            raw += ".vmt";
            // lexically_normal() also turns '/' into the platform separator.
            item.relative = std::filesystem::path(raw).lexically_normal();
            item.file_name = item.relative.filename().string();
        }

        if (!stays_inside_parent(item.relative) || item.file_name.empty()) {
            return vmt::Err<ExportPlan>(invalid_argument_error(
                "material \"" + material + "\" would write outside the destination folder",
                "Remove '..' and absolute path components from the material name"));
        }

        const std::string key = item.relative.generic_string();
        if (!taken.insert(key).second) {
            plan.duplicates.push_back(material);
            continue;
        }

        item.absolute = options.destination / item.relative;
        plan.items.push_back(std::move(item));
    }

    return vmt::Ok(std::move(plan));
}

} // namespace ui
