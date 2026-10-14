#pragma once

// ui preview_options - headless model for the engine preview settings:
// primitive shape, light rig and environment preset.
//
// Windows-free and unit tested. The Win32 dialogs edit this model; the shell
// converts the lighting to bridge::PreviewLighting for the engine. Values are
// persisted through UISettings so every option survives a restart (and a
// crash: settings writes are atomic).

#include "ui/settings.h"

#include <string>

namespace ui {

enum class PreviewShape {
    Sphere,
    Cube,
    Cylinder,
    Plane,
    BrushWall,
    BrushFloor,
    BrushCube,
    BrushDisplacement,
};

inline bool is_brush_shape(PreviewShape shape) noexcept {
    return shape == PreviewShape::BrushWall ||
           shape == PreviewShape::BrushFloor ||
           shape == PreviewShape::BrushCube ||
           shape == PreviewShape::BrushDisplacement;
}

inline bool is_model_shape(PreviewShape shape) noexcept {
    return !is_brush_shape(shape);
}

// "Environment" is a small set of one-click lighting setups, not a skybox:
// StudioDark is the neutral default, Outdoor mimics sunlight, Flat removes
// directional light so texture colors can be read without shading.
enum class PreviewEnvironment {
    StudioDark,
    StudioLight,
    Outdoor,
    Flat,
    Custom, // rig edited by hand; not offered as a preset in the UI
};

// Slider-friendly light rig. Azimuth 0 points at the viewer (light behind the
// camera), positive angles swing to the right; elevation is degrees above the
// horizon.
struct PreviewRig {
    float ambient = 0.30f;       // 0..1
    float key_azimuth = 38.0f;   // -180..180 degrees
    float key_elevation = 42.0f; // -90..90 degrees
    float key_intensity = 1.0f;  // 0..2
    float background = 0.16f;    // 0..1 gray level
};

// Effective engine-side lighting (colors, direction vectors).
struct PreviewLighting {
    float ambient[3] = { 0.30f, 0.30f, 0.32f };
    float key_dir[3] = { -0.42f, -0.70f, -0.58f };
    float key_color[3] = { 1.0f, 1.0f, 1.0f };
    float fill_dir[3] = { 0.55f, -0.25f, 0.60f };
    float fill_color[3] = { 0.22f, 0.22f, 0.26f };
    float background[3] = { 0.16f, 0.16f, 0.17f };
};

struct PreviewOptions {
    PreviewShape shape = PreviewShape::Sphere;
    PreviewEnvironment environment = PreviewEnvironment::StudioDark;
    PreviewRig rig;
    PreviewLighting lighting; // derived from environment or rig
};

// The rig and lighting a preset selects (lighting carries the preset's color
// tints; the rig keeps the sliders consistent when the user starts tweaking).
struct EnvironmentSetup {
    PreviewRig rig;
    PreviewLighting lighting;
};
EnvironmentSetup environment_setup(PreviewEnvironment environment);

// Sets environment + rig + lighting together. Custom is a no-op preset.
void apply_environment(PreviewOptions& options, PreviewEnvironment environment);

// Applies a manually edited rig: switches the environment to Custom and
// re-derives the lighting.
void apply_rig(PreviewOptions& options, const PreviewRig& rig);

// Neutral-color lighting for a rig.
PreviewLighting lighting_from_rig(const PreviewRig& rig);

// --- ids for persistence and the settings file -------------------------------
const char* shape_id(PreviewShape shape);
bool parse_shape(const std::string& id, PreviewShape& out);
const char* environment_id(PreviewEnvironment environment);
bool parse_environment(const std::string& id, PreviewEnvironment& out);

// UISettings <-> PreviewOptions. A malformed value keeps the default.
PreviewOptions options_from_settings(const UISettings& settings);
void options_to_settings(const PreviewOptions& options, UISettings& settings);

} // namespace ui
