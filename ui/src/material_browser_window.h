#pragma once

// ui material browser window - modeless "Material browser". Private to ui/src.
//
// A search box, a tree of materials grouped by folder, and a thumbnail pane
// rendered by the engine and cached on disk. Sources:
//   - "Scan game" enumerates the connected game's mounted file system
//     (including VPKs) through the engine, and
//   - "Scan folder" walks a directory on disk for .vmt files.
// Double-click (or the Open button) loads the selected material into the main
// window, where edits flow back into the engine preview.

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

namespace ui {

// Creates the browser window (or brings the existing one to the front). The
// window is owned by `owner` and destroyed with it.
void show_material_browser(HWND owner);

// Destroys the browser window when it exists (called while the main window is
// closing).
void destroy_material_browser();

} // namespace ui
