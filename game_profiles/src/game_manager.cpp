#include "profiles/game_manager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <system_error>

namespace profiles {

namespace {

bool regular_file_exists(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

bool directory_exists(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

std::string lowercase(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

} // namespace

bool GameManager::looks_like_bin_dir(const std::filesystem::path& path) noexcept {
    return regular_file_exists(path / "tier0.dll") &&
           regular_file_exists(path / "vstdlib.dll");
}

std::filesystem::path GameManager::detect_bin_dir(
    const std::filesystem::path& selected) {
    if (selected.empty()) {
        return {};
    }
    std::error_code ec;

    // Direct /bin folder.
    if (looks_like_bin_dir(selected)) {
        return selected;
    }

    // A Source module folder (contains gameinfo.txt): its bin is either inside
    // the module or lives in the enclosing game root.
    if (regular_file_exists(selected / "gameinfo.txt")) {
        if (looks_like_bin_dir(selected / "bin")) {
            return selected / "bin";
        }
        const std::filesystem::path parent_bin = selected.parent_path() / "bin";
        if (looks_like_bin_dir(parent_bin)) {
            return parent_bin;
        }
    }

    // A game root / SDK root carrying /bin as a direct child.
    if (looks_like_bin_dir(selected / "bin")) {
        return selected / "bin";
    }

    // Look at immediate children, so the user can pick a Steam library root or
    // an SDK folder and still end up with the right bin.
    if (directory_exists(selected)) {
        std::vector<std::filesystem::path> matches;
        std::filesystem::directory_iterator iterator(selected, ec);
        if (!ec) {
            for (const std::filesystem::directory_entry& entry : iterator) {
                std::error_code entry_ec;
                if (!entry.is_directory(entry_ec)) {
                    continue;
                }
                if (looks_like_bin_dir(entry.path())) {
                    matches.push_back(entry.path());
                } else if (looks_like_bin_dir(entry.path() / "bin")) {
                    matches.push_back(entry.path() / "bin");
                }
            }
        }
        std::sort(matches.begin(), matches.end());
        if (!matches.empty()) {
            return matches.front();
        }
    }

    return {};
}

std::filesystem::path GameManager::detect_game_dir(
    const std::filesystem::path& bin_dir) {
    if (bin_dir.empty()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path root = bin_dir.parent_path();
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        return {};
    }

    if (std::filesystem::is_regular_file(root / "gameinfo.txt", ec)) {
        return root;
    }

    std::vector<std::filesystem::path> candidates;
    std::filesystem::directory_iterator iterator(root, ec);
    if (ec) {
        return {};
    }
    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec) &&
            std::filesystem::is_regular_file(entry.path() / "gameinfo.txt", entry_ec)) {
            candidates.push_back(entry.path());
        }
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.empty() ? std::filesystem::path() : candidates.front();
}

std::string GameManager::read_game_name(
    const std::filesystem::path& gameinfo_path) noexcept {
    std::ifstream input(gameinfo_path, std::ios::in | std::ios::binary);
    if (!input) {
        return {};
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    std::regex re("\"game\"\\s+\"([^\"]*)\"", std::regex::icase);
    std::smatch match;
    if (std::regex_search(content, match, re) && match.size() >= 2) {
        return match.str(1);
    }
    return {};
}

std::string GameManager::detect_engine_branch(
    const std::filesystem::path& game_root,
    const std::filesystem::path& bin_dir) noexcept {
    std::ifstream input(game_root / "gameinfo.txt", std::ios::in | std::ios::binary);
    std::string content;
    if (input) {
        content.assign((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    }

    const std::string lower_name = lowercase(read_game_name(game_root / "gameinfo.txt"));
    if (lower_name.find("garry") != std::string::npos ||
        lower_name.find("gmod") != std::string::npos) {
        return "gmod";
    }

    std::regex appid_re("SteamAppId\\s+(\\d+)", std::regex::icase);
    std::smatch match;
    if (std::regex_search(content, match, appid_re) && match.size() >= 2) {
        const std::string appid = match.str(1);
        // Garry's Mod uses Steam app id 4000.
        if (appid == "4000" || appid == "4001") {
            return "gmod";
        }
    }

    if (!bin_dir.empty() && regular_file_exists(bin_dir / "tier0.dll")) {
        return "source_2013";
    }
    return "unknown";
}

void GameManager::discover(const std::filesystem::path& root, int max_depth) {
    clear();
    if (max_depth < 0) {
        max_depth = 0;
    }
    scan(root, max_depth);

    std::sort(profiles_.begin(), profiles_.end(),
              [](const GameProfile& left, const GameProfile& right) {
                  return lowercase(left.name) < lowercase(right.name);
              });
}

void GameManager::clear() noexcept {
    profiles_.clear();
    last_used_.clear();
}

GameProfile* GameManager::find(std::string_view id) noexcept {
    for (GameProfile& profile : profiles_) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

const GameProfile* GameManager::find(std::string_view id) const noexcept {
    for (const GameProfile& profile : profiles_) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

void GameManager::scan(const std::filesystem::path& root, int depth) {
    if (depth < 0 || root.empty()) {
        return;
    }
    std::error_code ec;
    if (!directory_exists(root)) {
        return;
    }

    if (regular_file_exists(root / "gameinfo.txt")) {
        bool duplicate = false;
        for (const GameProfile& existing : profiles_) {
            if (existing.game_dir == root) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            GameProfile profile;
            profile.game_dir = root;
            profile.id = root.filename().string();
            profile.gameinfo_game = read_game_name(root / "gameinfo.txt");
            profile.name = profile.gameinfo_game.empty() ? profile.id : profile.gameinfo_game;
            profile.bin_dir = detect_bin_dir(root);
            profile.engine_branch = detect_engine_branch(root, profile.bin_dir);
            profile.is_valid = !profile.bin_dir.empty();
            profiles_.push_back(profile);
        }
    }

    if (depth == 0) {
        return;
    }

    for (std::filesystem::directory_iterator it(root, ec);
         !ec && it != std::filesystem::directory_iterator(); ++it) {
        std::error_code entry_ec;
        if (it->is_directory(entry_ec) && !entry_ec) {
            scan(it->path(), depth - 1);
        }
    }
}

} // namespace profiles