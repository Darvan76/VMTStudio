#pragma once

// ui preview options dialog - modal "Preview options" window. Private to
// ui/src.
//
// Shape radio buttons, an environment preset combo and a light rig of sliders.
// Every change is applied to the engine immediately (when it is running) so
// the viewport updates live; the settings file is written once on close.

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

void show_preview_options_dialog(HWND owner);

} // namespace ui
