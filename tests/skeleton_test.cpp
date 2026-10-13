#include "ut.h"

// Including every module's public header here verifies that all of them
// compile and that their static libraries link. Phase 1 has exactly one test.
#include "vmt/vmt_core.h"
#include "vtf/vtf_reader.h"
#include "render/renderer.h"
#include "bridge/engine_bridge.h"
#include "profiles/game_profiles.h"
#include "ui/ui.h"

#include <cstring>

UT_TEST_CASE("skeleton: all module public headers compile and link") {
    UT_CHECK(std::strlen(vmt::vmt_core_version_string()) > 0);
    UT_CHECK(std::strlen(vtf::vtf_reader_version_string()) > 0);
    UT_CHECK(std::strlen(render::renderer_version_string()) > 0);
    UT_CHECK(std::strlen(bridge::engine_bridge_version_string()) > 0);
    UT_CHECK(std::strlen(profiles::game_profiles_version_string()) > 0);
    UT_CHECK(std::strlen(ui::ui_version_string()) > 0);
}

#include "bridge/engine_view.h"

UT_TEST_CASE("engine_view: interface methods for render mode, animation, speed, and thumbnails") {
    auto view_res = bridge::EngineView::create();
    UT_CHECK(view_res.ok());
    auto view = std::move(view_res.value());
    UT_CHECK(view != nullptr);
    UT_CHECK(!view->initialized());

    // Calling before initialization should report errors cleanly without crashing
    auto thumb_res = view->render_material_thumbnail("test_mat", 128, false);
    UT_CHECK(!thumb_res.ok());

    auto mode_res = view->set_render_mode("wireframe");
    UT_CHECK(!mode_res.ok());

    auto anim_res = view->set_animation("pause");
    UT_CHECK(!anim_res.ok());

    auto speed_res = view->set_speed(0.5f);
    UT_CHECK(!speed_res.ok());
}
