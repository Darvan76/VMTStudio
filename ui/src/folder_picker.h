#pragma once

// folder_picker - the Windows Explorer folder chooser. Private to ui/src.
//
// Replaces SHBrowseForFolderW, which opens a legacy tree widget inside a plain
// dialog. IFileOpenDialog in pick-folders mode opens the real shell view
// instead: address bar, breadcrumb navigation, search box and a working
// "New folder" button, i.e. the same dialog Explorer itself uses.
//
// The helper owns COM apartment initialisation, so callers do not have to. A
// cancel (and any COM failure) is reported as an empty path; it never throws.

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

// Shows the Explorer folder chooser. `title` is the dialog caption.
//
// `start_in` is the folder the dialog should open in. When it names an
// existing directory the dialog starts there; when it names something else (a
// typed path that does not exist, for instance) its parent is used instead, and
// the shell's own "last used" location is the final fallback.
//
// Returns the chosen folder, or an empty path when the user cancels.
std::filesystem::path pick_folder(HWND owner, const wchar_t* title,
                                  const std::filesystem::path& start_in = {});

} // namespace ui