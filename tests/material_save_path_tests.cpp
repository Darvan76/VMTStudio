#include "ut.h"

#include "ui/export_plan.h"
#include "ui/material_browser.h"
#include "ui/session.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {

ui::ExportOptions game_destination() {
    ui::ExportOptions options;
    options.layout = ui::ExportLayout::MaterialsTree;
    options.destination = std::filesystem::path("selected_game");
    return options;
}

} // namespace

UT_TEST_CASE("save to game: a loose model material retains its full materials hierarchy") {
    const std::filesystem::path source("addon with spaces/materials/models/props_c17/metal.vmt");
    const auto plan = ui::plan_material_export({ui::vtf_texture_path(source)}, game_destination());
    UT_CHECK(plan.ok());
    if (!plan) return;
    UT_CHECK_EQ(plan.value().items.size(), std::size_t{1});
    if (plan.value().items.size() != 1) return;

    const ui::ExportItem& item = plan.value().items.front();
    UT_CHECK_EQ(item.material, std::string("models/props_c17/metal"));
    UT_CHECK_EQ(item.absolute.generic_string(),
                std::string("selected_game/materials/models/props_c17/metal.vmt"));
    UT_CHECK(plan.value().duplicates.empty());
}

UT_TEST_CASE("save to game: loose and VPK versions of a material select the same override") {
    const std::string loose_name = ui::vtf_texture_path(
        std::filesystem::path("addons/example/materials/models/props_c17/metal.vmt"));
    const std::string packed_name = ui::material_name_from_game_path(
        "Materials\\models\\props_c17\\metal.VMT");

    const auto loose = ui::plan_material_export({loose_name}, game_destination());
    const auto packed = ui::plan_material_export({packed_name}, game_destination());
    UT_CHECK(loose.ok());
    UT_CHECK(packed.ok());
    if (!loose || !packed) return;
    UT_CHECK_EQ(loose.value().items.size(), std::size_t{1});
    UT_CHECK_EQ(packed.value().items.size(), std::size_t{1});
    if (loose.value().items.size() != 1 || packed.value().items.size() != 1) return;

    UT_CHECK_EQ(packed.value().items.front().absolute, loose.value().items.front().absolute);
    UT_CHECK_EQ(packed.value().items.front().relative.generic_string(),
                std::string("materials/models/props_c17/metal.vmt"));
}

UT_TEST_CASE("save to game: a VTF companion becomes a VMT at the texture's original path") {
    const std::string loose_name = ui::vtf_texture_path(
        std::filesystem::path("addons/example/materials/models/weapons/rifle/body.VTF"));
    const std::string packed_name = ui::material_name_from_game_path(
        "materials/models/weapons/rifle/body.vtf");

    // Both inputs describe the same new companion material, not body.vtf.vmt
    // and not a flattened materials/body.vmt.
    const auto plan = ui::plan_material_export({loose_name, packed_name}, game_destination());
    UT_CHECK(plan.ok());
    if (!plan) return;
    UT_CHECK_EQ(plan.value().items.size(), std::size_t{1});
    if (plan.value().items.size() != 1) return;
    UT_CHECK_EQ(plan.value().items.front().absolute.generic_string(),
                std::string("selected_game/materials/models/weapons/rifle/body.vmt"));
    UT_CHECK(plan.value().duplicates.empty());
}

UT_TEST_CASE("save to game: equal basenames in different folders remain separate materials") {
    const std::string prop = ui::vtf_texture_path(
        std::filesystem::path("addons/example/materials/models/props/metal.vmt"));
    const std::string weapon = ui::material_name_from_game_path(
        "materials/models/weapons/metal.vmt");

    const auto plan = ui::plan_material_export({prop, weapon}, game_destination());
    UT_CHECK(plan.ok());
    if (!plan) return;
    UT_CHECK_EQ(plan.value().items.size(), std::size_t{2});
    UT_CHECK(plan.value().duplicates.empty());
    if (plan.value().items.size() != 2) return;

    UT_CHECK_EQ(plan.value().items[0].absolute.generic_string(),
                std::string("selected_game/materials/models/props/metal.vmt"));
    UT_CHECK_EQ(plan.value().items[1].absolute.generic_string(),
                std::string("selected_game/materials/models/weapons/metal.vmt"));
    UT_CHECK(plan.value().items[0].absolute != plan.value().items[1].absolute);
}

UT_TEST_CASE("save to game: virtual path normalization cannot hide traversal or absolute names") {
    std::vector<std::string> unsafe = {
        "materials/../outside.vmt",
        "materials/models/../../outside.vmt",
        "materials\\models\\..\\outside.vmt",
        "/materials/models/absolute.vmt",
        "\\materials\\models\\absolute.vmt",
    };
#ifdef _WIN32
    unsafe.push_back("C:/outside/absolute.vmt");
    unsafe.push_back("C:drive_relative.vmt");
    unsafe.push_back("\\\\server\\share\\absolute.vmt");
#endif
    for (const std::string& raw : unsafe) {
        const std::string normalized = ui::material_name_from_game_path(raw);
        const auto plan = ui::plan_material_export({normalized}, game_destination());
        UT_CHECK(!plan.ok());
        if (!plan) UT_CHECK_EQ(plan.error().code, vmt::ErrorCode::InvalidArgument);
    }
}

UT_TEST_CASE("save to game: deriving a loose material name retains traversal for rejection") {
    const std::string name = ui::vtf_texture_path(std::filesystem::path(
        "addons/example/materials/models/../../outside.vmt"));
    const auto plan = ui::plan_material_export({name}, game_destination());
    UT_CHECK(!plan.ok());
    if (!plan) UT_CHECK_EQ(plan.error().code, vmt::ErrorCode::InvalidArgument);
}
