#include "vmt/value.h"

#include "internal.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vmt {

namespace {

std::string invalid_msg(std::string_view what, std::string_view text) {
    std::string out(what);
    out += ": '";
    out += text;
    out += "'";
    return out;
}

} // namespace

Result<int> parse_int(std::string_view text) {
    const std::string_view t = detail::trim(text);
    if (t.empty()) {
        return Result<int>(make_error(ErrorCode::InvalidArgument, "not a valid integer: ''"));
    }
    int v = 0;
    const char* b = t.data();
    const char* e = t.data() + t.size();
    const auto [ptr, ec] = std::from_chars(b, e, v);
    if (ec != std::errc() || ptr != e) {
        return Result<int>(make_error(ErrorCode::InvalidArgument, invalid_msg("not a valid integer", text)));
    }
    return Ok(v);
}

Result<float> parse_float(std::string_view text) {
    const std::string_view t = detail::trim(text);
    if (t.empty()) {
        return Result<float>(make_error(ErrorCode::InvalidArgument, "not a valid float: ''"));
    }
    char buf[64];
    const char* str = nullptr;
    std::string fallback;
    if (t.size() < sizeof(buf)) {
        std::memcpy(buf, t.data(), t.size());
        buf[t.size()] = '\0';
        str = buf;
    } else {
        fallback.assign(t.data(), t.size());
        str = fallback.c_str();
    }
    char* end = nullptr;
    const float v = std::strtof(str, &end);
    if (end != str + t.size()) {
        return Result<float>(make_error(ErrorCode::InvalidArgument, invalid_msg("not a valid float", text)));
    }
    return Ok(v);
}

Result<bool> parse_bool(std::string_view text) {
    const std::string_view t = detail::trim(text);
    switch (t.size()) {
        case 1:
            if (t[0] == '1') return Ok(true);
            if (t[0] == '0') return Ok(false);
            break;
        case 2:
            if (detail::iequals(t, "on")) return Ok(true);
            if (detail::iequals(t, "no")) return Ok(false);
            break;
        case 3:
            if (detail::iequals(t, "yes")) return Ok(true);
            break;
        case 4:
            if (detail::iequals(t, "true")) return Ok(true);
            break;
        case 5:
            if (detail::iequals(t, "false")) return Ok(false);
            break;
        default:
            break;
    }
    return Result<bool>(make_error(ErrorCode::InvalidArgument, invalid_msg("not a valid bool", text)));
}

Result<Vec3> parse_vec3(std::string_view text) {
    std::string_view t = detail::trim(text);
    if (!t.empty() && t.front() == '[') t.remove_prefix(1);
    if (!t.empty() && t.back() == ']') t.remove_suffix(1);
    const auto toks = detail::split_ws(t);
    if (toks.size() != 3) {
        return Result<Vec3>(make_error(ErrorCode::InvalidArgument, invalid_msg("vector requires 3 components", text)));
    }
    auto x = parse_float(toks[0]); if (!x) return Result<Vec3>(x.error());
    auto y = parse_float(toks[1]); if (!y) return Result<Vec3>(y.error());
    auto z = parse_float(toks[2]); if (!z) return Result<Vec3>(z.error());
    return Ok(Vec3{ x.value(), y.value(), z.value() });
}

Result<Color> parse_color(std::string_view text) {
    std::string_view t = detail::trim(text);
    if (!t.empty() && t.front() == '{') t.remove_prefix(1);
    if (!t.empty() && t.back() == '}') t.remove_suffix(1);
    const auto toks = detail::split_ws(t);
    if (toks.size() != 3 && toks.size() != 4) {
        return Result<Color>(make_error(ErrorCode::InvalidArgument, invalid_msg("color requires 3 or 4 components", text)));
    }
    auto r = parse_int(toks[0]); if (!r) return Result<Color>(r.error());
    auto g = parse_int(toks[1]); if (!g) return Result<Color>(g.error());
    auto b = parse_int(toks[2]); if (!b) return Result<Color>(b.error());
    int a = 255;
    if (toks.size() == 4) {
        auto pa = parse_int(toks[3]); if (!pa) return Result<Color>(pa.error());
        a = pa.value();
    }
    return Ok(Color{ r.value(), g.value(), b.value(), a });
}

Result<Matrix> parse_matrix(std::string_view text) {
    const auto toks = detail::split_ws(detail::trim(text));
    Matrix m;
    bool any = false;
    std::size_t i = 0;
    while (i < toks.size()) {
        if (detail::iequals(toks[i], "center")) {
            if (i + 2 >= toks.size()) return Result<Matrix>(make_error(ErrorCode::InvalidArgument, invalid_msg("matrix 'center' needs 2 values", text)));
            auto a = parse_float(toks[i + 1]); if (!a) return Result<Matrix>(a.error());
            auto b = parse_float(toks[i + 2]); if (!b) return Result<Matrix>(b.error());
            m.center[0] = a.value(); m.center[1] = b.value(); i += 3; any = true;
        } else if (detail::iequals(toks[i], "scale")) {
            if (i + 2 >= toks.size()) return Result<Matrix>(make_error(ErrorCode::InvalidArgument, invalid_msg("matrix 'scale' needs 2 values", text)));
            auto a = parse_float(toks[i + 1]); if (!a) return Result<Matrix>(a.error());
            auto b = parse_float(toks[i + 2]); if (!b) return Result<Matrix>(b.error());
            m.scale[0] = a.value(); m.scale[1] = b.value(); i += 3; any = true;
        } else if (detail::iequals(toks[i], "rotate")) {
            if (i + 1 >= toks.size()) return Result<Matrix>(make_error(ErrorCode::InvalidArgument, invalid_msg("matrix 'rotate' needs 1 value", text)));
            auto a = parse_float(toks[i + 1]); if (!a) return Result<Matrix>(a.error());
            m.rotate = a.value(); i += 2; any = true;
        } else if (detail::iequals(toks[i], "translate")) {
            if (i + 2 >= toks.size()) return Result<Matrix>(make_error(ErrorCode::InvalidArgument, invalid_msg("matrix 'translate' needs 2 values", text)));
            auto a = parse_float(toks[i + 1]); if (!a) return Result<Matrix>(a.error());
            auto b = parse_float(toks[i + 2]); if (!b) return Result<Matrix>(b.error());
            m.translate[0] = a.value(); m.translate[1] = b.value(); i += 3; any = true;
        } else {
            return Result<Matrix>(make_error(ErrorCode::InvalidArgument, invalid_msg("unknown matrix keyword", toks[i])));
        }
    }
    if (!any) {
        return Result<Matrix>(make_error(ErrorCode::InvalidArgument, invalid_msg("empty matrix", text)));
    }
    return Ok(m);
}

std::string format_int(int value) {
    return std::to_string(value);
}

std::string format_float(float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(value));
    return std::string(buf);
}

std::string format_bool(bool value) {
    return value ? "1" : "0";
}

std::string format_vec3(const Vec3& v) {
    return "[" + format_float(v.x) + " " + format_float(v.y) + " " + format_float(v.z) + "]";
}

std::string format_color(const Color& c) {
    if (c.a == 255) {
        return "{" + std::to_string(c.r) + " " + std::to_string(c.g) + " " + std::to_string(c.b) + "}";
    }
    return "{" + std::to_string(c.r) + " " + std::to_string(c.g) + " " + std::to_string(c.b) + " " + std::to_string(c.a) + "}";
}

std::string format_matrix(const Matrix& m) {
    return "center " + format_float(m.center[0]) + " " + format_float(m.center[1]) +
           " scale " + format_float(m.scale[0]) + " " + format_float(m.scale[1]) +
           " rotate " + format_float(m.rotate) +
           " translate " + format_float(m.translate[0]) + " " + format_float(m.translate[1]);
}

} // namespace vmt
