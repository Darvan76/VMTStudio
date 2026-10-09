#pragma once

// ui about dialog - modal "About VMT Viewer" window. Private to ui/src.
//
// States the licensing/redistribution position explicitly: the application
// ships no Valve files and reads everything from the user's own game install.

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

void show_about_dialog(HWND owner);

} // namespace ui
