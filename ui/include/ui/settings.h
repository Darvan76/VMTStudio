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
//   asset_kind=-1
//   asset_source=0
//   asset_grid=0
//   asset_sort=0,asc
//   asset_folders=D:\mods\my_addon\materials     (repeated)
//   asset_geometry=...                           (base64 QWidget::saveGeometry)
//
// The model viewer reuses the same store:
//   last_model=models/props_c17/oildrum001.mdl
//   recent_models=models/props_c17/oildrum001.mdl   (repeated, most recent first)
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

    // Material browser (legacy keys; kept so existing settings files round-trip).
    std::string browser_root;         // last scanned folder
    bool browser_scan_game = false;   // last scan source: true = game file system
    bool thumbnail_flip = true;       // engine readback rows are bottom-up

    // Asset Browser (F3/F4). Additive keys: older settings files simply lack
    // them and get the defaults below. `asset_kind` is a ui::AssetKind value or
    // -1 for "All"; `asset_source` is the source combo index (0..3).
    int asset_kind = -1;
    int asset_source = 0;
    bool asset_grid = false;
    std::string asset_sort = "0,asc"; // "<column>,asc|desc"
    std::vector<std::string> asset_folders; // user folders, most recent first
    std::string asset_geometry;       // QWidget::saveGeometry(), base64

    // Window / layout persistence.
    int window_width = 0;
    int window_height = 0;
    std::string splitter_sizes;        // comma-separated splitter pane sizes
    bool validation_dock_visible = false;

    // Model viewer: the last loaded model and the recent-models list. Entries
    // are either game-relative .mdl paths or absolute disk paths.
    std::string last_model;
    std::vector<std::string> recent_models; // most recent first

    // Application updater configuration
    std::string ignored_update_version; // version tag to suppress notification for (e.g. "v0.1.1")
    bool check_for_updates = true;      // check GitHub releases on startup

    // Missing or unreadable files yield defaults; a malformed line is ignored.
    static UISettings load(const std::filesystem::path& file);

    // Atomic (write-then-rename) save. Returns false when the file cannot be
    // written.
    bool save(const std::filesystem::path& file) const;

    // Moves the path to the front, removing duplicates, capped at the maximum.
    void remember_file(const std::string& path);

    // Same, for the recent-models list.
    void remember_model(const std::string& path);
};

} // namespace ui
