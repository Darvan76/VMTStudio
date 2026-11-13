#pragma once

#include "vmt/schema.h"

namespace ui {
namespace qt {

// The same validated, embedded schema library is shared by the inspector and
// asset previews. Loading errors retain their resource path and source location.
const vmt::Result<vmt::SchemaLibrary>& bundled_schemas();

// A copy for callers that own their schema library (such as MainWindow).
vmt::Result<vmt::SchemaLibrary> load_bundled_schemas();

} // namespace qt
} // namespace ui
