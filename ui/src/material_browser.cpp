#include "ui/material_browser.h"

#include <cctype>

namespace ui {
namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    return to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

std::string path_to_slashes(std::string s) {
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    return s;
}

std::string strip_vmt_extension(std::string s) {
    if (ends_with_ci(s, ".vmt") || ends_with_ci(s, ".vtf")) {
        s.resize(s.size() - 4);
    }
    return s;
}

} // namespace

std::string material_name_from_game_path(std::string_view game_path) {
    std::string path = path_to_slashes(std::string(game_path));
    const std::string prefix = "materials/";
    // The prefix is matched case-insensitively: loose folders are usually
    // lowercase, but "Materials/" appears in the wild too.
    while (path.size() >= prefix.size() &&
           to_lower(path.substr(0, prefix.size())) == prefix) {
        path = path.substr(prefix.size());
    }
    if (path.empty()) {
        return std::string();
    }
    return strip_vmt_extension(path);
}

} // namespace ui
