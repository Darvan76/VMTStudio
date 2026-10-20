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

// Parses a "models" dump. Unknown rows are ignored; malformed rows fail the
// whole parse (the backend is the only writer, so a broken row is a bug and
// silent data loss would be worse).
vmt::Result<ModelList> parse_models_dump(const std::string& text);

// Parses an "assets" dump (files under the requested roots, loose or VPK).
vmt::Result<AssetList> parse_assets_dump(const std::string& text);

// Parses a "model_scene" dump.
vmt::Result<ModelScene> parse_model_scene_dump(const std::string& text);

// Parses an "anim_state" dump.
vmt::Result<AnimState> parse_anim_state_dump(const std::string& text);

} // namespace bridge
