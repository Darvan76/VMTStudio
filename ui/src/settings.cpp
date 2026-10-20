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
        } else if (key == "window_width") {
            try { settings.window_width = std::stoi(value); } catch (...) { }
        } else if (key == "window_height") {
            try { settings.window_height = std::stoi(value); } catch (...) { }
        } else if (key == "splitter_sizes") {
            settings.splitter_sizes = value;
        } else if (key == "validation_dock_visible") {
            settings.validation_dock_visible = value == "1" || value == "true";
        } else if (key == "last_model") {
            settings.last_model = value;
        } else if (key == "asset_kind") {
            try { settings.asset_kind = std::stoi(value); } catch (...) { }
        } else if (key == "asset_source") {
            try { settings.asset_source = std::stoi(value); } catch (...) { }
        } else if (key == "asset_grid") {
            settings.asset_grid = value == "1" || value == "true";
        } else if (key == "asset_sort") {
            settings.asset_sort = value;
        } else if (key == "asset_folders") {
            if (!value.empty()) {
                settings.asset_folders.push_back(value);
            }
        } else if (key == "asset_geometry") {
            settings.asset_geometry = value;
        } else if (key == "ignored_update_version") {
            settings.ignored_update_version = value;
        } else if (key == "check_for_updates") {
            settings.check_for_updates = value == "1" || value == "true";
        } else if (key == "recent") {
            if (!value.empty() && settings.recent_files.size() < kMaxRecentFiles) {
                settings.recent_files.push_back(value);
            }
        } else if (key == "recent_models") {
            if (!value.empty() && settings.recent_models.size() < kMaxRecentFiles) {
                settings.recent_models.push_back(value);
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
        stream << "window_width=" << window_width << "\n";
        stream << "window_height=" << window_height << "\n";
        stream << "splitter_sizes=" << splitter_sizes << "\n";
        stream << "validation_dock_visible=" << (validation_dock_visible ? 1 : 0) << "\n";
        stream << "last_model=" << last_model << "\n";
        stream << "asset_kind=" << asset_kind << "\n";
        stream << "asset_source=" << asset_source << "\n";
        stream << "asset_grid=" << (asset_grid ? 1 : 0) << "\n";
        stream << "asset_sort=" << asset_sort << "\n";
        stream << "asset_geometry=" << asset_geometry << "\n";
        for (const std::string& folder : asset_folders) {
            stream << "asset_folders=" << folder << "\n";
        }
        stream << "ignored_update_version=" << ignored_update_version << "\n";
        stream << "check_for_updates=" << (check_for_updates ? 1 : 0) << "\n";
        for (const std::string& recent : recent_files) {
            stream << "recent=" << recent << "\n";
        }
        for (const std::string& model : recent_models) {
            stream << "recent_models=" << model << "\n";
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

void UISettings::remember_model(const std::string& path) {
    recent_models.erase(std::remove(recent_models.begin(), recent_models.end(), path),
                        recent_models.end());
    recent_models.insert(recent_models.begin(), path);
    if (recent_models.size() > kMaxRecentFiles) {
        recent_models.resize(kMaxRecentFiles);
    }
}

} // namespace ui
