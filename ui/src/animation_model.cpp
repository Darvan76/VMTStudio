#include "ui/animation_model.h"

// Scaffold: presentation logic lands with the animation dock work.

#include <algorithm>

namespace ui {

std::vector<SequenceRow> build_sequence_rows(const bridge::ModelScene&, const SequenceFilter&) {
    return {};
}

float clamp_animation_speed(float speed) {
    return std::max(kAnimationSpeedMin, std::min(kAnimationSpeedMax, speed));
}

std::string format_duration(float) {
    return {};
}

std::string format_frame_status(const bridge::AnimState&, const bridge::ModelScene&) {
    return {};
}

} // namespace ui
