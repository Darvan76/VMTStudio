#include "ui/text_encoding.h"

#include <windows.h>

#include <cstddef>

namespace ui {

bool is_valid_utf8(const std::string& bytes) {
    if (bytes.empty()) {
        return true;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    return length > 0;
}

std::wstring decode_for_display(const std::string& bytes, TextEncoding& encoding) {
    if (bytes.empty()) {
        encoding = TextEncoding::Utf8;
        return std::wstring();
    }

    const int utf8_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                                static_cast<int>(bytes.size()), nullptr, 0);
    if (utf8_length > 0) {
        encoding = TextEncoding::Utf8;
        std::wstring wide(static_cast<std::size_t>(utf8_length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                            static_cast<int>(bytes.size()), wide.data(), utf8_length);
        return wide;
    }

    encoding = TextEncoding::Ansi;
    const int ansi_length = MultiByteToWideChar(CP_ACP, 0, bytes.data(),
                                                static_cast<int>(bytes.size()), nullptr, 0);
    if (ansi_length <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(ansi_length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(),
                        ansi_length);
    return wide;
}

std::string encode_from_display(const std::wstring& wide, TextEncoding encoding) {
    if (wide.empty()) {
        return std::string();
    }
    const UINT code_page = encoding == TextEncoding::Utf8 ? CP_UTF8 : CP_ACP;
    const int length = WideCharToMultiByte(code_page, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) {
        return std::string();
    }
    std::string bytes(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(code_page, 0, wide.data(), static_cast<int>(wide.size()), bytes.data(),
                        length, nullptr, nullptr);
    return bytes;
}

} // namespace ui
