#pragma once

// vmt_core shader parameter schema (Phase 4).
//
// Schema files are JSON documents stored under data/schema/. Each file
// describes the parameters one VMT shader kind accepts, grouped for
// presentation, plus a "needs_review" list of parameters that have NOT yet
// been verified against the Valve Developer Community wiki and therefore must
// not be presented as confirmed knowledge.
//
// This module owns:
//   - the in-memory model of a schema (ShaderSchema, SchemaParam, ...)
//   - a strict loader/validator with precise file/line/column errors
//   - case-insensitive lookup of schemas by shader name
//
// The loader is engine-free and depends only on the standard library.
// Schema "type" fields are semantic metadata for editors/UI: the loader
// validates the schema file, not the VMT values of a document.

#include "vmt/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vmt {

// Semantic value domain a parameter carries in a VMT. This describes how an
// editor should interpret/format the raw VMT text; syntaxes overlap (a color
// may be written "{r g b}" or "[r g b]"), so the loader does not enforce them.
enum class ParamType : std::uint8_t {
    String,  // free-form text
    Texture, // material / VTF path
    Bool,    // 0/1 (also true/false, yes/no, on/off per vmt::parse_bool)
    Int,
    Float,
    Color,   // RGB(A) tint: "{r g b}", "{r g b a}" or "[r g b]"
    Vector,  // three-component value: "[x y z]"
    Matrix,  // texture transform: "center .. scale .. rotate .. translate .."
    Enum,    // one of SchemaParam::values
};

// Canonical lowercase name of a type: "string", "texture", "bool", "int",
// "float", "color", "vector", "matrix", "enum".
const char* param_type_name(ParamType type) noexcept;

// Inverse of param_type_name; InvalidArgument error for unknown names.
Result<ParamType> param_type_from_name(std::string_view name);

struct SchemaParam {
    std::string              name;          // authored spelling, e.g. "$basetexture"
    ParamType                type = ParamType::String;
    bool                     has_default = false;
    std::string              default_value; // raw VMT text, e.g. "1" or "[0 0 0]"
    std::string              description;
    std::vector<std::string> values;        // non-empty iff type == Enum
    bool                     has_range = false;
    double                   min_value = 0.0;
    double                   max_value = 0.0;
    // Presentation hint for editors: hidden behind a "Show advanced" toggle.
    // Not a statement about the engine - only about how common/tunable the
    // parameter is for typical users.
    bool                     advanced = false;
};

struct SchemaGroup {
    std::string              name;
    std::vector<SchemaParam> parameters;
};

// A parameter deliberately kept out of the main lists because it has not been
// verified against the Valve Developer Community wiki yet.
struct NeedsReviewParam {
    std::string name;   // best-known spelling, e.g. "$treeSway"
    std::string reason; // why it is unverified / what to check
};

class ShaderSchema {
public:
    ShaderSchema() = default;

    // Canonical shader name as authored in the JSON "shader" field.
    // Empty for a default-constructed schema (e.g. unknown shader lookup).
    const std::string& shader() const noexcept { return shader_; }

    // Parameters grouped for presentation, in file order.
    const std::vector<SchemaGroup>& groups() const noexcept { return groups_; }

    // Parameters awaiting manual verification; never part of groups().
    const std::vector<NeedsReviewParam>& needs_review() const noexcept { return needs_review_; }

    // True when this schema carries no groups and no needs_review entries,
    // i.e. the result of looking up an unknown shader.
    bool empty() const noexcept { return groups_.empty() && needs_review_.empty(); }

    // Case-insensitive lookups.
    const SchemaParam* find_param(std::string_view name) const noexcept;
    const SchemaGroup* find_group(std::string_view name) const noexcept;

private:
    friend class SchemaLibrary;

    std::string                   shader_;
    std::vector<SchemaGroup>      groups_;
    std::vector<NeedsReviewParam> needs_review_;
};

class SchemaLibrary {
public:
    SchemaLibrary() = default;

    // Load and validate every *.json file in dir (sorted order, deterministic
    // errors). Fails on the first invalid file. Missing dir => SchemaNotFound.
    static Result<SchemaLibrary> load_dir(const std::filesystem::path& dir);

    // Load one schema from JSON text. source_name is used in error messages
    // (pass the file path when available).
    static Result<SchemaLibrary> load_json(std::string_view text, std::string source_name = {});

    // Case-insensitive lookup. Unknown shader names return an empty schema
    // (ShaderSchema::empty() == true); this is not an error.
    ShaderSchema schema_for(std::string_view shader_name) const;

    bool has_shader(std::string_view shader_name) const noexcept;

    // Canonical names of all loaded schemas, sorted.
    std::vector<std::string> shader_names() const;

private:
    struct Entry {
        std::string  key; // lowercased shader name
        ShaderSchema schema;
    };
    std::vector<Entry> schemas_; // sorted by key
};

} // namespace vmt
