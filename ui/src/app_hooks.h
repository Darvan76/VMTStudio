#pragma once

// app_hooks - the seam between the main window (which owns the editor session,
// the engine and the settings file) and the auxiliary Win32 windows (material
// browser, preview options). All calls happen on the UI thread; the engine is
// only touched while the main window exists.

#include "ui/preview_options.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace ui {
namespace app {

// --- engine ------------------------------------------------------------------

bool engine_ready();

// Starts the engine when it is not running yet (compatibility probe, device
// creation, preview-path mount, preview options). Returns false and fills
// `error` when the connection is missing or the engine cannot start.
bool ensure_engine(std::wstring& error);

// Enumerates the game's .vmt materials through the engine file system
// (VPK-aware). Fails with a message when the engine cannot be started.
bool game_materials(std::vector<std::string>& names, bool& truncated, std::wstring& error);

// Reads one material through the engine file system. `material_name` is
// game-relative without extension ("brick/brickwall001a").
bool game_material_text(const std::string& material_name, std::string& text,
                        std::wstring& error);

// Opens a material (display form accepted: "brick/brickwall001a" or with
// ".vmt") in the editor and starts the preview. Returns false + message when
// the material cannot be read.
bool open_game_material(const std::string& material_name, std::wstring& error);

// Opens a .vmt file from disk in the editor (used by the browser for
// folder-scan entries; works without a game connection). Returns false on
// failure after showing the error.
bool open_material_file(const std::filesystem::path& path);

// Renders a thumbnail with the engine and caches it as a BMP. On success
// `bmp_path` is the cache file. Re-rendering overwrites the cache.
bool render_thumbnail(const std::string& material_name, std::filesystem::path& bmp_path,
                      std::wstring& error);

// Engine pixel readback orientation: some game binaries return rows bottom-up.
// The browser exposes this as a checkbox and persists it in the settings.
bool thumbnail_flip();
void set_thumbnail_flip(bool flip);

// <local app data>/VMTViewer/thumbnails. Empty when no location exists.
std::filesystem::path thumbnail_root();

// --- browser persistence -----------------------------------------------------

// Last folder the browser scanned (empty when the last scan was the game file
// system or nothing was scanned yet).
std::string last_browser_root();

// Remembers the browser source in the settings file. `root` is empty for a
// game-file-system scan.
void remember_browser_source(bool game_scan, const std::filesystem::path& root);

// --- preview options ---------------------------------------------------------

PreviewOptions preview_options();

// Applies to the running engine (no-op when the engine is stopped) and stores
// the options in memory. The caller persists them via commit_preview_options()
// once the user stops interacting.
void apply_preview_options(const PreviewOptions& options);

// Writes preview options to the settings file (atomic).
void commit_preview_options();

// Opens a .mdl file picker and asks the engine to load the model. Pending the
// StudioRender headers, this reports the limitation in a message box.
void pick_and_load_model(HWND owner);

} // namespace app
} // namespace ui
