#include "profiles/game_manager.h"

#include "vmt/document.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
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

bool iless(std::string_view lhs, std::string_view rhs) noexcept {
    return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](char a, char b) noexcept {
            return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a))) <
                   static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b)));
        });
}

struct GameInfo {
    std::string name;
    std::string app_id;
};

GameInfo read_game_info(const std::filesystem::path& gameinfo_path) noexcept {
    try {
        std::ifstream input(gameinfo_path, std::ios::in | std::ios::binary);
        if (!input) {
            return {};
        }
        const auto parsed = vmt::Document::parse(
            std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
        if (!parsed) {
            return {};
        }
        GameInfo info;
        const auto& document = parsed.value();
        if (const auto* game = document.find("game")) {
            info.name = game->value();
        }
        if (const auto* filesystem = document.find_block("FileSystem")) {
            for (const auto& node : filesystem->children()) {
                if (node.is_parameter() && lowercase(std::string(node.key())) == "steamappid") {
                    info.app_id = node.value();
                    break;
                }
            }
        }
        return info;
    } catch (...) {
        return {};
    }
}

std::string detect_engine_branch(const GameInfo& info,
                                const std::filesystem::path& game_root,
                                const std::filesystem::path& bin_dir) {
    const std::string lower_name = lowercase(info.name);
    // L4D2 uses its own Source branch; its DLL ABI is not SDK 2013's.
    if (info.app_id == "550" || lower_name == "left 4 dead 2" ||
        lowercase(game_root.filename().string()) == "left4dead2") {
        return "left4dead2";
    }
    if (info.app_id == "4000" || info.app_id == "4001" ||
        lower_name.find("garry") != std::string::npos ||
        lower_name.find("gmod") != std::string::npos) {
        return "gmod";
    }
    if (!bin_dir.empty() && regular_file_exists(bin_dir / "tier0.dll")) {
        return "source_2013";
    }
    return "unknown";
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
                const std::filesystem::path& entry_path = entry.path();
                if (looks_like_bin_dir(entry_path)) {
                    matches.push_back(entry_path);
                } else if (looks_like_bin_dir(entry_path / "bin")) {
                    matches.push_back(entry_path / "bin");
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
        const std::filesystem::path& entry_path = entry.path();
        if (entry.is_directory(entry_ec) &&
            std::filesystem::is_regular_file(entry_path / "gameinfo.txt", entry_ec)) {
            candidates.push_back(entry_path);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.empty() ? std::filesystem::path() : candidates.front();
}

void GameManager::discover(const std::filesystem::path& root, int max_depth,
                           const std::atomic<bool>* cancelled) {
    clear();
    if (max_depth < 0) {
        max_depth = 0;
    }
    scan(root, max_depth, cancelled);
    if (cancelled && cancelled->load()) {
        clear();
        return;
    }

    std::sort(profiles_.begin(), profiles_.end(),
              [](const GameProfile& left, const GameProfile& right) noexcept {
                  return iless(left.name, right.name);
              });
}

void GameManager::clear() noexcept {
    profiles_.clear();
    last_used_.clear();
}

GameProfile* GameManager::find(std::string_view id) noexcept {
    const auto it = std::find_if(profiles_.begin(), profiles_.end(),
                                 [id](const GameProfile& profile) noexcept {
                                     return profile.id == id;
                                 });
    return it != profiles_.end() ? &(*it) : nullptr;
}

const GameProfile* GameManager::find(std::string_view id) const noexcept {
    const auto it = std::find_if(profiles_.begin(), profiles_.end(),
                                 [id](const GameProfile& profile) noexcept {
                                     return profile.id == id;
                                 });
    return it != profiles_.end() ? &(*it) : nullptr;
}

void GameManager::scan(const std::filesystem::path& root, int depth,
                       const std::atomic<bool>* cancelled) {
    if (depth < 0 || root.empty() || (cancelled && cancelled->load())) {
        return;
    }
    std::error_code ec;
    if (!directory_exists(root)) {
        return;
    }

    if (regular_file_exists(root / "gameinfo.txt")) {
        const bool duplicate = std::any_of(
            profiles_.begin(), profiles_.end(),
            [&root](const GameProfile& existing) noexcept {
                return existing.game_dir == root;
            });
        if (!duplicate) {
            GameProfile profile;
            profile.game_dir = root;
            profile.id = root.filename().string();
            const GameInfo info = read_game_info(root / "gameinfo.txt");
            profile.gameinfo_game = info.name;
            profile.name = profile.gameinfo_game.empty() ? profile.id : profile.gameinfo_game;
            profile.bin_dir = detect_bin_dir(root);
            profile.engine_branch = detect_engine_branch(info, root, profile.bin_dir);
            // Full install validation: the report explains WHY a profile is
            // not usable (empty folder, missing DLLs, wrong arch, ...).
            profile.install = check_install(profile);
            profile.is_valid = profile.install.usable;
            profiles_.push_back(std::move(profile));
        }
    }

    if (depth == 0) {
        return;
    }

    for (std::filesystem::directory_iterator it(root, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (cancelled && cancelled->load()) return;
        std::error_code entry_ec;
        if (it->is_directory(entry_ec) && !entry_ec) {
            scan(it->path(), depth - 1, cancelled);
        }
    }
}

} // namespace profiles
