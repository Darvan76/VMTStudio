#include "ut.h"

// Compile the public headers and link real entry points from each module.
#include "vmt/vmt_core.h"
#include "vtf/vtf_reader.h"
#include "bridge/engine_view.h"
#include "bridge/pe.h"
#include "profiles/game_manager.h"
#include "ui/ui.h"

#include <string>

UT_TEST_CASE("modules: public headers compile and real entry points link") {
    UT_CHECK(vmt::Document::parse("UnlitGeneric {}\n").ok());
    UT_CHECK(!vtf::list_formats().empty());
    UT_CHECK_EQ(std::string(bridge::to_string(bridge::PeArch::X86)), "x86");
    profiles::GameManager games;
    games.clear();
    UT_CHECK(games.profiles().empty());
    ui::EditorSession session;
    UT_CHECK(session.set_text("UnlitGeneric {}\n").ok());
}

UT_TEST_CASE("engine_view: render mode, animation and speed reject uninitialized calls") {
    auto view_res = bridge::EngineView::create();
    UT_CHECK(view_res.ok());
    auto view = std::move(view_res.value());
    UT_CHECK(view != nullptr);
    UT_CHECK(!view->initialized());

    // Calling before initialization should report errors cleanly without crashing
    auto mode_res = view->set_render_mode("wireframe");
    UT_CHECK(!mode_res.ok());

    auto anim_res = view->set_animation("pause");
    UT_CHECK(!anim_res.ok());

    auto speed_res = view->set_speed(0.5f);
    UT_CHECK(!speed_res.ok());
}

UT_TEST_CASE("engine_view: cancelled startup never launches the backend") {
    auto result = bridge::EngineView::create();
    UT_CHECK(result.ok());
    if (!result) return;
    bridge::EngineViewConfig config;
    config.bin_dir = "cancelled/bin";
    config.game_dir = "cancelled/game";
    config.cancelled = std::make_shared<std::atomic<bool>>(true);
    const auto initialized = result.value()->initialize(config);
    UT_CHECK(!initialized);
    if (!initialized) {
        UT_CHECK(initialized.error().code == vmt::ErrorCode::Internal);
        UT_CHECK(initialized.error().message.find("cancelled") != std::string::npos);
    }
    UT_CHECK(!result.value()->initialized());
}
