#include "ut.h"

#include "ui/animation_model.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

bridge::ModelSequence make_sequence(int index, std::string label, std::string activity,
                                    float fps, int frames, float duration, bool looping,
                                    bool hidden) {
    bridge::ModelSequence sequence;
    sequence.index = index;
    sequence.label = std::move(label);
    sequence.activity = std::move(activity);
    sequence.fps = fps;
    sequence.frames = frames;
    sequence.duration = duration;
    sequence.looping = looping;
    sequence.hidden = hidden;
    return sequence;
}

} // namespace

UT_TEST_CASE("animation_model: hidden sequences follow the show-hidden filter") {
    bridge::ModelScene scene;
    scene.sequences.push_back(make_sequence(0, "idle", "", 30.0f, 40, 1.33f, true, false));
    scene.sequences.push_back(make_sequence(1, "secret_pose", "", 10.0f, 5, 0.50f, false, true));
    scene.sequences.push_back(make_sequence(2, "run", "", 20.0f, 20, 1.00f, true, false));

    const ui::SequenceFilter plain;
    const std::vector<ui::SequenceRow> visible = ui::build_sequence_rows(scene, plain);
    UT_CHECK_EQ(visible.size(), std::size_t(2));
    UT_CHECK_EQ(visible[0].index, 0);
    UT_CHECK_EQ(visible[1].index, 2);
    UT_CHECK(!visible[0].hidden);
    UT_CHECK(!visible[1].hidden);

    ui::SequenceFilter all;
    all.show_hidden = true;
    const std::vector<ui::SequenceRow> rows = ui::build_sequence_rows(scene, all);
    UT_CHECK_EQ(rows.size(), std::size_t(3));
    UT_CHECK_EQ(rows[0].index, 0);
    UT_CHECK_EQ(rows[1].index, 1);
    UT_CHECK_EQ(rows[2].index, 2);
    UT_CHECK(rows[1].hidden);
    UT_CHECK(!rows[2].hidden);
}

UT_TEST_CASE("animation_model: display picks activity, label or fallback") {
    bridge::ModelScene scene;
    scene.sequences.push_back(make_sequence(0, "idle_a", "ACT_IDLE", 30.0f, 40, 1.33f, true, false));
    scene.sequences.push_back(make_sequence(1, "wave", "", 20.0f, 10, 0.50f, false, false));
    scene.sequences.push_back(make_sequence(2, "", "", 15.0f, 8, 0.53f, false, false));

    const ui::SequenceFilter by_label;
    const std::vector<ui::SequenceRow> labels = ui::build_sequence_rows(scene, by_label);
    UT_CHECK_EQ(labels.size(), std::size_t(3));
    UT_CHECK_EQ(labels[0].display, std::string("idle_a"));
    UT_CHECK_EQ(labels[1].display, std::string("wave"));
    UT_CHECK_EQ(labels[2].display, std::string("sequence 2"));
    // The raw fields are copied unchanged, whatever the display picks.
    UT_CHECK_EQ(labels[0].label, std::string("idle_a"));
    UT_CHECK_EQ(labels[0].activity, std::string("ACT_IDLE"));

    ui::SequenceFilter by_activity;
    by_activity.by_activity = true;
    const std::vector<ui::SequenceRow> activities = ui::build_sequence_rows(scene, by_activity);
    UT_CHECK_EQ(activities.size(), std::size_t(3));
    UT_CHECK_EQ(activities[0].display, std::string("ACT_IDLE"));
    // No activity: the label stands in, and with neither the index fallback.
    UT_CHECK_EQ(activities[1].display, std::string("wave"));
    UT_CHECK_EQ(activities[2].display, std::string("sequence 2"));
}

UT_TEST_CASE("animation_model: alphabetical order is case-insensitive and stable") {
    bridge::ModelScene scene;
    scene.sequences.push_back(make_sequence(0, "beta", "", 30.0f, 40, 1.33f, true, false));
    scene.sequences.push_back(make_sequence(1, "Alpha", "", 20.0f, 10, 0.50f, false, false));
    scene.sequences.push_back(make_sequence(2, "BETA", "", 15.0f, 8, 0.53f, false, false));

    const ui::SequenceFilter engine_order;
    const std::vector<ui::SequenceRow> unsorted = ui::build_sequence_rows(scene, engine_order);
    UT_CHECK_EQ(unsorted.size(), std::size_t(3));
    UT_CHECK_EQ(unsorted[0].index, 0);
    UT_CHECK_EQ(unsorted[1].index, 1);
    UT_CHECK_EQ(unsorted[2].index, 2);

    ui::SequenceFilter alphabetical;
    alphabetical.alphabetical = true;
    const std::vector<ui::SequenceRow> sorted = ui::build_sequence_rows(scene, alphabetical);
    UT_CHECK_EQ(sorted.size(), std::size_t(3));
    UT_CHECK_EQ(sorted[0].display, std::string("Alpha"));
    // "beta" and "BETA" compare equal ignoring case: the engine order is kept.
    UT_CHECK_EQ(sorted[1].index, 0);
    UT_CHECK_EQ(sorted[1].display, std::string("beta"));
    UT_CHECK_EQ(sorted[2].index, 2);
    UT_CHECK_EQ(sorted[2].display, std::string("BETA"));
}

UT_TEST_CASE("animation_model: rows copy the playback metadata") {
    bridge::ModelScene scene;
    scene.sequences.push_back(make_sequence(3, "walk", "ACT_WALK", 24.5f, 37, 1.51f, true, false));
    scene.sequences.push_back(make_sequence(4, "crouch", "", 12.0f, 9, 0.75f, false, true));

    ui::SequenceFilter filter;
    filter.show_hidden = true;
    const std::vector<ui::SequenceRow> rows = ui::build_sequence_rows(scene, filter);
    UT_CHECK_EQ(rows.size(), std::size_t(2));

    UT_CHECK_EQ(rows[0].index, 3);
    UT_CHECK_EQ(rows[0].label, std::string("walk"));
    UT_CHECK_EQ(rows[0].activity, std::string("ACT_WALK"));
    UT_CHECK(rows[0].fps == 24.5f);
    UT_CHECK_EQ(rows[0].frames, 37);
    UT_CHECK(rows[0].duration == 1.51f);
    UT_CHECK(rows[0].looping);
    UT_CHECK(!rows[0].hidden);

    UT_CHECK_EQ(rows[1].index, 4);
    UT_CHECK(rows[1].fps == 12.0f);
    UT_CHECK_EQ(rows[1].frames, 9);
    UT_CHECK(rows[1].duration == 0.75f);
    UT_CHECK(!rows[1].looping);
    UT_CHECK(rows[1].hidden);
}

UT_TEST_CASE("animation_model: speed clamps into the slider range") {
    UT_CHECK(ui::clamp_animation_speed(0.0f) == ui::kAnimationSpeedMin);
    UT_CHECK(ui::clamp_animation_speed(-2.0f) == ui::kAnimationSpeedMin);
    UT_CHECK(ui::clamp_animation_speed(9.0f) == ui::kAnimationSpeedMax);
    UT_CHECK(ui::clamp_animation_speed(2.5f) == 2.5f);
    UT_CHECK(ui::clamp_animation_speed(ui::kAnimationSpeedMin) == ui::kAnimationSpeedMin);
    UT_CHECK(ui::clamp_animation_speed(ui::kAnimationSpeedMax) == ui::kAnimationSpeedMax);
}

UT_TEST_CASE("animation_model: durations always carry two decimals") {
    UT_CHECK_EQ(ui::format_duration(0.0f), std::string("0.00 s"));
    UT_CHECK_EQ(ui::format_duration(1.234f), std::string("1.23 s"));
    UT_CHECK_EQ(ui::format_duration(12.5f), std::string("12.50 s"));
    UT_CHECK_EQ(ui::format_duration(2.0f), std::string("2.00 s"));
}

UT_TEST_CASE("animation_model: frame status names the pointed sequence") {
    bridge::ModelScene scene;
    scene.sequences.push_back(make_sequence(0, "idle", "", 30.0f, 40, 1.33f, true, false));
    scene.sequences.push_back(make_sequence(1, "run", "", 20.0f, 10, 0.50f, true, false));

    bridge::AnimState state;
    state.sequence = 0;
    state.frame = 12.0f;
    UT_CHECK_EQ(ui::format_frame_status(state, scene), std::string("frame 12 / 40 - 30.0 fps"));

    // The state selects the second sequence: its frame count and fps show.
    state.sequence = 1;
    state.frame = 3.0f;
    UT_CHECK_EQ(ui::format_frame_status(state, scene), std::string("frame 3 / 10 - 20.0 fps"));

    // Fractional playheads round to the nearest frame.
    state.sequence = 0;
    state.frame = 12.4f;
    UT_CHECK_EQ(ui::format_frame_status(state, scene), std::string("frame 12 / 40 - 30.0 fps"));
    state.frame = 12.6f;
    UT_CHECK_EQ(ui::format_frame_status(state, scene), std::string("frame 13 / 40 - 30.0 fps"));
}

UT_TEST_CASE("animation_model: frame status appends ground speed and paused") {
    bridge::ModelScene scene;
    scene.sequences.push_back(make_sequence(0, "walk", "", 30.0f, 40, 1.33f, true, false));

    bridge::AnimState state;
    state.sequence = 0;
    state.frame = 12.0f;

    // Zero ground speed and running playback stay out of the status.
    UT_CHECK_EQ(ui::format_frame_status(state, scene), std::string("frame 12 / 40 - 30.0 fps"));

    state.ground_speed = 12.3f;
    UT_CHECK_EQ(ui::format_frame_status(state, scene),
                std::string("frame 12 / 40 - 30.0 fps - 12.3 u/s"));

    state.paused = true;
    UT_CHECK_EQ(ui::format_frame_status(state, scene),
                std::string("frame 12 / 40 - 30.0 fps - 12.3 u/s (paused)"));

    state.ground_speed = 0.0f;
    UT_CHECK_EQ(ui::format_frame_status(state, scene),
                std::string("frame 12 / 40 - 30.0 fps (paused)"));
}

UT_TEST_CASE("animation_model: frame status falls back for unknown sequences") {
    bridge::ModelScene scene;
    scene.sequences.push_back(make_sequence(0, "idle", "", 30.0f, 40, 1.33f, true, false));
    scene.sequences.push_back(make_sequence(1, "run", "", 20.0f, 10, 0.50f, true, false));

    bridge::AnimState state;
    state.sequence = 99; // out of range: the first sequence stands in
    state.frame = 7.0f;
    UT_CHECK_EQ(ui::format_frame_status(state, scene), std::string("frame 7 / 40 - 30.0 fps"));

    // No sequences at all: zeros instead of a crash.
    const bridge::ModelScene empty;
    bridge::AnimState empty_state;
    UT_CHECK_EQ(ui::format_frame_status(empty_state, empty), std::string("frame 0 / 0 - 0.0 fps"));
}
