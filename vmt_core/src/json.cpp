#include "json.h"

#include "internal.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vmt {
namespace detail {

namespace {

constexpr int k_max_depth = 64;

bool is_json_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code <= 0x7F) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

bool is_cont_byte(unsigned char b) {
    return b >= 0x80 && b <= 0xBF;
}

// Byte length of a structurally valid UTF-8 sequence at pos, or 0 when the
// bytes are malformed: bad lead/continuation byte, overlong form, an encoded
// surrogate, or a code point above U+10FFFF.
std::size_t utf8_sequence_length(std::string_view text, std::size_t pos) {
    const unsigned char b0 = static_cast<unsigned char>(text[pos]);
    const std::size_t size = text.size();
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        return (pos + 1 < size && is_cont_byte(static_cast<unsigned char>(text[pos + 1]))) ? 2 : 0;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (pos + 2 >= size) return 0;
        const unsigned char b1 = static_cast<unsigned char>(text[pos + 1]);
        const unsigned char b2 = static_cast<unsigned char>(text[pos + 2]);
        if (b0 == 0xE0) {
            if (b1 < 0xA0 || b1 > 0xBF) return 0; // overlong
        } else if (b0 == 0xED) {
            if (b1 < 0x80 || b1 > 0x9F) return 0; // encoded surrogate
        } else if (!is_cont_byte(b1)) {
            return 0;
        }
        return is_cont_byte(b2) ? 3 : 0;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (pos + 3 >= size) return 0;
        const unsigned char b1 = static_cast<unsigned char>(text[pos + 1]);
        const unsigned char b2 = static_cast<unsigned char>(text[pos + 2]);
        const unsigned char b3 = static_cast<unsigned char>(text[pos + 3]);
        if (b0 == 0xF0) {
            if (b1 < 0x90 || b1 > 0xBF) return 0; // overlong
        } else if (b0 == 0xF4) {
            if (b1 < 0x80 || b1 > 0x8F) return 0; // > U+10FFFF
        } else if (!is_cont_byte(b1)) {
            return 0;
        }
        return (is_cont_byte(b2) && is_cont_byte(b3)) ? 4 : 0;
    }
    return 0;
}

std::string escape_message(char c) {
    std::string message = "invalid escape sequence '\\";
    const unsigned char value = static_cast<unsigned char>(c);
    if (value >= 0x20 && value < 0x7F) {
        message.push_back(c);
    } else {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "x%02X", static_cast<unsigned int>(value));
        message.append(buffer);
    }
    message.push_back('\'');
    return message;
}

JsonValue make_null(std::size_t offset) {
    JsonValue value;
    value.kind = JsonValue::Kind::Null;
    value.offset = static_cast<std::uint32_t>(offset);
    return value;
}

JsonValue make_bool(bool boolean, std::size_t offset) {
    JsonValue value;
    value.kind = JsonValue::Kind::Bool;
    value.boolean = boolean;
    value.offset = static_cast<std::uint32_t>(offset);
    return value;
}

JsonValue make_string(std::string text, std::size_t offset) {
    JsonValue value;
    value.kind = JsonValue::Kind::String;
    value.string = std::move(text);
    value.offset = static_cast<std::uint32_t>(offset);
    return value;
}

// Tracks container nesting for the depth limit; decrements on every return.
class DepthGuard {
public:
    explicit DepthGuard(int& depth) : depth_(depth) { ++depth_; }
    ~DepthGuard() { --depth_; }

    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;

private:
    int& depth_;
};

class Parser {
public:
    Parser(std::string_view text, std::string source)
        : text_(text), source_(std::move(source)) {
        if (text_.size() >= 3 &&
            static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    Result<JsonValue> parse_document() {
        Result<JsonValue> value = parse_value();
        if (!value) return value;
        skip_ws();
        if (pos_ != text_.size()) {
            return Result<JsonValue>(error_at(pos_, "unexpected trailing content"));
        }
        return value;
    }

private:
    void skip_ws() {
        while (pos_ < text_.size() && is_json_ws(text_[pos_])) ++pos_;
    }

    Error error_at(std::size_t offset, std::string message) const {
        const std::string copy(text_);
        const SourceLocation location = loc_of(copy, offset);
        return make_error(ErrorCode::ParseError, std::move(message), {}, source_, location.line, location.column);
    }

    bool match_literal(std::string_view literal) {
        if (text_.compare(pos_, literal.size(), literal) != 0) return false;
        pos_ += literal.size();
        return true;
    }

    Result<JsonValue> parse_value() {
        skip_ws();
        const std::size_t start = pos_;
        if (start >= text_.size()) {
            return Result<JsonValue>(error_at(start, "unexpected end of input"));
        }
        const char c = text_[start];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') {
            Result<std::string> decoded = parse_string();
            if (!decoded) return Result<JsonValue>(decoded.error());
            return Ok(make_string(std::move(decoded.value()), start));
        }
        if (c == 't') {
            if (match_literal("true")) return Ok(make_bool(true, start));
            return Result<JsonValue>(error_at(start, "expected value"));
        }
        if (c == 'f') {
            if (match_literal("false")) return Ok(make_bool(false, start));
            return Result<JsonValue>(error_at(start, "expected value"));
        }
        if (c == 'n') {
            if (match_literal("null")) return Ok(make_null(start));
            return Result<JsonValue>(error_at(start, "expected value"));
        }
        if (c == '-' || is_digit(c)) return parse_number();
        return Result<JsonValue>(error_at(start, "expected value"));
    }

    Result<JsonValue> parse_object() {
        JsonValue object;
        object.kind = JsonValue::Kind::Object;
        object.offset = static_cast<std::uint32_t>(pos_);
        if (depth_ >= k_max_depth) {
            return Result<JsonValue>(error_at(object.offset, "maximum nesting depth (64) exceeded"));
        }
        DepthGuard guard(depth_);
        ++pos_; // '{'
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return Ok(std::move(object));
        }
        for (;;) {
            skip_ws();
            if (pos_ >= text_.size()) {
                return Result<JsonValue>(error_at(object.offset, "unterminated object"));
            }
            const std::size_t key_offset = pos_;
            if (text_[pos_] != '"') {
                return Result<JsonValue>(error_at(pos_, "expected '\"' to start object key"));
            }
            Result<std::string> key = parse_string();
            if (!key) return Result<JsonValue>(key.error());
            for (const auto& member : object.members) {
                if (member.first == key.value()) {
                    return Result<JsonValue>(error_at(key_offset, "duplicate object key \"" + key.value() + "\""));
                }
            }
            skip_ws();
            if (pos_ >= text_.size()) {
                return Result<JsonValue>(error_at(object.offset, "unterminated object"));
            }
            if (text_[pos_] != ':') {
                return Result<JsonValue>(error_at(pos_, "expected ':' after object key"));
            }
            ++pos_;
            Result<JsonValue> value = parse_value();
            if (!value) return value;
            object.members.emplace_back(std::move(key.value()), std::move(value.value()));
            skip_ws();
            if (pos_ >= text_.size()) {
                return Result<JsonValue>(error_at(object.offset, "unterminated object"));
            }
            const char c = text_[pos_];
            if (c == ',') {
                ++pos_;
                skip_ws();
                if (pos_ < text_.size() && text_[pos_] == '}') {
                    return Result<JsonValue>(error_at(pos_, "invalid trailing comma in object"));
                }
                continue;
            }
            if (c == '}') {
                ++pos_;
                return Ok(std::move(object));
            }
            return Result<JsonValue>(error_at(pos_, "expected ',' or '}' in object"));
        }
    }

    Result<JsonValue> parse_array() {
        JsonValue array;
        array.kind = JsonValue::Kind::Array;
        array.offset = static_cast<std::uint32_t>(pos_);
        if (depth_ >= k_max_depth) {
            return Result<JsonValue>(error_at(array.offset, "maximum nesting depth (64) exceeded"));
        }
        DepthGuard guard(depth_);
        ++pos_; // '['
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return Ok(std::move(array));
        }
        for (;;) {
            Result<JsonValue> value = parse_value();
            if (!value) return value;
            array.items.push_back(std::move(value.value()));
            skip_ws();
            if (pos_ >= text_.size()) {
                return Result<JsonValue>(error_at(array.offset, "unterminated array"));
            }
            const char c = text_[pos_];
            if (c == ',') {
                ++pos_;
                skip_ws();
                if (pos_ < text_.size() && text_[pos_] == ']') {
                    return Result<JsonValue>(error_at(pos_, "invalid trailing comma in array"));
                }
                continue;
            }
            if (c == ']') {
                ++pos_;
                return Ok(std::move(array));
            }
            return Result<JsonValue>(error_at(pos_, "expected ',' or ']' in array"));
        }
    }

    Result<std::string> parse_string() {
        const std::size_t start = pos_; // at opening quote
        ++pos_;
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) {
                return Result<std::string>(error_at(start, "unterminated string"));
            }
            const unsigned char byte = static_cast<unsigned char>(text_[pos_]);
            if (byte == '"') {
                ++pos_;
                return Ok(std::move(out));
            }
            if (byte < 0x20) {
                return Result<std::string>(error_at(pos_, "unescaped control character in string"));
            }
            if (byte != '\\') {
                if (byte < 0x80) {
                    out.push_back(static_cast<char>(byte));
                    ++pos_;
                } else {
                    const std::size_t length = utf8_sequence_length(text_, pos_);
                    if (length == 0) {
                        return Result<std::string>(error_at(pos_, "invalid UTF-8 sequence"));
                    }
                    out.append(text_.data() + pos_, length);
                    pos_ += length;
                }
                continue;
            }
            const std::size_t escape = pos_;
            ++pos_;
            if (pos_ >= text_.size()) {
                return Result<std::string>(error_at(start, "unterminated string"));
            }
            const char escaped = text_[pos_];
            switch (escaped) {
                case '"':  out.push_back('"');  ++pos_; break;
                case '\\': out.push_back('\\'); ++pos_; break;
                case '/':  out.push_back('/');  ++pos_; break;
                case 'b':  out.push_back('\b'); ++pos_; break;
                case 'f':  out.push_back('\f'); ++pos_; break;
                case 'n':  out.push_back('\n'); ++pos_; break;
                case 'r':  out.push_back('\r'); ++pos_; break;
                case 't':  out.push_back('\t'); ++pos_; break;
                case 'u': {
                    ++pos_;
                    std::uint32_t code = 0;
                    if (!parse_hex4(code)) {
                        return Result<std::string>(error_at(escape, "invalid unicode escape"));
                    }
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                            return Result<std::string>(error_at(escape, "invalid unicode surrogate pair"));
                        }
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return Result<std::string>(error_at(escape, "invalid unicode surrogate pair"));
                        }
                        code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                        append_utf8(out, code);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return Result<std::string>(error_at(escape, "invalid unicode surrogate pair"));
                    } else {
                        append_utf8(out, code);
                    }
                    break;
                }
                default:
                    return Result<std::string>(error_at(escape, escape_message(escaped)));
            }
        }
    }

    bool parse_hex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) return false;
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const int digit = hex_value(text_[pos_ + i]);
            if (digit < 0) return false;
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        pos_ += 4;
        out = value;
        return true;
    }

    Result<JsonValue> parse_number() {
        const std::size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size()) {
            return Result<JsonValue>(error_at(start, "invalid number"));
        }
        if (text_[pos_] == '0') {
            ++pos_;
            if (pos_ < text_.size() && is_digit(text_[pos_])) {
                return Result<JsonValue>(error_at(start, "invalid number"));
            }
        } else if (is_digit(text_[pos_])) {
            while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
        } else {
            return Result<JsonValue>(error_at(start, "invalid number"));
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                return Result<JsonValue>(error_at(start, "invalid number"));
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || !is_digit(text_[pos_])) {
                return Result<JsonValue>(error_at(start, "invalid number"));
            }
            while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
        }
        const std::string_view token = text_.substr(start, pos_ - start);
        double number = 0.0;
        const char* const begin = token.data();
        const char* const end = token.data() + token.size();
        const std::from_chars_result conversion = std::from_chars(begin, end, number);
        if (conversion.ec != std::errc() || conversion.ptr != end || !std::isfinite(number)) {
            return Result<JsonValue>(error_at(start, "invalid number"));
        }
        JsonValue value;
        value.kind = JsonValue::Kind::Number;
        value.number = number;
        value.offset = static_cast<std::uint32_t>(start);
        return Ok(std::move(value));
    }

    std::string_view text_;
    std::string source_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

} // namespace

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (kind != Kind::Object) return nullptr;
    for (const auto& member : members) {
        if (std::string_view(member.first) == key) return &member.second;
    }
    return nullptr;
}

const char* JsonValue::kind_name() const noexcept {
    switch (kind) {
        case Kind::Null:   return "null";
        case Kind::Bool:   return "boolean";
        case Kind::Number: return "number";
        case Kind::String: return "string";
        case Kind::Array:  return "array";
        case Kind::Object: return "object";
    }
    return "null";
}

Result<JsonValue> parse_json(std::string_view text, std::string source_name) {
    Parser parser(text, std::move(source_name));
    return parser.parse_document();
}

} // namespace detail
} // namespace vmt
