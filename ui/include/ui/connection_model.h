#pragma once

// ui connection model - tests a game /bin directory with the real
// engine_bridge loader and produces user-facing report lines.
//
// The loader loads the game DLLs into this process (they stay loaded); the
// model never throws and always reports per-DLL status.
//
// Game /bin detection is owned by game_profiles: `ui::detect_bin_dir` and
// `ui::detect_game_dir` are thin wrappers around the static helpers on
// `profiles::GameManager` so the UI and the catalogue share one implementation.

#include <filesystem>
#include <string>
#include <vector>

namespace ui {

struct ConnectionCheck {
    std::filesystem::path bin_dir;
    std::string architecture;        // "x86", "x64", "unknown"
    bool connected = false;          // every required DLL loaded with its expected export
    std::string summary;             // one-line overall status
    std::vector<std::string> lines;  // "[OK] name (arch, export)" / "[FAIL] name: error"
};

// Runs bridge::DllLoader against bin_dir using the DLL set the Phase 3 spike
// requires. A missing directory yields present=false reports for every DLL.
ConnectionCheck test_connection(const std::filesystem::path& bin_dir);

// Best-effort game folder detection from a /bin folder: the parent directory
// when it directly contains gameinfo.txt, otherwise the first immediate
// subdirectory (sorted) that contains one. Empty path when nothing is found.
std::filesystem::path detect_game_dir(const std::filesystem::path& bin_dir);

// Best-effort Source /bin folder detection from a game root or mod folder: a
// directory that directly contains tier0.dll + vstdlib.dll is returned as-is,
// otherwise the function looks for a "bin" child (or the "bin" folder beside a
// gameinfo.txt file). Also scans the immediate children of the selected folder
// (Steam library / SDK roots). Empty path when nothing looks like a Source
// /bin folder. Delegates to profiles::GameManager::detect_bin_dir.
std::filesystem::path detect_bin_dir(const std::filesystem::path& selected);

} // namespace ui
