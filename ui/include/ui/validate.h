#pragma once

// ui validate - static validation of a parsed VMT document (Phase 8).
//
// Rules and severities:
//   error   parse-error         recovered parse diagnostics (unterminated
//                               block/string, unexpected token, ...)
//   error   missing-texture     texture does not resolve through the engine
//                               file system (VPKs included)
//   warning unknown-shader      no schema covers the shader; parameter checks
//                               are skipped
//   warning unknown-parameter   parameter is not part of the shader's schema
//   warning type-mismatch       value does not parse as the schema type
//   warning range-mismatch      numeric value outside the schema min/max
//   warning missing-dependency  parameter needs an enabling parameter that is
//                               absent or disabled (e.g. $phongboost vs $phong)
//   warning duplicate-parameter same key appears twice in one block
//   warning path-backslash      texture path uses '\'
//   warning path-extension      texture path includes a ".vtf" extension
//   info    path-case           texture path contains uppercase characters
//   info    texture-check-skipped    no engine file system was available
//   info    schema-unavailable  schemas failed to load; schema rules skipped
//
// Unknown shader and unknown parameter are warnings by design: they never block
// saving or previewing. Findings carry 1-based line/column numbers so the UI
// can jump to them.

#include "vmt/document.h"
#include "vmt/schema.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

enum class Severity : std::uint8_t { Info, Warning, Error };

struct Finding {
    Severity     severity = Severity::Info;
    std::string  code;    // stable rule id (see the list above)
    std::string  message; // user-facing, single line
    std::uint32_t line = 0;   // 1-based; 0 = unknown
    std::uint32_t column = 0; // 1-based; 0 = unknown
};

// Result of resolving one texture reference (material-relative name, forward
// slashes, no extension) through the engine file system.
enum class TextureStatus : std::uint8_t {
    Found,
    Missing,
    Unavailable, // no engine file system; the check cannot be performed
};

// Resolver contract: `name` is normalized (trimmed, forward slashes, no .vtf
// extension, lowercased). Implementations look up "materials/<name>.vtf".
using TextureResolver = std::function<TextureStatus(const std::string& name)>;

// Suggester contract: when a texture is Missing, checks for similar texture
// files on disk / mounted paths (typos, edit distance, single/double letters, case)
// and returns the suggested replacement without "materials/" or ".vtf", or empty string.
using TextureSuggester = std::function<std::string(const std::string& name)>;

struct ValidationOptions {
    // Schema library used for shader/parameter/type/range checks. When null,
    // schema-based rules are skipped and one "schema-unavailable" info is
    // reported.
    const vmt::SchemaLibrary* schemas = nullptr;
    // Empty when validation runs without a preview engine connection: texture
    // existence is then reported as a single "texture-check-skipped" info,
    // unless material_roots provides disk paths to check.
    TextureResolver resolve_texture;
    // Callback to suggest a similar texture if resolve_texture reports Missing.
    TextureSuggester suggest_texture;
    // Optional disk roots (e.g. material_roots) searched for textures and typo suggestions.
    std::vector<std::filesystem::path> material_roots;
};

// Validates a parsed document. Findings are sorted by line (unknown lines
// last), then by column and severity. Deterministic.
std::vector<Finding> validate_document(const vmt::Document& document,
                                       const ValidationOptions& options);

// Parses inline VMT text and validates it. Test/helper convenience.
std::vector<Finding> validate_text(const std::string& text,
                                   const ValidationOptions& options);

// --- Typo detection & texture path resolution helpers ---

// Computes the Damerau-Levenshtein edit distance between two strings,
// counting insertions, deletions, substitutions, and adjacent transpositions (case-insensitive).
std::size_t texture_edit_distance(std::string_view a, std::string_view b);

// Returns true if two texture names (without path/extension) are considered
// similar (e.g. single/double letter typos, case, edit distance <= threshold).
bool are_texture_names_similar(std::string_view a, std::string_view b);

// Searches the provided material roots for a texture file matching or similar
// to `normalized_name` (e.g. "b_wallaper1" or "brick/b_wallaper1").
// Returns the suggested material-relative name (e.g. "b_wallpaper1" or
// "brick/b_wallpaper1"), or an empty string if no candidate was found.
std::string find_similar_texture_name(
    const std::string& normalized_name,
    const std::vector<std::filesystem::path>& roots);

// Searches the provided material roots on disk for an exact or similar
// texture file for `game_path` (e.g. "materials/b_wallaper1.vtf").
// Returns the absolute/resolved filesystem path to the file if found, or empty path if not.
std::filesystem::path resolve_texture_file_gracefully(
    const std::string& game_path,
    const std::vector<std::filesystem::path>& roots);

} // namespace ui
