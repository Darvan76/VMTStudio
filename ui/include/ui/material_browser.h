#pragma once

#include <string>
#include <string_view>

namespace ui {

// Converts a game path to the material name accepted by the engine: forward
// slashes, no leading "materials/" (including redundant prefixes), and no
// .vmt/.vtf extension. Prefix and extension matching is case-insensitive;
// the remaining spelling is preserved.
std::string material_name_from_game_path(std::string_view game_path);

} // namespace ui
