#pragma once

// ui main window - private interface for the Win32 application shell.
// Everything Valve-free lives in the public ui headers; this one is only
// included by ui sources.

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
#include <shellapi.h>

#include <string>

namespace ui {

// Creates the main window and runs the message loop. Returns the process
// exit code.
int run_app(HINSTANCE instance, int show_command);

// Hidden compatibility probe mode (spawned as a child process by the preview
// flow): initializes the engine from the given folders, renders one frame and
// shuts down. Returns 0 on success, non-zero on failure or crash, so a faulty
// game install cannot take the viewer down with it.
int run_engine_probe(const std::wstring& bin_dir, const std::wstring& game_dir);

} // namespace ui
