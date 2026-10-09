#pragma once

// ui settings - tiny key=value store for recent files, the last game /bin
// directory, preview options and view toggles. Windows-free so it can be unit
// tested; the per-user location is decided by the caller.
//
// File format (UTF-8, one entry per line, unknown keys are ignored):
//   show_advanced=1
//   last_bin_dir=D:\Games\GarrysMod\bin
//   recent=D:\maps\dev.vmt          (repeated, most recent first)
//   preview_shape=sphere
//   preview_environment=studio_dark
//   preview_rig=0.3000 38.0000 42.0000 1.0000 0.1600
//   browser_root=D:\Games\GarrysMod\garrysmod\materials
//
// Saves are crash-safe: the file is written next to its target and renamed
// over it only after the bytes are on disk, so a crash mid-write cannot leave
// a truncated settings file behind.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ui {

struct UISettings {
    static constexpr std::size_t kMaxRecentFiles = 10;

    std::vector<std::string> recent_files; // most recent first
    std::string last_bin_dir;
    std::string last_game_dir; // folder containing gameinfo.txt
    bool show_advanced = false;

    // Phase 9 preview options (parsed by preview_options.cpp).
    std::string preview_shape = "sphere";
    std::string preview_environment = "studio_dark";
    std::string preview_rig; // "ambient azimuth elevation intensity background"

    // Material browser.
    std::string browser_root;         // last scanned folder
    bool browser_scan_game = false;   // last scan source: true = game file system
    bool thumbnail_flip = true;       // engine readback rows are bottom-up

    // Missing or unreadable files yield defaults; a malformed line is ignored.
    static UISettings load(const std::filesystem::path& file);

    // Atomic (write-then-rename) save. Returns false when the file cannot be
    // written.
    bool save(const std::filesystem::path& file) const;

    // Moves the path to the front, removing duplicates, capped at the maximum.
    void remember_file(const std::string& path);
};

} // namespace ui
