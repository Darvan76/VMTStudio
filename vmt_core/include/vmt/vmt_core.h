#pragma once

// vmt_core - lossless VMT parser, document model, serializer, typed value
// helpers and schema loader.
//
// This module has zero dependency on the engine, on the UI and on Windows
// APIs, so it can be unit tested alone.

#include "vmt/error.h"
#include "vmt/result.h"
#include "vmt/span.h"
#include "vmt/value.h"
#include "vmt/document.h"
#include "vmt/schema.h"

namespace vmt {

// Module identity; also gives the static library a guaranteed symbol.
const char* vmt_core_version_string() noexcept;

} // namespace vmt
