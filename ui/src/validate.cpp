#include "ui/validate.h"

#include "vmt/value.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ui {
namespace {

std::string lower_copy(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

std::string trim_copy(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool has_uppercase(std::string_view text) {
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z') {
            return true;
        }
    }
    return false;
}

std::unordered_set<std::string> known_parameters(const vmt::ShaderSchema& schema) {
    std::unordered_set<std::string> known;
    for (const vmt::SchemaGroup& group : schema.groups()) {
        for (const vmt::SchemaParam& param : group.parameters) {
            known.insert(lower_copy(param.name));
        }
    }
    // Parameters awaiting verification are known, just unconfirmed: flagging
    // them as "unknown" would contradict the schema's own needs_review list.
    for (const vmt::NeedsReviewParam& param : schema.needs_review()) {
        known.insert(lower_copy(param.name));
    }
    return known;
}

// Family dependencies verified against the Valve Developer Community wiki:
// the listed parameters have no effect unless their gate is present and
// enabled. Texture-valued gates ($envmap, $detail, $bumpmap) count as enabled
// when they hold any non-empty path; boolean gates ($phong) use parse_bool.
const char* dependency_gate(std::string_view lower_name) {
    if (lower_name.rfind("$phong", 0) == 0 && lower_name != "$phong") {
        return "$phong";
    }
    if (lower_name == "$normalmapalphaenvmapmask" || lower_name == "$basealphaenvmapmask") {
        return "$envmap";
    }
    if (lower_name.rfind("$envmap", 0) == 0 && lower_name != "$envmap") {
        return "$envmap";
    }
    if (lower_name.rfind("$detail", 0) == 0 && lower_name != "$detail") {
        return "$detail";
    }
    if (lower_name == "$bumpframe" || lower_name == "$bumpframes" ||
        lower_name == "$bumpmapscale") {
        return "$bumpmap";
    }
    return nullptr;
}

bool gate_enabled(const vmt::Document& document, std::string_view gate) {
    const vmt::Node* node = document.find(gate);
    if (node == nullptr) {
        return false;
    }
    const std::string value = trim_copy(node->value());
    if (value.empty()) {
        return false;
    }
    const auto parsed = vmt::parse_bool(value);
    if (parsed) {
        return parsed.value();
    }
    return true; // a texture/path value: presence enables the family
}

struct Collector {
    std::vector<Finding> findings;

    void add(Severity severity, std::string code, std::string message,
             const vmt::Node& node) {
        findings.push_back(Finding{severity, std::move(code), std::move(message),
                                   node.location().line, node.location().column});
    }

    void add(Severity severity, std::string code, std::string message,
             vmt::SourceLocation location) {
        findings.push_back(Finding{severity, std::move(code), std::move(message),
                                   location.line, location.column});
    }
};

void check_value_type(Collector& out, const vmt::Node& node, const vmt::SchemaParam& param) {
    const std::string value = trim_copy(node.value());
    if (value.empty()) {
        return; // an empty value is a separate concern; do not guess its type
    }

    switch (param.type) {
    case vmt::ParamType::Bool:
        if (!vmt::parse_bool(value)) {
            out.add(Severity::Warning, "type-mismatch",
                    "parameter \"" + std::string(node.key()) + "\" expects a boolean (0/1)",
                    node);
        }
        break;
    case vmt::ParamType::Int:
        if (!vmt::parse_int(value)) {
            out.add(Severity::Warning, "type-mismatch",
                    "parameter \"" + std::string(node.key()) + "\" expects an integer",
                    node);
        }
        break;
    case vmt::ParamType::Float:
        if (!vmt::parse_float(value)) {
            out.add(Severity::Warning, "type-mismatch",
                    "parameter \"" + std::string(node.key()) + "\" expects a number",
                    node);
        }
        break;
    case vmt::ParamType::Color:
        if (!vmt::parse_color(value)) {
            out.add(Severity::Warning, "type-mismatch",
                    "parameter \"" + std::string(node.key()) +
                        "\" expects a color like \"{255 255 255}\"",
                    node);
        }
        break;
    case vmt::ParamType::Vector:
        if (!vmt::parse_vec3(value)) {
            out.add(Severity::Warning, "type-mismatch",
                    "parameter \"" + std::string(node.key()) +
                        "\" expects three numbers like \"[0 0 1]\"",
                    node);
        }
        break;
    case vmt::ParamType::Matrix:
        if (!vmt::parse_matrix(value)) {
            out.add(Severity::Warning, "type-mismatch",
                    "parameter \"" + std::string(node.key()) +
                        "\" expects a texture transform (\"center ... scale ...\")",
                    node);
        }
        break;
    case vmt::ParamType::Enum: {
        bool found = false;
        for (const std::string& candidate : param.values) {
            if (candidate == value) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::string allowed;
            for (const std::string& candidate : param.values) {
                if (!allowed.empty()) {
                    allowed += ", ";
                }
                allowed += "\"" + candidate + "\"";
            }
            out.add(Severity::Warning, "type-mismatch",
                    "parameter \"" + std::string(node.key()) + "\" expects one of " + allowed +
                        " (got \"" + value + "\")",
                    node);
        }
        break;
    }
    case vmt::ParamType::Texture:
    case vmt::ParamType::String:
        break;
    }

    if (param.has_range && (param.type == vmt::ParamType::Int || param.type == vmt::ParamType::Float)) {
        bool parsed = false;
        double numeric = 0.0;
        if (param.type == vmt::ParamType::Int) {
            const auto result = vmt::parse_int(value);
            parsed = result.ok();
            if (parsed) {
                numeric = static_cast<double>(result.value());
            }
        } else {
            const auto result = vmt::parse_float(value);
            parsed = result.ok();
            if (parsed) {
                numeric = static_cast<double>(result.value());
            }
        }
        if (parsed && (numeric < param.min_value || numeric > param.max_value)) {
            std::string range = "[" + vmt::format_float(static_cast<float>(param.min_value)) +
                                ", " + vmt::format_float(static_cast<float>(param.max_value)) + "]";
            out.add(Severity::Warning, "range-mismatch",
                    "parameter \"" + std::string(node.key()) +
                        "\" is outside the schema range " + range + " (got \"" + value + "\")",
                    node);
        }
    }
}

void check_texture(Collector& out, const vmt::Node& node, bool is_envmap,
                   const ValidationOptions& options, bool& skipped_reported) {
    const std::string value = trim_copy(node.value());
    if (value.empty()) {
        return;
    }
    const std::string lower = lower_copy(value);
    if (lower.rfind("_rt_", 0) == 0) {
        return; // engine render target; not a file on disk
    }
    if (is_envmap && lower == "env_cubemap") {
        return; // special value: the map's cubemap, not a VTF
    }

    const bool backslash = value.find('\\') != std::string::npos;
    const bool extension =
        lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".vtf") == 0;
    const bool uppercase = has_uppercase(value);

    if (backslash) {
        out.add(Severity::Warning, "path-backslash",
                "texture path \"" + value + "\" uses backslashes; use forward slashes",
                node);
    }
    if (extension) {
        out.add(Severity::Warning, "path-extension",
                "texture path \"" + value + "\" includes \".vtf\"; drop the extension",
                node);
    }
    if (uppercase) {
        out.add(Severity::Info, "path-case",
                "texture path \"" + value +
                    "\" contains uppercase characters; Valve convention is lowercase",
                node);
    }

    std::string normalized = value;
    for (char& c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (extension) {
        normalized.erase(normalized.size() - 4);
    }
    // Source stores VPK paths lowercased and its file system is
    // case-insensitive: resolve in lowercase so mixed-case references do not
    // produce duplicate lookups.
    normalized = lower_copy(normalized);

    if (!options.resolve_texture) {
        if (!skipped_reported) {
            skipped_reported = true;
            out.add(Severity::Info, "texture-check-skipped",
                    "texture existence was not checked: no engine file system is connected",
                    node);
        }
        return;
    }

    const TextureStatus status = options.resolve_texture(normalized);
    if (status == TextureStatus::Missing) {
        out.add(Severity::Error, "missing-texture",
                "texture \"" + normalized +
                    "\" was not found (materials/" + normalized +
                    ".vtf is missing from the mounted search paths)",
                node);
    } else if (status == TextureStatus::Unavailable && !skipped_reported) {
        skipped_reported = true;
        out.add(Severity::Info, "texture-check-skipped",
                "texture existence could not be checked: the engine file system is unavailable",
                node);
    }
}

} // namespace

std::vector<Finding> validate_document(const vmt::Document& document,
                                       const ValidationOptions& options) {
    Collector out;
    const vmt::ShaderSchema schema =
        options.schemas != nullptr ? options.schemas->schema_for(document.shader())
                                   : vmt::ShaderSchema();
    const bool have_schema = options.schemas != nullptr;

    // The parser is lossless and recovering: it never fails, it records
    // diagnostics. Every diagnostic is an error the user must fix.
    for (const vmt::Diagnostic& diagnostic : document.diagnostics()) {
        out.add(Severity::Error, "parse-error", diagnostic.message, diagnostic.location);
    }

    if (!have_schema) {
        out.add(Severity::Info, "schema-unavailable",
                "shader parameter schemas are not loaded; only generic checks ran",
                vmt::SourceLocation{0, 0});
    } else if (!document.shader().empty() && !options.schemas->has_shader(document.shader())) {
        // Spec: unknown shader is a warning, never an error. Parameter checks
        // are skipped because the schema is what defines the parameter set.
        out.add(Severity::Warning, "unknown-shader",
                "no parameter schema for shader \"" + std::string(document.shader()) +
                    "\"; parameter checks were skipped",
                document.shader_location());
    }

    const bool check_params = have_schema && !schema.empty();
    const std::unordered_set<std::string> known =
        check_params ? known_parameters(schema) : std::unordered_set<std::string>();

    std::unordered_map<std::string, const vmt::Node*> first_seen;
    bool skipped_reported = false;

    for (const vmt::Node* node : document.parameters()) {
        const std::string lower = lower_copy(node->key());

        // Duplicate parameters (within the shader body block).
        const auto inserted = first_seen.emplace(lower, node);
        if (!inserted.second) {
            out.add(Severity::Warning, "duplicate-parameter",
                    "duplicate parameter \"" + std::string(node->key()) +
                        "\" (first defined at line " +
                        std::to_string(inserted.first->second->location().line) + ")",
                    *node);
        }

        if (check_params) {
            if (!known.count(lower)) {
                out.add(Severity::Warning, "unknown-parameter",
                        "parameter \"" + std::string(node->key()) + "\" is not part of shader \"" +
                            std::string(schema.shader()) + "\"",
                        *node);
            }

            // Missing dependency parameter (phong family, envmap family, ...).
            const char* gate = dependency_gate(lower);
            if (gate != nullptr && !gate_enabled(document, gate)) {
                out.add(Severity::Warning, "missing-dependency",
                        "parameter \"" + std::string(node->key()) +
                            "\" has no effect unless \"" + gate +
                            "\" is enabled (set \"" + gate + "\" \"1\")",
                        *node);
            }

            const vmt::SchemaParam* schema_param = schema.find_param(node->key());
            if (schema_param != nullptr) {
                check_value_type(out, *node, *schema_param);
                if (schema_param->type == vmt::ParamType::Texture) {
                    check_texture(out, *node, lower == "$envmap", options, skipped_reported);
                }
            }
        }
    }

    std::stable_sort(out.findings.begin(), out.findings.end(),
                     [](const Finding& a, const Finding& b) {
                         const std::uint32_t line_a = a.line == 0 ? UINT32_MAX : a.line;
                         const std::uint32_t line_b = b.line == 0 ? UINT32_MAX : b.line;
                         if (line_a != line_b) {
                             return line_a < line_b;
                         }
                         if (a.column != b.column) {
                             return a.column < b.column;
                         }
                         return static_cast<int>(a.severity) < static_cast<int>(b.severity);
                     });
    return std::move(out.findings);
}

std::vector<Finding> validate_text(const std::string& text, const ValidationOptions& options) {
    vmt::Result<vmt::Document> parsed = vmt::Document::parse(text, "inline.vmt");
    if (!parsed) {
        const vmt::Error& error = parsed.error();
        return {Finding{Severity::Error, "parse-error", error.message, error.line, error.column}};
    }
    return validate_document(parsed.value(), options);
}

} // namespace ui
