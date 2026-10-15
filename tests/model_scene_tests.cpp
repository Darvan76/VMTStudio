#include "ut.h"

#include "ui/model_scene.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Presentation tests for the model materials panel: bodypart grouping, the
// Unused group, the `used_by`/`shared` derivation, the walk order behind the
// next/previous buttons and the display names of the tree rows.

namespace {

void add_material(bridge::ModelScene& scene, int texture_index, std::string name,
                  std::string shader) {
    bridge::ModelMaterial material;
    material.texture_index = texture_index;
    material.name = std::move(name);
    material.shader = std::move(shader);
    scene.materials.push_back(std::move(material));
}

void add_bodypart(bridge::ModelScene& scene, int index, std::string name) {
    bridge::ModelBodypart bodypart;
    bodypart.index = index;
    bodypart.name = std::move(name);
    scene.bodyparts.push_back(std::move(bodypart));
}

void add_use(bridge::ModelScene& scene, int texture_index, int bodypart, int submodel,
             std::string bodypart_name) {
    bridge::ModelMaterialUse use;
    use.texture_index = texture_index;
    use.bodypart = bodypart;
    use.submodel = submodel;
    use.bodypart_name = std::move(bodypart_name);
    scene.material_uses.push_back(std::move(use));
}

} // namespace

UT_TEST_CASE("model_scene: groups follow bodypart order with rows in texture-table order") {
    bridge::ModelScene scene;
    add_material(scene, 0, "models/props/hand", "VertexLitGeneric");
    add_material(scene, 1, "models/props/shared_paint", "VertexLitGeneric");
    add_material(scene, 2, "models/props/weapon", "UnlitGeneric");
    add_material(scene, 3, "models/props/loose_thing", "");
    add_material(scene, 4, "models/props/one_site", "Patch");
    add_bodypart(scene, 0, "hand");
    add_bodypart(scene, 1, "weapon");

    // The dump rows arrive out of order, with a repeated use and with a use
    // that names a texture the material table does not have.
    add_use(scene, 1, 1, 2, "weapon");
    add_use(scene, 0, 0, 0, "hand");
    add_use(scene, 4, 0, 3, "hand");
    add_use(scene, 1, 0, 1, "hand");
    add_use(scene, 2, 1, 0, "weapon");
    add_use(scene, 4, 0, 3, "hand");
    add_use(scene, 42, 5, 0, "ghost");

    const std::vector<ui::ModelMaterialGroup> groups = ui::build_material_groups(scene);
    UT_CHECK_EQ(groups.size(), std::size_t(3));
    UT_CHECK_EQ(groups[0].name, std::string("hand"));
    UT_CHECK_EQ(groups[1].name, std::string("weapon"));
    UT_CHECK_EQ(groups[2].name, std::string(ui::kUnusedMaterialGroupName));

    // "hand" holds texture indices 0, 1 and 4 in texture-table order.
    UT_CHECK_EQ(groups[0].rows.size(), std::size_t(3));
    UT_CHECK_EQ(groups[0].rows[0].texture_index, 0);
    UT_CHECK_EQ(groups[0].rows[0].name, std::string("models/props/hand"));
    UT_CHECK_EQ(groups[0].rows[0].display_name, std::string("hand"));
    UT_CHECK_EQ(groups[0].rows[0].shader, std::string("VertexLitGeneric"));
    UT_CHECK_EQ(groups[0].rows[0].used_by, std::string("hand/0"));
    UT_CHECK(!groups[0].rows[0].shared);
    UT_CHECK_EQ(groups[0].rows[1].texture_index, 1);
    UT_CHECK_EQ(groups[0].rows[1].used_by, std::string("hand/1, weapon/2"));
    UT_CHECK(groups[0].rows[1].shared);
    UT_CHECK_EQ(groups[0].rows[2].texture_index, 4);
    UT_CHECK_EQ(groups[0].rows[2].used_by, std::string("hand/3"));
    UT_CHECK(!groups[0].rows[2].shared); // repeated use rows are one site only

    // The shared material shows up again under "weapon", still once.
    UT_CHECK_EQ(groups[1].rows.size(), std::size_t(2));
    UT_CHECK_EQ(groups[1].rows[0].texture_index, 1);
    UT_CHECK_EQ(groups[1].rows[0].used_by, std::string("hand/1, weapon/2"));
    UT_CHECK(groups[1].rows[0].shared);
    UT_CHECK_EQ(groups[1].rows[1].texture_index, 2);
    UT_CHECK_EQ(groups[1].rows[1].display_name, std::string("weapon"));
    UT_CHECK_EQ(groups[1].rows[1].shader, std::string("UnlitGeneric"));
    UT_CHECK_EQ(groups[1].rows[1].used_by, std::string("weapon/0"));
    UT_CHECK(!groups[1].rows[1].shared);

    // Materials no use points at keep their table order in "Unused".
    UT_CHECK_EQ(groups[2].rows.size(), std::size_t(1));
    UT_CHECK_EQ(groups[2].rows[0].texture_index, 3);
    UT_CHECK_EQ(groups[2].rows[0].display_name, std::string("loose_thing"));
    UT_CHECK_EQ(groups[2].rows[0].shader, std::string(""));
    UT_CHECK_EQ(groups[2].rows[0].used_by, std::string(""));
    UT_CHECK(!groups[2].rows[0].shared);
}

UT_TEST_CASE("model_scene: used_by lists unique sites sorted by bodypart then submodel") {
    bridge::ModelScene scene;
    add_material(scene, 0, "models/props/cross", "");
    add_material(scene, 1, "models/props/within", "");
    add_bodypart(scene, 0, "hand");
    add_bodypart(scene, 1, "weapon");

    // Inserted in reverse and with a repeat: the row must not follow the use
    // order of the scene, and repeated rows are not sites of their own.
    add_use(scene, 0, 1, 1, "weapon");
    add_use(scene, 0, 1, 1, "weapon");
    add_use(scene, 0, 0, 0, "hand");
    // One bodypart, two submodels, listed highest first.
    add_use(scene, 1, 0, 5, "hand");
    add_use(scene, 1, 0, 2, "hand");

    const std::vector<ui::ModelMaterialGroup> groups = ui::build_material_groups(scene);
    UT_CHECK_EQ(groups.size(), std::size_t(2));
    UT_CHECK_EQ(groups[0].name, std::string("hand"));
    UT_CHECK_EQ(groups[0].rows.size(), std::size_t(2));

    UT_CHECK_EQ(groups[0].rows[0].texture_index, 0);
    UT_CHECK_EQ(groups[0].rows[0].used_by, std::string("hand/0, weapon/1"));
    UT_CHECK(groups[0].rows[0].shared);

    UT_CHECK_EQ(groups[0].rows[1].texture_index, 1);
    UT_CHECK_EQ(groups[0].rows[1].used_by, std::string("hand/2, hand/5"));
    UT_CHECK(groups[0].rows[1].shared);

    // The weapon group carries the shared material with the same list.
    UT_CHECK_EQ(groups[1].name, std::string("weapon"));
    UT_CHECK_EQ(groups[1].rows.size(), std::size_t(1));
    UT_CHECK_EQ(groups[1].rows[0].texture_index, 0);
    UT_CHECK_EQ(groups[1].rows[0].used_by, std::string("hand/0, weapon/1"));
}

UT_TEST_CASE("model_scene: shared marks materials used by several submodels") {
    bridge::ModelScene scene;
    add_material(scene, 0, "models/props/single", "");
    add_material(scene, 1, "models/props/same_bodypart", "");
    add_material(scene, 2, "models/props/across_bodyparts", "");
    add_bodypart(scene, 0, "hand");
    add_bodypart(scene, 1, "weapon");

    add_use(scene, 0, 0, 0, "hand");
    add_use(scene, 0, 0, 0, "hand"); // repeated use rows are still one site
    add_use(scene, 1, 0, 0, "hand");
    add_use(scene, 1, 0, 3, "hand");
    add_use(scene, 2, 0, 0, "hand");
    add_use(scene, 2, 1, 0, "weapon");

    const std::vector<ui::ModelMaterialGroup> groups = ui::build_material_groups(scene);
    UT_CHECK_EQ(groups.size(), std::size_t(2));
    UT_CHECK_EQ(groups[0].rows.size(), std::size_t(3));
    UT_CHECK(!groups[0].rows[0].shared);
    UT_CHECK(groups[0].rows[1].shared);
    UT_CHECK(groups[0].rows[2].shared);

    // Only the material shared across bodyparts shows up under "weapon".
    UT_CHECK_EQ(groups[1].rows.size(), std::size_t(1));
    UT_CHECK_EQ(groups[1].rows[0].texture_index, 2);
    UT_CHECK(groups[1].rows[0].shared);
}

UT_TEST_CASE("model_scene: flatten walks groups then rows") {
    bridge::ModelScene scene;
    add_material(scene, 0, "models/props/unused_a", "");
    add_material(scene, 1, "models/props/hand_skin", "");
    add_material(scene, 2, "models/props/weapon_skin", "");
    add_bodypart(scene, 0, "hand");
    add_bodypart(scene, 1, "weapon");
    add_use(scene, 1, 0, 0, "hand");
    add_use(scene, 2, 1, 0, "weapon");

    const std::vector<ui::ModelMaterialGroup> groups = ui::build_material_groups(scene);
    const std::vector<const ui::ModelMaterialRow*> rows = ui::flatten_material_rows(groups);
    UT_CHECK_EQ(rows.size(), std::size_t(3));

    // The pointers address the original groups: hand, weapon, then Unused.
    // Wrapping around at the ends is the widget's job, not this model's.
    UT_CHECK(rows[0] == &groups[0].rows[0]);
    UT_CHECK(rows[1] == &groups[1].rows[0]);
    UT_CHECK(rows[2] == &groups[2].rows[0]);
    UT_CHECK_EQ(rows[0]->display_name, std::string("hand_skin"));
    UT_CHECK_EQ(rows[1]->display_name, std::string("weapon_skin"));
    UT_CHECK_EQ(rows[2]->display_name, std::string("unused_a"));

    const std::vector<ui::ModelMaterialGroup> none;
    UT_CHECK(ui::flatten_material_rows(none).empty());
}

UT_TEST_CASE("model_scene: display name keeps the last path segment") {
    // The segment after the final slash.
    UT_CHECK_EQ(ui::material_display_name("models/props_c17/oildrum001"),
                std::string("oildrum001"));
    UT_CHECK_EQ(ui::material_display_name("materials/dev/dev_measuregeneric01"),
                std::string("dev_measuregeneric01"));

    // Without slashes the whole name is the display name.
    UT_CHECK_EQ(ui::material_display_name("oildrum001"), std::string("oildrum001"));
    UT_CHECK_EQ(ui::material_display_name("brickwall001a"), std::string("brickwall001a"));

    // A trailing slash leaves an empty last segment: the previous one shows.
    UT_CHECK_EQ(ui::material_display_name("models/props_c17/"), std::string("props_c17"));
    UT_CHECK_EQ(ui::material_display_name("models/props_c17//"), std::string("props_c17"));
    UT_CHECK_EQ(ui::material_display_name("models//oildrum001"), std::string("oildrum001"));

    // No segment at all: empty in, empty out.
    UT_CHECK_EQ(ui::material_display_name(""), std::string(""));
    UT_CHECK_EQ(ui::material_display_name("/"), std::string(""));
    UT_CHECK_EQ(ui::material_display_name("///"), std::string(""));
}

UT_TEST_CASE("model_scene: empty scenes have no groups, unused materials form the Unused group") {
    const bridge::ModelScene empty;
    UT_CHECK(ui::build_material_groups(empty).empty());
    UT_CHECK(ui::flatten_material_rows(ui::build_material_groups(empty)).empty());

    // Materials without any use: a single Unused group in table order.
    bridge::ModelScene no_uses;
    add_material(no_uses, 0, "models/props/one", "VertexLitGeneric");
    add_material(no_uses, 1, "models/props/two", "");
    const std::vector<ui::ModelMaterialGroup> groups = ui::build_material_groups(no_uses);
    UT_CHECK_EQ(groups.size(), std::size_t(1));
    UT_CHECK_EQ(groups[0].name, std::string(ui::kUnusedMaterialGroupName));
    UT_CHECK_EQ(groups[0].rows.size(), std::size_t(2));
    UT_CHECK_EQ(groups[0].rows[0].texture_index, 0);
    UT_CHECK_EQ(groups[0].rows[1].texture_index, 1);
}

UT_TEST_CASE("model_scene: uses with unknown texture indices are ignored") {
    bridge::ModelScene scene;
    add_material(scene, 0, "models/props/one", "");
    add_use(scene, 7, 0, 0, "hand");
    add_use(scene, 9, 1, 2, "weapon");

    // Neither phantom bodypart creates a group: every material stays unused.
    const std::vector<ui::ModelMaterialGroup> groups = ui::build_material_groups(scene);
    UT_CHECK_EQ(groups.size(), std::size_t(1));
    UT_CHECK_EQ(groups[0].name, std::string(ui::kUnusedMaterialGroupName));
    UT_CHECK_EQ(groups[0].rows.size(), std::size_t(1));
    UT_CHECK_EQ(groups[0].rows[0].texture_index, 0);
    UT_CHECK_EQ(groups[0].rows[0].used_by, std::string(""));
    UT_CHECK(!groups[0].rows[0].shared);
}

UT_TEST_CASE("model_scene: group names prefer the bodypart table, then the use rows, then the index") {
    bridge::ModelScene scene;
    add_material(scene, 0, "models/props/hand_skin", "");
    add_material(scene, 1, "models/props/weapon_skin", "");
    add_bodypart(scene, 0, "hand_from_table");
    // Bodypart 1 has no bodyparts-table entry: the name the use rows carry
    // stands in for it.
    add_use(scene, 0, 0, 0, "hand_from_use");
    add_use(scene, 1, 1, 0, "weapon_from_use");

    const std::vector<ui::ModelMaterialGroup> groups = ui::build_material_groups(scene);
    UT_CHECK_EQ(groups.size(), std::size_t(2));
    UT_CHECK_EQ(groups[0].name, std::string("hand_from_table"));
    UT_CHECK_EQ(groups[1].name, std::string("weapon_from_use"));
    UT_CHECK_EQ(groups[0].rows[0].used_by, std::string("hand_from_table/0"));
    UT_CHECK_EQ(groups[1].rows[0].used_by, std::string("weapon_from_use/0"));

    // With no name anywhere the index still gives the group a label.
    bridge::ModelScene nameless;
    add_material(nameless, 0, "models/props/skin", "");
    add_use(nameless, 0, 2, 0, "");
    const std::vector<ui::ModelMaterialGroup> labeled = ui::build_material_groups(nameless);
    UT_CHECK_EQ(labeled.size(), std::size_t(1));
    UT_CHECK_EQ(labeled[0].name, std::string("2"));
    UT_CHECK_EQ(labeled[0].rows[0].used_by, std::string("2/0"));
}
