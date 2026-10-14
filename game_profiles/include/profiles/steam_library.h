#pragma once

// steam_library - Crowbar-style Steam installation & library auto-detection.
//
// The detector locates the Steam installation without any user interaction:
//
//   1. the `STEAMPATH` environment variable,
//   2. the registry value `HKCU\Software\Valve\Steam\SteamPath`,
//   3. the well-known install locations under "C:\Program Files".
//
// It then expands every Steam library folder declared in
// `steamapps/libraryfolders.vdf` and hands each of them to
// GameManager::discover, so Source Engine games are catalogued exactly like a
// manual folder scan - just without the browsing.
//
// Everything here is best-effort: no function throws, every failure degrades
// to an empty result, and the root-dependent entry points accept an explicit
// steam root so they stay unit-testable on machines without Steam.

#include "profiles/game_manager.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace profiles {

class SteamDetector {
public:
    SteamDetector() = delete;

    // Locates the Steam installation root, or nullopt when Steam cannot be
    // found. Every candidate is validated as an existing directory before it
    // is returned.
    static std::optional<std::filesystem::path> find_steam_root() noexcept;

    // Parses `steam_root/steamapps/libraryfolders.vdf` (KeyValues text: lines
    // such as "path" "D:\\SteamLibrary" - both the per-folder blocks of the
    // current format and the flat numeric keys of the legacy one). Returns the
    // declared library roots de-duplicated (case-insensitive); falls back to
    // `{steam_root}` when the file is missing, unreadable or yields no paths.
    static std::vector<std::filesystem::path> parse_library_folders(
        const std::filesystem::path& steam_root) noexcept;

    // Same as discover_all, but for an explicit root: no environment or
    // registry lookups, so tests can pass a fixture directory.
    static std::vector<GameProfile> discover_all_in(
        const std::filesystem::path& steam_root,
        int max_depth_per_library = 4) noexcept;

    // Runs GameManager::discover on every known Steam library and returns the
    // de-duplicated union of the resulting profiles. Empty when Steam itself
    // is not installed.
    static std::vector<GameProfile> discover_all(
        int max_depth_per_library = 4) noexcept;
};

} // namespace profiles
