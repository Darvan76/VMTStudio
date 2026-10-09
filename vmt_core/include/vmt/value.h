#pragma once

#include "vmt/result.h"

#include <string>
#include <string_view>

namespace vmt {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool operator==(const Vec3& o) const noexcept { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3& o) const noexcept { return !(*this == o); }
};

struct Color {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 255;
    bool operator==(const Color& o) const noexcept { return r == o.r && g == o.g && b == o.b && a == o.a; }
    bool operator!=(const Color& o) const noexcept { return !(*this == o); }
};

// "$basetexturetransform" style value: center x y scale x y rotate r translate x y.
struct Matrix {
    float center[2]    = { 0.0f, 0.0f };
    float scale[2]     = { 1.0f, 1.0f };
    float rotate       = 0.0f;
    float translate[2] = { 0.0f, 0.0f };
    bool operator==(const Matrix& o) const noexcept {
        return center[0] == o.center[0] && center[1] == o.center[1] &&
               scale[0] == o.scale[0] && scale[1] == o.scale[1] &&
               rotate == o.rotate &&
               translate[0] == o.translate[0] && translate[1] == o.translate[1];
    }
    bool operator!=(const Matrix& o) const noexcept { return !(*this == o); }
};

// Typed value parsing/formatting. These are pure and engine-free.

Result<int>    parse_int(std::string_view text);
Result<float>  parse_float(std::string_view text);
Result<bool>   parse_bool(std::string_view text);
Result<Vec3>   parse_vec3(std::string_view text);   // "[1 0 0]" or "1 0 0"
Result<Color>  parse_color(std::string_view text);  // "{255 255 255}" or "{r g b a}"
Result<Matrix> parse_matrix(std::string_view text); // "center .5 .5 scale 1 1 rotate 0 translate 0 0"

std::string format_int(int value);
std::string format_float(float value);
std::string format_bool(bool value);
std::string format_vec3(const Vec3& v);      // "[1 0 0]"
std::string format_color(const Color& c);    // "{255 255 255}" (4th component only when a != 255)
std::string format_matrix(const Matrix& m);

} // namespace vmt
