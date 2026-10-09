#include "ui/settings.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>

namespace ui {

UISettings UISettings::load(const std::filesystem::path& file) {
    UISettings settings;
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return settings;
    }

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);

        if (key == "show_advanced") {
            settings.show_advanced = value == "1" || value == "true";
        } else if (key == "last_bin_dir") {
            settings.last_bin_dir = value;
        } else if (key == "last_game_dir") {
            settings.last_game_dir = value;
        } else if (key == "preview_shape") {
            settings.preview_shape = value;
        } else if (key == "preview_environment") {
            settings.preview_environment = value;
        } else if (key == "preview_rig") {
            settings.preview_rig = value;
        } else if (key == "browser_root") {
            settings.browser_root = value;
        } else if (key == "browser_scan_game") {
            settings.browser_scan_game = value == "1" || value == "true";
        } else if (key == "thumbnail_flip") {
            settings.thumbnail_flip = value == "1" || value == "true";
        } else if (key == "recent") {
            if (!value.empty() && settings.recent_files.size() < kMaxRecentFiles) {
                settings.recent_files.push_back(value);
            }
        }
    }
    return settings;
}

bool UISettings::save(const std::filesystem::path& file) const {
    if (file.empty()) {
        return false;
    }

    std::filesystem::path temp = file;
    temp += L".tmp";

    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }

        stream << "show_advanced=" << (show_advanced ? 1 : 0) << "\n";
        stream << "last_bin_dir=" << last_bin_dir << "\n";
        stream << "last_game_dir=" << last_game_dir << "\n";
        stream << "preview_shape=" << preview_shape << "\n";
        stream << "preview_environment=" << preview_environment << "\n";
        stream << "preview_rig=" << preview_rig << "\n";
        stream << "browser_root=" << browser_root << "\n";
        stream << "browser_scan_game=" << (browser_scan_game ? 1 : 0) << "\n";
        stream << "thumbnail_flip=" << (thumbnail_flip ? 1 : 0) << "\n";
        for (const std::string& recent : recent_files) {
            stream << "recent=" << recent << "\n";
        }
        stream.close();
        if (!stream) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
    }

    // Rename over the target so a crash cannot truncate the live file. On
    // Windows rename fails when the target exists, hence the remove + retry.
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        ec.clear();
        std::filesystem::rename(temp, file, ec);
    }
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }
    return true;
}

void UISettings::remember_file(const std::string& path) {
    recent_files.erase(std::remove(recent_files.begin(), recent_files.end(), path),
                       recent_files.end());
    recent_files.insert(recent_files.begin(), path);
    if (recent_files.size() > kMaxRecentFiles) {
        recent_files.resize(kMaxRecentFiles);
    }
}

} // namespace ui
