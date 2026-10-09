#pragma once

// ui text encoding - maps a VMT document's byte buffer to the Win32 text
// control and back without corrupting files that are not valid UTF-8.
//
// VMT files are usually ASCII or UTF-8. When the bytes are not valid UTF-8 the
// control shows them through the process ANSI code page, and editing writes
// them back through the same code page, so untouched bytes roundtrip exactly.
// (Characters typed that the ANSI code page cannot represent are replaced by
// '?'; that limitation is inherent to the file's encoding.)

#include <string>

namespace ui {

enum class TextEncoding : unsigned char {
    Utf8,
    Ansi,
};

// Strict UTF-8 validation (no replacement characters).
bool is_valid_utf8(const std::string& bytes);

// Decodes a document buffer for display and reports which encoding was used.
// The returned encoding must be passed back to encode_from_display().
std::wstring decode_for_display(const std::string& bytes, TextEncoding& encoding);

// Encodes control text back into document bytes using the given encoding.
std::string encode_from_display(const std::wstring& wide, TextEncoding encoding);

} // namespace ui
