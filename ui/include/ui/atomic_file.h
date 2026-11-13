#pragma once

#include "vmt/result.h"

#include <filesystem>
#include <string_view>

namespace ui {

// Writes a unique sibling temporary file, flushes it, then replaces the target
// in one filesystem operation. Failure never deletes or truncates the target.
// The parent directory must already exist. Reparse-point targets are rejected.
vmt::Result<void> atomic_write_file(const std::filesystem::path& target,
                                   std::string_view bytes);

} // namespace ui
