#pragma once

// Internal shared helpers for vmt_core. Not installed; included by the .cpp
// translation units of this module only.

#include "vmt/span.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vmt {
namespace detail {

inline bool is_hspace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? char(c + ('a' - 'A')) : c;
}

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

inline std::string_view trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

inline std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        const std::size_t start = i;
        while (i < s.size() && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// 1-based line/column for a byte offset.
inline SourceLocation loc_of(const std::string& s, std::size_t off) {
    SourceLocation loc{ 1, 1 };
    const std::size_t n = off < s.size() ? off : s.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (s[i] == '\n') { ++loc.line; loc.column = 1; }
        else { ++loc.column; }
    }
    return loc;
}

// Decode a (possibly) quoted string literal. If raw is unterminated we still
// strip a leading/trailing quote where present, so recovery keeps working.
inline std::string decode_quoted(std::string_view raw) {
    std::string_view body = raw;
    if (!body.empty() && body.front() == '"') body.remove_prefix(1);
    if (!body.empty() && body.back() == '"') body.remove_suffix(1);

    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            const char d = body[i + 1];
            switch (d) {
                case 'n':  out.push_back('\n'); ++i; break;
                case 't':  out.push_back('\t'); ++i; break;
                case 'r':  out.push_back('\r'); ++i; break;
                case '"':  out.push_back('"');  ++i; break;
                case '\\': out.push_back('\\'); ++i; break;
                default:   out.push_back(c); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace detail
} // namespace vmt
