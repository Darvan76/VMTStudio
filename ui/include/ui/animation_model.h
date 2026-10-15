#pragma once

// ui animation_model - presentation model for the animation player dock.
// Windows-free and Qt-free, so the logic stays unit testable: it turns a model
// scene dump plus a filter into the sequence list the combo shows, and formats
// the playback state for the status line.

#include "bridge/engine_view.h"

#include <string>
#include <vector>

namespace ui {

// Combo filter, mirroring the HLMV "View" menu options.
struct SequenceFilter {
    bool show_hidden = false;   // include STUDIO_HIDDEN sequences
    bool by_activity = false;   // show activity names instead of labels
    bool alphabetical = false;  // sort by display name; otherwise engine order
};

// One entry of the sequence combo.
struct SequenceRow {
    int index = 0;            // engine sequence index
    std::string label;
    std::string activity;
    std::string display;      // what the combo shows (label or activity)
    float fps = 0.0f;
    int frames = 0;
    float duration = 0.0f;    // seconds
    bool looping = false;
    bool hidden = false;
};

// Builds the combo rows: hidden sequences are dropped unless requested,
// `display` prefers the activity name (when by_activity is set and the activity
// is non-empty) and otherwise uses the label, falling back to
// "sequence <index>" when both are empty. Alphabetical ordering is stable and
// case-insensitive; otherwise the engine order is kept.
std::vector<SequenceRow> build_sequence_rows(const bridge::ModelScene& scene,
                                             const SequenceFilter& filter);

// Speed scale bounds of the animation slider.
constexpr float kAnimationSpeedMin = 0.05f;
constexpr float kAnimationSpeedMax = 4.0f;

// Clamps a speed scale into the slider range.
float clamp_animation_speed(float speed);

// "1.23 s" / "0.00 s"; two decimals, no unit suffix surprises.
std::string format_duration(float seconds);

// "frame 12 / 40 - 30.0 fps" for the status line of the dock. Uses the
// sequence the state points at and falls back to the first sequence (or zeros
// when the scene has none). Appends " - 12.3 u/s" while the model moves at a
// non-zero ground speed and " (paused)" while playback is paused.
std::string format_frame_status(const bridge::AnimState& state,
                                const bridge::ModelScene& scene);

} // namespace ui
