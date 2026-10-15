#include "ut.h"

#include "ui/export_plan.h"

#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

// Tests for ui::plan_material_export() / ui::flat_material_file_name(), the
// filesystem-free planner behind "Export Model Materials". The cases cover the
// flat and materials-tree layouts, name sanitization, duplicate detection,
// input de-duplication and the rejection of unsafe material names; nothing
// here touches the filesystem.

namespace {

namespace fs = std::filesystem;

std::vector<std::string> make_materials(std::initializer_list<const char*> names) {
    return std::vector<std::string>(names.begin(), names.end());
}

} // namespace

UT_TEST_CASE("export_plan: flat names flatten the path and replace unsafe characters") {
    UT_CHECK_EQ(ui::flat_material_file_name("models/props_c17/oildrum001"),
                std::string("models_props_c17_oildrum001.vmt"));
    UT_CHECK_EQ(ui::flat_material_file_name("models\\props_c17\\oildrum001"),
                std::string("models_props_c17_oildrum001.vmt"));
    UT_CHECK_EQ(ui::flat_material_file_name("a b/c d"), std::string("a_b_c_d.vmt"));
    UT_CHECK_EQ(ui::flat_material_file_name("vertexlit::generic"),
                std::string("vertexlit__generic.vmt"));
    UT_CHECK_EQ(ui::flat_material_file_name(u8"über/straße"),
                std::string("__ber_stra__e.vmt"));
    UT_CHECK_EQ(ui::flat_material_file_name("mix-a.b_c-1"),
                std::string("mix-a.b_c-1.vmt"));
}

UT_TEST_CASE("export_plan: flat names without any readable character fail") {
    UT_CHECK_EQ(ui::flat_material_file_name(""), std::string());
    UT_CHECK_EQ(ui::flat_material_file_name("   "), std::string());
    UT_CHECK_EQ(ui::flat_material_file_name("///"), std::string());
    UT_CHECK_EQ(ui::flat_material_file_name("::"), std::string());
    UT_CHECK_EQ(ui::flat_material_file_name("___"), std::string());
}

UT_TEST_CASE("export_plan: flat layout keeps the input order and the destination root") {
    ui::ExportOptions options;
    options.destination = fs::path("export_out");

    const auto plan = ui::plan_material_export(
        make_materials({"models/props_c17/oildrum001", "brick/brickwall001a"}), options);
    UT_CHECK(plan.ok());
    if (!plan.ok()) {
        return;
    }

    const ui::ExportPlan& value = plan.value();
    UT_CHECK_EQ(value.items.size(), std::size_t(2));
    UT_CHECK(value.duplicates.empty());
    if (value.items.size() != std::size_t(2)) {
        return;
    }

    UT_CHECK_EQ(value.items[0].material, std::string("models/props_c17/oildrum001"));
    UT_CHECK_EQ(value.items[0].file_name, std::string("models_props_c17_oildrum001.vmt"));
    UT_CHECK_EQ(value.items[0].relative.generic_string(),
                std::string("models_props_c17_oildrum001.vmt"));
    UT_CHECK_EQ(value.items[0].absolute.generic_string(),
                std::string("export_out/models_props_c17_oildrum001.vmt"));

    UT_CHECK_EQ(value.items[1].material, std::string("brick/brickwall001a"));
    UT_CHECK_EQ(value.items[1].file_name, std::string("brick_brickwall001a.vmt"));
    UT_CHECK_EQ(value.items[1].absolute.generic_string(),
                std::string("export_out/brick_brickwall001a.vmt"));
}

UT_TEST_CASE("export_plan: materials tree layout mirrors the game folder") {
    ui::ExportOptions options;
    options.layout = ui::ExportLayout::MaterialsTree;
    options.destination = fs::path("export_out");

    const auto plan = ui::plan_material_export(
        make_materials({"models/props_c17/oildrum001"}), options);
    UT_CHECK(plan.ok());
    if (!plan.ok() || plan.value().items.size() != std::size_t(1)) {
        return;
    }

    const ui::ExportItem& item = plan.value().items[0];
    UT_CHECK_EQ(item.material, std::string("models/props_c17/oildrum001"));
    UT_CHECK_EQ(item.file_name, std::string("oildrum001.vmt"));
    UT_CHECK_EQ(item.relative.generic_string(),
                std::string("materials/models/props_c17/oildrum001.vmt"));
    UT_CHECK_EQ(item.absolute.generic_string(),
                std::string("export_out/materials/models/props_c17/oildrum001.vmt"));
}

UT_TEST_CASE("export_plan: tree layout normalizes separators and reports collisions") {
    ui::ExportOptions options;
    options.layout = ui::ExportLayout::MaterialsTree;
    options.destination = fs::path("export_out");

    const auto plan = ui::plan_material_export(
        make_materials({"a/b", "a//b", "a/./b"}), options);
    UT_CHECK(plan.ok());
    if (!plan.ok()) {
        return;
    }

    const ui::ExportPlan& value = plan.value();
    UT_CHECK_EQ(value.items.size(), std::size_t(1));
    UT_CHECK_EQ(value.duplicates.size(), std::size_t(2));
    if (value.items.size() == std::size_t(1)) {
        UT_CHECK_EQ(value.items[0].material, std::string("a/b"));
        UT_CHECK_EQ(value.items[0].relative.generic_string(),
                    std::string("materials/a/b.vmt"));
    }
    if (value.duplicates.size() == std::size_t(2)) {
        UT_CHECK_EQ(value.duplicates[0], std::string("a//b"));
        UT_CHECK_EQ(value.duplicates[1], std::string("a/./b"));
    }
}

UT_TEST_CASE("export_plan: flat layout reports materials that collapse to one file") {
    ui::ExportOptions options;
    options.destination = fs::path("export_out");

    const auto plan = ui::plan_material_export(make_materials({"a/b_c", "a_b/c"}), options);
    UT_CHECK(plan.ok());
    if (!plan.ok()) {
        return;
    }

    const ui::ExportPlan& value = plan.value();
    UT_CHECK_EQ(value.items.size(), std::size_t(1));
    UT_CHECK_EQ(value.duplicates.size(), std::size_t(1));
    if (value.items.size() == std::size_t(1)) {
        UT_CHECK_EQ(value.items[0].material, std::string("a/b_c"));
        UT_CHECK_EQ(value.items[0].file_name, std::string("a_b_c.vmt"));
    }
    if (value.duplicates.size() == std::size_t(1)) {
        UT_CHECK_EQ(value.duplicates[0], std::string("a_b/c"));
    }
}

UT_TEST_CASE("export_plan: repeated material names are exported once and order is kept") {
    ui::ExportOptions options;
    options.destination = fs::path("export_out");

    const auto plan = ui::plan_material_export(
        make_materials({"b/second", "a/first", "b/second", "c/third"}), options);
    UT_CHECK(plan.ok());
    if (!plan.ok()) {
        return;
    }

    const ui::ExportPlan& value = plan.value();
    UT_CHECK_EQ(value.items.size(), std::size_t(3));
    UT_CHECK(value.duplicates.empty());
    if (value.items.size() == std::size_t(3)) {
        UT_CHECK_EQ(value.items[0].material, std::string("b/second"));
        UT_CHECK_EQ(value.items[1].material, std::string("a/first"));
        UT_CHECK_EQ(value.items[2].material, std::string("c/third"));
    }
}

UT_TEST_CASE("export_plan: an empty destination is rejected") {
    ui::ExportOptions options;
    const auto plan = ui::plan_material_export(make_materials({"brick/brickwall001a"}), options);
    UT_CHECK(!plan.ok());
    if (!plan.ok()) {
        UT_CHECK(plan.error().code == vmt::ErrorCode::InvalidArgument);
    }
}

UT_TEST_CASE("export_plan: blank material names and unusable flat names are rejected") {
    ui::ExportOptions options;
    options.destination = fs::path("export_out");

    const auto empty = ui::plan_material_export(make_materials({""}), options);
    UT_CHECK(!empty.ok());
    if (!empty.ok()) {
        UT_CHECK(empty.error().code == vmt::ErrorCode::InvalidArgument);
    }

    const auto blank = ui::plan_material_export(make_materials({"   ", "a/b"}), options);
    UT_CHECK(!blank.ok());
    if (!blank.ok()) {
        UT_CHECK(blank.error().code == vmt::ErrorCode::InvalidArgument);
    }

    const auto unusable = ui::plan_material_export(make_materials({"///"}), options);
    UT_CHECK(!unusable.ok());
    if (!unusable.ok()) {
        UT_CHECK(unusable.error().code == vmt::ErrorCode::InvalidArgument);
    }
}

UT_TEST_CASE("export_plan: materials cannot escape the destination folder") {
    ui::ExportOptions options;
    options.layout = ui::ExportLayout::MaterialsTree;
    options.destination = fs::path("export_out");

    const auto traversal = ui::plan_material_export(make_materials({"../../escape"}), options);
    UT_CHECK(!traversal.ok());
    if (!traversal.ok()) {
        UT_CHECK(traversal.error().code == vmt::ErrorCode::InvalidArgument);
    }

    const auto innerTraversal = ui::plan_material_export(make_materials({"a/../../b"}), options);
    UT_CHECK(!innerTraversal.ok());
    if (!innerTraversal.ok()) {
        UT_CHECK(innerTraversal.error().code == vmt::ErrorCode::InvalidArgument);
    }

    const auto dotDot = ui::plan_material_export(make_materials({".."}), options);
    UT_CHECK(!dotDot.ok());
    if (!dotDot.ok()) {
        UT_CHECK(dotDot.error().code == vmt::ErrorCode::InvalidArgument);
    }

    const auto rooted = ui::plan_material_export(make_materials({"/root/absolute"}), options);
    UT_CHECK(!rooted.ok());
    if (!rooted.ok()) {
        UT_CHECK(rooted.error().code == vmt::ErrorCode::InvalidArgument);
    }

#ifdef _WIN32
    // Drive-style names only parse as absolute on Windows.
    const auto drive = ui::plan_material_export(make_materials({"C:/windows/temp"}), options);
    UT_CHECK(!drive.ok());
    if (!drive.ok()) {
        UT_CHECK(drive.error().code == vmt::ErrorCode::InvalidArgument);
    }
#endif

    // The flat layout has no folders at all: slashes become '_' and the file
    // still lands directly inside the destination.
    ui::ExportOptions flat;
    flat.destination = fs::path("export_out");
    const auto flatPlan = ui::plan_material_export(make_materials({"../../escape"}), flat);
    UT_CHECK(flatPlan.ok());
    if (flatPlan.ok() && flatPlan.value().items.size() == std::size_t(1)) {
        UT_CHECK_EQ(flatPlan.value().items[0].file_name, std::string(".._.._escape.vmt"));
        UT_CHECK_EQ(flatPlan.value().items[0].relative.generic_string(),
                    std::string(".._.._escape.vmt"));
    }
}
