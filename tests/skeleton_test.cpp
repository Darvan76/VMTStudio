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
