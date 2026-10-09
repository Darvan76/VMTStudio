#pragma once

// Internal strict JSON parser used by the schema loader. Not installed, not
// part of the public API. Parses RFC 8259 JSON (UTF-8 input, BOM tolerated)
// and reports 1-based line/column errors.

#include "vmt/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vmt {
namespace detail {

struct JsonValue {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> items;                            // Array elements
    std::vector<std::pair<std::string, JsonValue>> members;  // Object members, file order preserved
    std::uint32_t offset = 0;                                // byte offset of the value's first token

    // Object member by exact key; nullptr when absent or not an object.
    const JsonValue* find(std::string_view key) const noexcept;

    // "null" "boolean" "number" "string" "array" "object".
    const char* kind_name() const noexcept;
};

// Parse and validate one JSON document. source_name is copied into any
// returned Error (error.file).
Result<JsonValue> parse_json(std::string_view text, std::string source_name);

} // namespace detail
} // namespace vmt
