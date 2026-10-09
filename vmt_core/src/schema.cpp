#include "vmt/schema.h"

#include "internal.h"
#include "json.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vmt {

namespace {

using detail::JsonValue;

// Source text + name used to build validation errors with line/column.
struct Ctx {
    const std::string& file;
    std::string_view   text;
};

Error schema_error(const Ctx& ctx, std::size_t offset, std::string message) {
    const std::string copy(ctx.text);
    const SourceLocation location = detail::loc_of(copy, offset);
    return make_error(ErrorCode::SchemaValidationError, std::move(message), {}, ctx.file, location.line, location.column);
}

Error schema_error(const Ctx& ctx, const JsonValue& at, std::string message) {
    return schema_error(ctx, at.offset, std::move(message));
}

std::string lower_copy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out.push_back(detail::ascii_lower(c));
    return out;
}

std::string lower_trimmed(std::string_view text) {
    return lower_copy(detail::trim(text));
}

bool is_identifier(std::string_view text) {
    if (text.empty()) return false;
    const char first = text[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'))) return false;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// Parameter names start with '$' or '%'; remaining characters are
// [A-Za-z0-9_]. Used for both parameters and needs_review names.
bool is_param_name(std::string_view text) {
    if (text.empty()) return false;
    if (text[0] != '$' && text[0] != '%') return false;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

std::string number_text(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return std::string(buffer);
}

// A validated parameter name and the group that defined it, for
// duplicate/cross-list checks within one schema.
struct ParamOrigin {
    std::string lower_name;
    std::string group_name;
};

Result<void> validate_param(const JsonValue& node, const Ctx& ctx, const std::string& path,
                            SchemaParam& param, std::vector<ParamOrigin>& origins,
                            const std::string& group_name) {
    if (node.kind != JsonValue::Kind::Object) {
        return Err(schema_error(ctx, node, path + ": expected an object (got " + node.kind_name() + ")"));
    }
    for (const auto& member : node.members) {
        if (member.first != "name" && member.first != "type" && member.first != "default" &&
            member.first != "description" && member.first != "values" &&
            member.first != "min" && member.first != "max" && member.first != "advanced") {
            return Err(schema_error(ctx, member.second,
                path + "." + member.first + ": unknown field (type: " + member.second.kind_name() +
                "); expected one of: name, type, default, description, values, min, max, advanced"));
        }
    }

    const JsonValue* name = node.find("name");
    if (name == nullptr) return Err(schema_error(ctx, node, path + ": missing required field \"name\""));
    if (name->kind != JsonValue::Kind::String) {
        return Err(schema_error(ctx, *name, path + ".name: expected a non-empty string (got " + name->kind_name() + ")"));
    }
    if (name->string.empty()) {
        return Err(schema_error(ctx, *name, path + ".name: expected a non-empty string (got empty string)"));
    }
    if (!is_param_name(name->string)) {
        return Err(schema_error(ctx, *name,
            path + ".name: \"" + name->string + "\" is not a valid parameter name (expected '$' or '%' followed by [A-Za-z0-9_]*)"));
    }
    const std::string lower_name = lower_copy(name->string);
    for (const ParamOrigin& origin : origins) {
        if (origin.lower_name == lower_name) {
            return Err(schema_error(ctx, *name,
                path + ".name: duplicate parameter \"" + name->string + "\" (already defined in group \"" + origin.group_name + "\")"));
        }
    }
    param.name = name->string;

    const JsonValue* type = node.find("type");
    if (type == nullptr) return Err(schema_error(ctx, node, path + ": missing required field \"type\""));
    if (type->kind != JsonValue::Kind::String) {
        return Err(schema_error(ctx, *type, path + ".type: expected a string (got " + type->kind_name() + ")"));
    }
    Result<ParamType> parsed_type = param_type_from_name(type->string);
    if (!parsed_type) {
        return Err(schema_error(ctx, *type, path + ".type: " + parsed_type.error().message));
    }
    param.type = parsed_type.value();

    if (const JsonValue* default_node = node.find("default")) {
        if (default_node->kind != JsonValue::Kind::String) {
            return Err(schema_error(ctx, *default_node,
                path + ".default: expected a string (raw VMT text) (got " + default_node->kind_name() + ")"));
        }
        param.has_default = true;
        param.default_value = default_node->string;
    }

    if (const JsonValue* description = node.find("description")) {
        if (description->kind != JsonValue::Kind::String) {
            return Err(schema_error(ctx, *description, path + ".description: expected a string (got " + description->kind_name() + ")"));
        }
        param.description = description->string;
    }

    if (const JsonValue* advanced = node.find("advanced")) {
        if (advanced->kind != JsonValue::Kind::Bool) {
            return Err(schema_error(ctx, *advanced,
                path + ".advanced: expected a boolean (got " + advanced->kind_name() + ")"));
        }
        param.advanced = advanced->boolean;
    }

    const JsonValue* values = node.find("values");
    if (param.type == ParamType::Enum) {
        if (values == nullptr) {
            return Err(schema_error(ctx, node, path + ".values: required for enum parameters and must be a non-empty array of strings"));
        }
        if (values->kind != JsonValue::Kind::Array || values->items.empty()) {
            return Err(schema_error(ctx, *values, path + ".values: expected a non-empty array of strings"));
        }
        for (std::size_t i = 0; i < values->items.size(); ++i) {
            const JsonValue& item = values->items[i];
            if (item.kind != JsonValue::Kind::String || item.string.empty()) {
                return Err(schema_error(ctx, item, path + ".values[" + std::to_string(i) + "]: expected a non-empty string"));
            }
            param.values.push_back(item.string);
        }
    } else if (values != nullptr) {
        return Err(schema_error(ctx, *values,
            path + ".values: only allowed for enum parameters (type is \"" + std::string(param_type_name(param.type)) + "\")"));
    }

    const bool numeric = param.type == ParamType::Int || param.type == ParamType::Float;
    const JsonValue* min_node = node.find("min");
    const JsonValue* max_node = node.find("max");
    if (min_node != nullptr) {
        if (!numeric) {
            return Err(schema_error(ctx, *min_node,
                path + ".min: only allowed for int or float parameters (type is \"" + std::string(param_type_name(param.type)) + "\")"));
        }
        if (min_node->kind != JsonValue::Kind::Number) {
            return Err(schema_error(ctx, *min_node, path + ".min: expected a number (got " + min_node->kind_name() + ")"));
        }
        param.min_value = min_node->number;
        param.has_range = true;
    }
    if (max_node != nullptr) {
        if (!numeric) {
            return Err(schema_error(ctx, *max_node,
                path + ".max: only allowed for int or float parameters (type is \"" + std::string(param_type_name(param.type)) + "\")"));
        }
        if (max_node->kind != JsonValue::Kind::Number) {
            return Err(schema_error(ctx, *max_node, path + ".max: expected a number (got " + max_node->kind_name() + ")"));
        }
        param.max_value = max_node->number;
        param.has_range = true;
    }
    if (min_node != nullptr && max_node != nullptr && !(param.min_value <= param.max_value)) {
        return Err(schema_error(ctx, *max_node, path + ".max: must be >= min (" + number_text(param.min_value) + ")"));
    }

    origins.push_back(ParamOrigin{ lower_name, group_name });
    return Ok();
}

Result<void> validate_groups(const JsonValue& node, const Ctx& ctx,
                             std::vector<SchemaGroup>& groups,
                             std::vector<ParamOrigin>& origins) {
    if (node.kind != JsonValue::Kind::Array) {
        return Err(schema_error(ctx, node, "groups: expected an array (got " + std::string(node.kind_name()) + ")"));
    }
    std::vector<std::string> group_names; // lowercased, for duplicate detection
    for (std::size_t gi = 0; gi < node.items.size(); ++gi) {
        const JsonValue& group_node = node.items[gi];
        const std::string path = "groups[" + std::to_string(gi) + "]";
        if (group_node.kind != JsonValue::Kind::Object) {
            return Err(schema_error(ctx, group_node, path + ": expected an object (got " + group_node.kind_name() + ")"));
        }
        for (const auto& member : group_node.members) {
            if (member.first != "name" && member.first != "parameters") {
                return Err(schema_error(ctx, member.second,
                    path + "." + member.first + ": unknown field (type: " + member.second.kind_name() +
                    "); expected one of: name, parameters"));
            }
        }

        const JsonValue* name = group_node.find("name");
        if (name == nullptr) return Err(schema_error(ctx, group_node, path + ": missing required field \"name\""));
        if (name->kind != JsonValue::Kind::String) {
            return Err(schema_error(ctx, *name, path + ".name: expected a non-empty string (got " + name->kind_name() + ")"));
        }
        if (name->string.empty()) {
            return Err(schema_error(ctx, *name, path + ".name: expected a non-empty string (got empty string)"));
        }
        const std::string lower_name = lower_copy(name->string);
        for (std::size_t prev = 0; prev < group_names.size(); ++prev) {
            if (group_names[prev] == lower_name) {
                return Err(schema_error(ctx, *name,
                    path + ".name: duplicate group \"" + name->string + "\" (already defined at groups[" + std::to_string(prev) + "])"));
            }
        }
        group_names.push_back(lower_name);

        const JsonValue* parameters = group_node.find("parameters");
        if (parameters == nullptr) {
            return Err(schema_error(ctx, group_node, path + ": missing required field \"parameters\""));
        }
        if (parameters->kind != JsonValue::Kind::Array) {
            return Err(schema_error(ctx, *parameters, path + ".parameters: expected an array (got " + parameters->kind_name() + ")"));
        }

        SchemaGroup group;
        group.name = name->string;
        for (std::size_t pi = 0; pi < parameters->items.size(); ++pi) {
            const std::string param_path = path + ".parameters[" + std::to_string(pi) + "]";
            SchemaParam param;
            Result<void> valid = validate_param(parameters->items[pi], ctx, param_path, param, origins, group.name);
            if (!valid) return valid;
            group.parameters.push_back(std::move(param));
        }
        groups.push_back(std::move(group));
    }
    return Ok();
}

Result<void> validate_needs_review(const JsonValue& node, const Ctx& ctx,
                                   std::vector<NeedsReviewParam>& needs_review,
                                   const std::vector<ParamOrigin>& origins) {
    if (node.kind != JsonValue::Kind::Array) {
        return Err(schema_error(ctx, node, "needs_review: expected an array (got " + std::string(node.kind_name()) + ")"));
    }
    std::vector<std::string> seen; // lowercased names, for duplicate detection
    for (std::size_t i = 0; i < node.items.size(); ++i) {
        const JsonValue& entry_node = node.items[i];
        const std::string path = "needs_review[" + std::to_string(i) + "]";
        if (entry_node.kind != JsonValue::Kind::Object) {
            return Err(schema_error(ctx, entry_node, path + ": expected an object (got " + entry_node.kind_name() + ")"));
        }
        for (const auto& member : entry_node.members) {
            if (member.first != "name" && member.first != "reason") {
                return Err(schema_error(ctx, member.second,
                    path + "." + member.first + ": unknown field (type: " + member.second.kind_name() +
                    "); expected one of: name, reason"));
            }
        }

        const JsonValue* name = entry_node.find("name");
        if (name == nullptr) return Err(schema_error(ctx, entry_node, path + ": missing required field \"name\""));
        if (name->kind != JsonValue::Kind::String) {
            return Err(schema_error(ctx, *name, path + ".name: expected a non-empty string (got " + name->kind_name() + ")"));
        }
        if (name->string.empty()) {
            return Err(schema_error(ctx, *name, path + ".name: expected a non-empty string (got empty string)"));
        }
        if (!is_param_name(name->string)) {
            return Err(schema_error(ctx, *name,
                path + ".name: \"" + name->string + "\" is not a valid parameter name (expected '$' or '%' followed by [A-Za-z0-9_]*)"));
        }
        const std::string lower_name = lower_copy(name->string);
        for (std::size_t prev = 0; prev < seen.size(); ++prev) {
            if (seen[prev] == lower_name) {
                return Err(schema_error(ctx, *name,
                    path + ".name: duplicate needs_review entry \"" + name->string + "\" (already listed at needs_review[" + std::to_string(prev) + "])"));
            }
        }
        for (const ParamOrigin& origin : origins) {
            if (origin.lower_name == lower_name) {
                return Err(schema_error(ctx, *name,
                    path + ".name: \"" + name->string + "\" is already defined as a parameter in group \"" + origin.group_name + "\""));
            }
        }
        seen.push_back(lower_name);

        NeedsReviewParam review;
        review.name = name->string;
        if (const JsonValue* reason = entry_node.find("reason")) {
            if (reason->kind != JsonValue::Kind::String) {
                return Err(schema_error(ctx, *reason, path + ".reason: expected a string (got " + reason->kind_name() + ")"));
            }
            review.reason = reason->string;
        }
        needs_review.push_back(std::move(review));
    }
    return Ok();
}

Result<void> validate_schema(const JsonValue& root, const Ctx& ctx,
                             std::string& shader_name,
                             std::vector<SchemaGroup>& groups,
                             std::vector<NeedsReviewParam>& needs_review) {
    if (root.kind != JsonValue::Kind::Object) {
        return Err(schema_error(ctx, root, "root: expected a JSON object (got " + std::string(root.kind_name()) + ")"));
    }
    for (const auto& member : root.members) {
        if (member.first != "format" && member.first != "shader" &&
            member.first != "groups" && member.first != "needs_review") {
            return Err(schema_error(ctx, member.second,
                member.first + ": unknown field (type: " + member.second.kind_name() +
                "); expected one of: format, shader, groups, needs_review"));
        }
    }

    if (const JsonValue* format = root.find("format")) {
        if (format->kind != JsonValue::Kind::String) {
            return Err(schema_error(ctx, *format, "format: expected the string \"vmt-schema/1\" (got " + std::string(format->kind_name()) + ")"));
        }
        if (format->string != "vmt-schema/1") {
            return Err(schema_error(ctx, *format, "format: expected \"vmt-schema/1\" (got \"" + format->string + "\")"));
        }
    }

    const JsonValue* shader = root.find("shader");
    if (shader == nullptr) return Err(schema_error(ctx, root, "missing required field \"shader\""));
    if (shader->kind != JsonValue::Kind::String) {
        return Err(schema_error(ctx, *shader, "shader: expected a non-empty string (got " + std::string(shader->kind_name()) + ")"));
    }
    if (shader->string.empty()) {
        return Err(schema_error(ctx, *shader, "shader: expected a non-empty string (got empty string)"));
    }
    if (!is_identifier(shader->string)) {
        return Err(schema_error(ctx, *shader,
            "shader: \"" + shader->string + "\" is not a valid identifier (expected [A-Za-z][A-Za-z0-9_]*)"));
    }
    shader_name = shader->string;

    const JsonValue* groups_node = root.find("groups");
    if (groups_node == nullptr) return Err(schema_error(ctx, root, "missing required field \"groups\""));
    std::vector<ParamOrigin> origins;
    Result<void> groups_valid = validate_groups(*groups_node, ctx, groups, origins);
    if (!groups_valid) return groups_valid;

    const JsonValue* review_node = root.find("needs_review");
    if (review_node != nullptr) {
        Result<void> review_valid = validate_needs_review(*review_node, ctx, needs_review, origins);
        if (!review_valid) return review_valid;
    }
    return Ok();
}

} // namespace

const char* param_type_name(ParamType type) noexcept {
    switch (type) {
        case ParamType::String:  return "string";
        case ParamType::Texture: return "texture";
        case ParamType::Bool:    return "bool";
        case ParamType::Int:     return "int";
        case ParamType::Float:   return "float";
        case ParamType::Color:   return "color";
        case ParamType::Vector:  return "vector";
        case ParamType::Matrix:  return "matrix";
        case ParamType::Enum:    return "enum";
    }
    return "string";
}

Result<ParamType> param_type_from_name(std::string_view name) {
    if (name == "string")  return Ok(ParamType::String);
    if (name == "texture") return Ok(ParamType::Texture);
    if (name == "bool")    return Ok(ParamType::Bool);
    if (name == "int")     return Ok(ParamType::Int);
    if (name == "float")   return Ok(ParamType::Float);
    if (name == "color")   return Ok(ParamType::Color);
    if (name == "vector")  return Ok(ParamType::Vector);
    if (name == "matrix")  return Ok(ParamType::Matrix);
    if (name == "enum")    return Ok(ParamType::Enum);
    return Result<ParamType>(make_error(ErrorCode::InvalidArgument,
        "unknown parameter type '" + std::string(name) +
        "' (expected one of: string, texture, bool, int, float, color, vector, matrix, enum)"));
}

const SchemaParam* ShaderSchema::find_param(std::string_view name) const noexcept {
    for (const SchemaGroup& group : groups_) {
        for (const SchemaParam& param : group.parameters) {
            if (detail::iequals(param.name, name)) return &param;
        }
    }
    return nullptr;
}

const SchemaGroup* ShaderSchema::find_group(std::string_view name) const noexcept {
    for (const SchemaGroup& group : groups_) {
        if (detail::iequals(group.name, name)) return &group;
    }
    return nullptr;
}

Result<SchemaLibrary> SchemaLibrary::load_json(std::string_view text, std::string source_name) {
    Result<JsonValue> parsed = detail::parse_json(text, source_name);
    if (!parsed) {
        Error error = parsed.error();
        error.code = ErrorCode::SchemaLoadError;
        return Result<SchemaLibrary>(std::move(error));
    }

    const JsonValue& root = parsed.value();
    const Ctx ctx{ source_name, text };
    ShaderSchema schema;
    Result<void> valid = validate_schema(root, ctx, schema.shader_, schema.groups_, schema.needs_review_);
    if (!valid) return Result<SchemaLibrary>(valid.error());

    SchemaLibrary library;
    library.schemas_.push_back(Entry{ lower_copy(schema.shader_), std::move(schema) });
    return Ok(std::move(library));
}

Result<SchemaLibrary> SchemaLibrary::load_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    const bool is_dir = std::filesystem::is_directory(dir, ec);
    if (ec || !is_dir) {
        return Result<SchemaLibrary>(make_error(ErrorCode::SchemaNotFound,
            "schema directory not found: " + dir.string(),
            "Check the schema directory path."));
    }

    std::vector<std::filesystem::path> files;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return Result<SchemaLibrary>(make_error(ErrorCode::SchemaNotFound,
            "cannot read schema directory: " + dir.string(),
            "Check the schema directory path."));
    }
    const std::filesystem::directory_iterator end;
    while (it != end) {
        std::error_code entry_error;
        const std::filesystem::directory_entry& entry = *it;
        if (entry.is_regular_file(entry_error) && !entry_error) {
            const std::filesystem::path& file = entry.path();
            if (detail::iequals(file.extension().string(), ".json")) {
                files.push_back(file);
            }
        }
        it.increment(ec);
        if (ec) {
            return Result<SchemaLibrary>(make_error(ErrorCode::SchemaNotFound,
                "cannot read schema directory: " + dir.string(),
                "Check the schema directory path."));
        }
    }
    std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return a.string() < b.string();
    });

    SchemaLibrary library;
    std::vector<std::pair<std::string, std::string>> origins; // shader key -> file it came from
    for (const std::filesystem::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return Result<SchemaLibrary>(make_error(ErrorCode::SchemaLoadError, "cannot read schema file", {}, file.string()));
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) {
            return Result<SchemaLibrary>(make_error(ErrorCode::SchemaLoadError, "cannot read schema file", {}, file.string()));
        }
        Result<SchemaLibrary> loaded = load_json(text, file.string());
        if (!loaded) return loaded;
        SchemaLibrary one = std::move(loaded.value());
        const Entry& entry = one.schemas_.front(); // load_json always yields exactly one entry
        for (const auto& origin : origins) {
            if (origin.first == entry.key) {
                return Result<SchemaLibrary>(make_error(ErrorCode::SchemaValidationError,
                    "duplicate schema for shader '" + entry.schema.shader() + "' (files: " + origin.second + " and " + file.string() + ")",
                    {}, file.string()));
            }
        }
        origins.emplace_back(entry.key, file.string());
        library.schemas_.push_back(std::move(one.schemas_.front()));
    }
    std::sort(library.schemas_.begin(), library.schemas_.end(),
              [](const Entry& a, const Entry& b) { return a.key < b.key; });
    return Ok(std::move(library));
}

ShaderSchema SchemaLibrary::schema_for(std::string_view shader_name) const {
    const std::string key = lower_trimmed(shader_name);
    for (const Entry& entry : schemas_) {
        if (entry.key == key) return entry.schema;
    }
    return ShaderSchema{};
}

bool SchemaLibrary::has_shader(std::string_view shader_name) const noexcept {
    const std::string_view name = detail::trim(shader_name);
    for (const Entry& entry : schemas_) {
        if (entry.key.size() != name.size()) continue;
        bool equal = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (entry.key[i] != detail::ascii_lower(name[i])) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
    }
    return false;
}

std::vector<std::string> SchemaLibrary::shader_names() const {
    std::vector<std::string> names;
    names.reserve(schemas_.size());
    for (const Entry& entry : schemas_) names.push_back(entry.schema.shader());
    return names;
}

} // namespace vmt
