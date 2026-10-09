#pragma once

// game_profiles - JSON profiles: interface version strings, DLL names and
// paths per game.
//
// Phase 1 skeleton: placeholder public header. Profile loading arrives later.

namespace profiles {

// The concrete declarations arrive in a later phase. For now the module exposes
// a single symbol so the static library has a well-defined translation unit.
const char* game_profiles_version_string() noexcept;

} // namespace profiles
