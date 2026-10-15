#include "ut.h"

#include "ui/animation_model.h"

// Scaffold: real coverage lands with the animation dock work.

UT_TEST_CASE("animation_model: scaffold placeholder") {
    UT_CHECK(ui::clamp_animation_speed(1.0f) == 1.0f);
}
