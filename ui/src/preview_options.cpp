#include "ui/preview_options.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ui {
namespace {

float clampf(float value, float lo, float hi) {
    if (!(value == value)) { // NaN
        return lo;
    }
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

// Direction the light travels, from a rig position. Azimuth 0 puts the light
// at the viewer (+z), elevation raises it; the travel direction points from
// the light towards the origin.
void direction_from_angles(float azimuth_deg, float elevation_deg, float out[3]) {
    constexpr float kPi = 3.14159265358979323846f;
    const float az = azimuth_deg * kPi / 180.0f;
    const float el = elevation_deg * kPi / 180.0f;
    const float ce = std::cos(el);
    // Object -> light unit vector.
    const float lx = ce * std::sin(az);
    const float ly = std::sin(el);
    const float lz = ce * std::cos(az);
    out[0] = -lx;
    out[1] = -ly;
    out[2] = -lz;
}

PreviewRig clamp_rig(PreviewRig rig) {
    rig.ambient = clampf(rig.ambient, 0.0f, 1.0f);
    rig.key_azimuth = clampf(rig.key_azimuth, -180.0f, 180.0f);
    rig.key_elevation = clampf(rig.key_elevation, -90.0f, 90.0f);
    rig.key_intensity = clampf(rig.key_intensity, 0.0f, 2.0f);
    rig.background = clampf(rig.background, 0.0f, 1.0f);
    return rig;
}

std::string join_rig(const PreviewRig& rig) {
    char buffer[160] = {};
    std::snprintf(buffer, sizeof(buffer), "%.4f %.4f %.4f %.4f %.4f", rig.ambient,
                  rig.key_azimuth, rig.key_elevation, rig.key_intensity, rig.background);
    return buffer;
}

bool parse_floats(const std::string& text, std::vector<float>& out) {
    out.clear();
    const char* cursor = text.c_str();
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '\0') break;
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor) {
            return false;
        }
        out.push_back(value);
        cursor = end;
    }
    return !out.empty();
}

} // namespace

EnvironmentSetup environment_setup(PreviewEnvironment environment) {
    EnvironmentSetup setup;
    switch (environment) {
    case PreviewEnvironment::StudioDark: {
        setup.rig.ambient = 0.30f;
        setup.rig.background = 0.16f;
        setup.rig.key_intensity = 1.0f;
        setup.lighting.ambient[0] = 0.30f;
        setup.lighting.ambient[1] = 0.30f;
        setup.lighting.ambient[2] = 0.32f;
        setup.lighting.key_color[0] = 1.0f;
        setup.lighting.key_color[1] = 1.0f;
        setup.lighting.key_color[2] = 1.0f;
        setup.lighting.fill_color[0] = 0.22f;
        setup.lighting.fill_color[1] = 0.22f;
        setup.lighting.fill_color[2] = 0.26f;
        setup.lighting.background[0] = 0.16f;
        setup.lighting.background[1] = 0.16f;
        setup.lighting.background[2] = 0.17f;
        break;
    }
    case PreviewEnvironment::StudioLight: {
        setup.rig.ambient = 0.55f;
        setup.rig.background = 0.62f;
        setup.rig.key_intensity = 0.85f;
        setup.lighting.ambient[0] = 0.55f;
        setup.lighting.ambient[1] = 0.55f;
        setup.lighting.ambient[2] = 0.55f;
        setup.lighting.key_color[0] = 0.95f;
        setup.lighting.key_color[1] = 0.95f;
        setup.lighting.key_color[2] = 0.95f;
        setup.lighting.fill_color[0] = 0.35f;
        setup.lighting.fill_color[1] = 0.35f;
        setup.lighting.fill_color[2] = 0.35f;
        setup.lighting.background[0] = 0.62f;
        setup.lighting.background[1] = 0.62f;
        setup.lighting.background[2] = 0.64f;
        break;
    }
    case PreviewEnvironment::Outdoor: {
        setup.rig.ambient = 0.38f;
        setup.rig.background = 0.45f;
        setup.rig.key_intensity = 1.4f;
        setup.rig.key_azimuth = 55.0f;
        setup.rig.key_elevation = 55.0f;
        setup.lighting.ambient[0] = 0.32f;
        setup.lighting.ambient[1] = 0.38f;
        setup.lighting.ambient[2] = 0.48f;
        setup.lighting.key_color[0] = 1.0f;
        setup.lighting.key_color[1] = 0.96f;
        setup.lighting.key_color[2] = 0.86f;
        setup.lighting.fill_color[0] = 0.24f;
        setup.lighting.fill_color[1] = 0.28f;
        setup.lighting.fill_color[2] = 0.36f;
        setup.lighting.background[0] = 0.42f;
        setup.lighting.background[1] = 0.48f;
        setup.lighting.background[2] = 0.58f;
        break;
    }
    case PreviewEnvironment::Flat: {
        setup.rig.ambient = 0.9f;
        setup.rig.background = 0.5f;
        setup.rig.key_intensity = 0.0f;
        setup.lighting.ambient[0] = 0.9f;
        setup.lighting.ambient[1] = 0.9f;
        setup.lighting.ambient[2] = 0.9f;
        setup.lighting.key_color[0] = 0.0f;
        setup.lighting.key_color[1] = 0.0f;
        setup.lighting.key_color[2] = 0.0f;
        setup.lighting.fill_color[0] = 0.0f;
        setup.lighting.fill_color[1] = 0.0f;
        setup.lighting.fill_color[2] = 0.0f;
        setup.lighting.background[0] = 0.5f;
        setup.lighting.background[1] = 0.5f;
        setup.lighting.background[2] = 0.5f;
        break;
    }
    case PreviewEnvironment::Custom:
        break;
    }
    direction_from_angles(setup.rig.key_azimuth, setup.rig.key_elevation,
                          setup.lighting.key_dir);
    direction_from_angles(setup.rig.key_azimuth - 150.0f, 15.0f, setup.lighting.fill_dir);
    return setup;
}

void apply_environment(PreviewOptions& options, PreviewEnvironment environment) {
    if (environment == PreviewEnvironment::Custom) {
        return;
    }
    const EnvironmentSetup setup = environment_setup(environment);
    options.environment = environment;
    options.rig = setup.rig;
    options.lighting = setup.lighting;
}

PreviewLighting lighting_from_rig(const PreviewRig& rig_in) {
    const PreviewRig rig = clamp_rig(rig_in);
    PreviewLighting lighting;
    const float ambient = rig.ambient;
    for (int i = 0; i < 3; ++i) {
        lighting.ambient[i] = ambient;
    }
    direction_from_angles(rig.key_azimuth, rig.key_elevation, lighting.key_dir);
    for (int i = 0; i < 3; ++i) {
        lighting.key_color[i] = rig.key_intensity;
    }
    direction_from_angles(rig.key_azimuth - 150.0f, 15.0f, lighting.fill_dir);
    lighting.fill_color[0] = 0.22f;
    lighting.fill_color[1] = 0.22f;
    lighting.fill_color[2] = 0.26f;
    for (int i = 0; i < 3; ++i) {
        lighting.background[i] = rig.background;
    }
    return lighting;
}

void apply_rig(PreviewOptions& options, const PreviewRig& rig) {
    options.environment = PreviewEnvironment::Custom;
    options.rig = clamp_rig(rig);
    options.lighting = lighting_from_rig(options.rig);
}

const char* shape_id(PreviewShape shape) {
    switch (shape) {
    case PreviewShape::Sphere:
        return "sphere";
    case PreviewShape::Cube:
        return "cube";
    case PreviewShape::Cylinder:
        return "cylinder";
    case PreviewShape::Plane:
        break;
    }
    return "plane";
}

bool parse_shape(const std::string& id, PreviewShape& out) {
    if (id == "sphere") {
        out = PreviewShape::Sphere;
        return true;
    }
    if (id == "cube") {
        out = PreviewShape::Cube;
        return true;
    }
    if (id == "cylinder") {
        out = PreviewShape::Cylinder;
        return true;
    }
    if (id == "plane") {
        out = PreviewShape::Plane;
        return true;
    }
    return false;
}

const char* environment_id(PreviewEnvironment environment) {
    switch (environment) {
    case PreviewEnvironment::StudioDark:
        return "studio_dark";
    case PreviewEnvironment::StudioLight:
        return "studio_light";
    case PreviewEnvironment::Outdoor:
        return "outdoor";
    case PreviewEnvironment::Flat:
        return "flat";
    case PreviewEnvironment::Custom:
        break;
    }
    return "custom";
}

bool parse_environment(const std::string& id, PreviewEnvironment& out) {
    if (id == "studio_dark") {
        out = PreviewEnvironment::StudioDark;
        return true;
    }
    if (id == "studio_light") {
        out = PreviewEnvironment::StudioLight;
        return true;
    }
    if (id == "outdoor") {
        out = PreviewEnvironment::Outdoor;
        return true;
    }
    if (id == "flat") {
        out = PreviewEnvironment::Flat;
        return true;
    }
    if (id == "custom") {
        out = PreviewEnvironment::Custom;
        return true;
    }
    return false;
}

PreviewOptions options_from_settings(const UISettings& settings) {
    PreviewOptions options;
    PreviewShape shape;
    if (parse_shape(settings.preview_shape, shape)) {
        options.shape = shape;
    }

    // Restore the rig first; the environment preset then overwrites it unless
    // the persisted state is a hand-edited (custom) rig.
    {
        std::vector<float> values;
        if (parse_floats(settings.preview_rig, values) && values.size() >= 5) {
            options.rig.ambient = clampf(values[0], 0.0f, 1.0f);
            options.rig.key_azimuth = clampf(values[1], -180.0f, 180.0f);
            options.rig.key_elevation = clampf(values[2], -90.0f, 90.0f);
            options.rig.key_intensity = clampf(values[3], 0.0f, 2.0f);
            options.rig.background = clampf(values[4], 0.0f, 1.0f);
        }
    }

    PreviewEnvironment environment;
    if (parse_environment(settings.preview_environment, environment) &&
        environment != PreviewEnvironment::Custom) {
        apply_environment(options, environment);
    } else {
        options.environment = PreviewEnvironment::Custom;
        options.lighting = lighting_from_rig(options.rig);
    }
    return options;
}

void options_to_settings(const PreviewOptions& options, UISettings& settings) {
    settings.preview_shape = shape_id(options.shape);
    settings.preview_environment = environment_id(options.environment);
    settings.preview_rig = join_rig(options.rig);
}

} // namespace ui
