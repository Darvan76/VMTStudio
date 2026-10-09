#pragma once

// engine_bridge - loads the game's own DLLs from /bin via CreateInterface,
// mounts the search paths from gameinfo.txt and exposes the material system.
//
// Phase 1 skeleton: placeholder public header. No DLL is loaded yet.
//
// IMPORTANT: this public header must stay Valve-free. All Valve headers and
// types belong in src/ private headers only.

namespace bridge {

// The concrete declarations arrive in a later phase. For now the module exposes
// a single symbol so the static library has a well-defined translation unit.
const char* engine_bridge_version_string() noexcept;

} // namespace bridge
