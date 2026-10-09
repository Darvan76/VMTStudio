#pragma once

// ui connection dialog - modal "Game connection" window. Private to ui/src.
//
// Lets the user pick the game's /bin folder, shows the detected architecture
// and the per-DLL connection report from the real engine_bridge loader, and
// remembers the last choice in the settings file.
//
// Game name detection is pending: game_profiles has no public API yet.

#include "ui/settings.h"

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

namespace ui {

void show_connection_dialog(HWND owner, UISettings& settings,
                            const std::filesystem::path& settings_path);

} // namespace ui
