#pragma once

// game_manager - high-level registry of installed Source Engine game profiles
// discovered on disk (Crowbar-style auto-detection). This is the glue between
// the low-level `game_profiles` JSON definitions and the UI connection flow.
//
// Discovery is purely filesystem based and never throws; profiles that cannot
// be fully resolved are still returned with empty paths so the UI can report
// the failure instead of crashing.

#include "profiles/install_check.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace profiles {

struct GameProfile {
    std::string id;          // stable key, usually the game folder name
    std::string name;         // human readable (from gameinfo.txt, fallback: folder name)
    std::string engine_branch; // e.g. "source_2013", "gmod", "unknown"
    std::filesystem::path game_dir; // folder containing gameinfo.txt
    std::filesystem::path bin_dir;  // folder containing tier0.dll / vstdlib.dll
    std::string gameinfo_game;       // "game" value from gameinfo.txt
    bool is_valid = false;          // same as install.usable (all required engine DLLs usable)
    InstallReport install;          // why the install is (not) usable: problem + detail
};

class GameManager {
public:
    GameManager() = default;

    // Replace the current catalogue with every Source game discovered under
    // `root`. `max_depth` caps recursion so a whole Steam library root can be
    // scanned cheaply.
    void discover(const std::filesystem::path& root, int max_depth = 4);

    void clear() noexcept;

    const std::vector<GameProfile>& profiles() const noexcept { return profiles_; }

    std::vector<GameProfile>& profiles() noexcept { return profiles_; }

    // Returns pointer to the matching profile or nullptr.
    GameProfile* find(std::string_view id) noexcept;
    const GameProfile* find(std::string_view id) const noexcept;

    // Persist the last used profile id (simple in-memory helper; callers can
    // layer real settings on top).
    void set_last_used(std::string_view id) noexcept { last_used_ = std::string(id); }
    const std::string& last_used() const noexcept { return last_used_; }

    // --- filesystem detection helpers (single source of truth; the UI layer
    // delegates to these so Crowbar-style detection behaves identically in the
    // catalogue scan and the connection dialog) ---

    // True when `path` directly contains both tier0.dll and vstdlib.dll, the
    // minimal marker set of a Source Engine /bin folder.
    static bool looks_like_bin_dir(const std::filesystem::path& path) noexcept;

    // Best-effort Source /bin folder detection: a directory that directly
    // contains tier0.dll + vstdlib.dll is returned as-is, a Source module
    // folder (gameinfo.txt) resolves to its own "bin" or the "bin" beside it,
    // and a game root / SDK root / Steam library resolves through its "bin"
    // child or its immediate children (sorted, first match). Empty path when
    // nothing looks like a Source /bin folder.
    static std::filesystem::path detect_bin_dir(const std::filesystem::path& selected);

    // Best-effort game folder detection from a /bin folder: the parent
    // directory when it directly contains gameinfo.txt, otherwise the first
    // immediate subdirectory (sorted) that contains one. Empty path when
    // nothing is found.
    static std::filesystem::path detect_game_dir(const std::filesystem::path& bin_dir);

private:
    std::vector<GameProfile> profiles_;
    std::string last_used_;

    static std::string read_game_name(const std::filesystem::path& gameinfo_path) noexcept;
    static std::string detect_engine_branch(const std::filesystem::path& game_root,
                                            const std::filesystem::path& bin_dir) noexcept;

    void scan(const std::filesystem::path& root, int depth);
};

} // namespace profiles