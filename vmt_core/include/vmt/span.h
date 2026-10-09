#pragma once

#include <cstdint>

namespace vmt {

// Byte range into a document's source buffer (offsets are byte offsets).
struct TextSpan {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    bool empty() const noexcept { return length == 0; }
    std::uint32_t end() const noexcept { return offset + length; }
    bool contains(TextSpan other) const noexcept {
        return other.offset >= offset && other.end() <= end();
    }
};

struct SourceLocation {
    std::uint32_t line = 0;   // 1-based; 0 = unknown
    std::uint32_t column = 0; // 1-based; 0 = unknown
};

} // namespace vmt
