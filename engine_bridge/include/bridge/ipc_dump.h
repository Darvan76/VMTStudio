#pragma once

// ipc_dump - parser for the response files of the IPC protocol v2
// (see docs/IPC_PROTOCOL.md).
//
// The backend writes UTF-8 TSV files; this module turns them into the plain
// value types of engine_view.h. It is Windows-free so it can be unit tested
// without an engine connection.

#include "bridge/engine_view.h"

#include <string>

namespace bridge {

// Protocol version of the file ("VMTStudio-IPC\t2"); 0 when the header is
// missing or malformed. The parser refuses versions it does not know.
int dump_protocol_version(const std::string& text);

// RAW_PREFIX .bin: original file size (u64 little-endian), then exactly
// min(file size, requested maximum) bytes. Rejects invalid bounds/framing.
vmt::Result<BinaryPrefix> parse_binary_prefix(
    const std::vector<unsigned char>& payload, std::uint32_t max_bytes);

// Parses an "assets" dump (files under the requested roots, loose or VPK).
vmt::Result<AssetList> parse_assets_dump(const std::string& text);

// Parses an "exists" dump. A missing/malformed result is an error, never a
// negative lookup: the caller must distinguish unavailable VFS from absence.
vmt::Result<bool> parse_exists_dump(const std::string& text);

// Parses a "model_scene" dump.
vmt::Result<ModelScene> parse_model_scene_dump(const std::string& text);

} // namespace bridge
