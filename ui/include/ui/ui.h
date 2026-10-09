#pragma once

// ui - main window, parameter panel, raw text editor, material browser.
//
// Phase 9 layout (Win32 native):
//   left    - file commands and recent files
//   center  - engine viewport (shapes, light rig, environment presets)
//   right   - parameter panel generated from the shader schema, grouped, with a
//             "Show advanced" toggle and an "Unrecognized" group for parameters
//             the schema does not describe
//   bottom  - raw VMT text editor
//   browser - modeless Material browser (tree, search, engine-rendered
//             thumbnails cached on disk)
//
// The visual panel and the text editor share one vmt::Document through
// EditorSession; edits made in either place update the other immediately and
// serialization stays lossless (vmt_core guarantees byte-preserving edits).
//
// The editing core (session, panel model, settings, preview options, material
// index) is Windows-free and unit tested; the Win32 shell lives in ui/src.

#include "ui/connection_model.h"
#include "ui/material_browser.h"
#include "ui/panel_model.h"
#include "ui/preview_options.h"
#include "ui/session.h"
#include "ui/settings.h"
#include "ui/text_encoding.h"

namespace ui {

// Module identity; also gives the static library a guaranteed symbol.
const char* ui_version_string() noexcept;

} // namespace ui
