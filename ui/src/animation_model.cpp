#include "ui/animation_model.h"

// Presentation logic behind the animation dock: builds the sequence combo
// rows out of a model scene dump and formats the playback values shown in the
// status line. Kept free of Windows and Qt headers so the whole module is unit
// tested by model_view_tests.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace ui {

namespace {

// Lowercase ASCII copy used for the case-insensitive ordering. Sequence names
// are ASCII in practice; doing it by hand keeps the order independent of the
// process locale.
std::string ascii_lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 'A' && byte <= 'Z') {
            c = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return result;
}

// The sequence the state points at, or the first one when the state carries an
// index the scene does not know. Null when the scene has no sequences at all.
const bridge::ModelSequence* sequence_for_state(const bridge::AnimState& state,
                                                const bridge::ModelScene& scene) {
    for (const bridge::ModelSequence& candidate : scene.sequences) {
        if (candidate.index == state.sequence) {
            return &candidate;
        }
    }
    if (!scene.sequences.empty()) {
        return &scene.sequences.front();
    }
    return nullptr;
}

} // namespace

std::vector<SequenceRow> build_sequence_rows(const bridge::ModelScene& scene,
                                             const SequenceFilter& filter) {
    std::vector<SequenceRow> rows;
    rows.reserve(scene.sequences.size());
    for (const bridge::ModelSequence& sequence : scene.sequences) {
        if (sequence.hidden && !filter.show_hidden) {
            continue;
        }

        SequenceRow row;
        row.index = sequence.index;
        row.label = sequence.label;
        row.activity = sequence.activity;
        if (filter.by_activity && !sequence.activity.empty()) {
            row.display = sequence.activity;
        } else {
            row.display = sequence.label;
        }
        if (row.display.empty()) {
            row.display = "sequence " + std::to_string(sequence.index);
        }
        row.fps = sequence.fps;
        row.frames = sequence.frames;
        row.duration = sequence.duration;
        row.looping = sequence.looping;
        row.hidden = sequence.hidden;
        rows.push_back(std::move(row));
    }

    if (filter.alphabetical) {
        std::stable_sort(rows.begin(), rows.end(),
                         [](const SequenceRow& left, const SequenceRow& right) {
                             return ascii_lower(left.display) < ascii_lower(right.display);
                         });
    }
    return rows;
}

float clamp_animation_speed(float speed) {
    return std::max(kAnimationSpeedMin, std::min(kAnimationSpeedMax, speed));
}

std::string format_duration(float seconds) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f s", static_cast<double>(seconds));
    return std::string(buffer);
}

std::string format_frame_status(const bridge::AnimState& state,
                                const bridge::ModelScene& scene) {
    const bridge::ModelSequence* sequence = sequence_for_state(state, scene);
    const int frames = sequence != nullptr ? sequence->frames : 0;
    const float fps = sequence != nullptr ? sequence->fps : 0.0f;
    const long frame = std::lround(state.frame);

    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "frame %ld / %d - %.1f fps", frame, frames,
                  static_cast<double>(fps));
    std::string text(buffer);

    if (state.ground_speed != 0.0f) {
        std::snprintf(buffer, sizeof(buffer), " - %.1f u/s",
                      static_cast<double>(state.ground_speed));
        text += buffer;
    }
    if (state.paused) {
        text += " (paused)";
    }
    return text;
}

} // namespace ui
