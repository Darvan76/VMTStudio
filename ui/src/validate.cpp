#include "ui/validate.h"

#include "vmt/value.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

    if (!options.resolve_texture && options.material_roots.empty()) {
        if (!skipped_reported) {
            skipped_reported = true;
            out.add(Severity::Info, "texture-check-skipped",
                    "texture existence was not checked: no engine file system is connected",
                    node);
        }
        return;
    }

    TextureStatus status = TextureStatus::Unavailable;
    if (options.resolve_texture) {
        status = options.resolve_texture(normalized);
    } else {
        std::error_code ec;
        const std::filesystem::path rel = "materials/" + normalized + ".vtf";
        bool found = false;
        for (const auto& root : options.material_roots) {
            if (std::filesystem::is_regular_file(root / rel, ec)) {
                found = true;
                break;
            }
        }
        status = found ? TextureStatus::Found : TextureStatus::Missing;
    }

    if (status == TextureStatus::Missing) {
        std::string suggestion;
        if (options.suggest_texture) {
            suggestion = options.suggest_texture(normalized);
        } else if (!options.material_roots.empty()) {
            suggestion = find_similar_texture_name(normalized, options.material_roots);
        }

        std::string message = "texture \"" + normalized +
            "\" was not found (materials/" + normalized +
            ".vtf is missing from the mounted search paths)";
        if (!suggestion.empty()) {
            message += "; did you mean \"" + suggestion + "\"?";
        }

        out.add(Severity::Error, "missing-texture", std::move(message), node);
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

// --- Typo detection & texture path resolution helpers ---

namespace {

std::string collapse_consecutive_repeats(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) {
        char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (res.empty() || res.back() != lower) {
            res.push_back(lower);
        }
    }
    return res;
}

std::pair<std::string_view, std::string_view> split_trailing_digits(std::string_view s) {
    std::size_t i = s.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) {
        --i;
    }
    return {s.substr(0, i), s.substr(i)};
}

int compute_similarity_score(std::string_view target, std::string_view candidate) {
    if (target.empty() || candidate.empty()) {
        return 1000;
    }

    // Exact match case-insensitively
    if (target.size() == candidate.size()) {
        bool eq = true;
        for (std::size_t i = 0; i < target.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(target[i])) !=
                std::tolower(static_cast<unsigned char>(candidate[i]))) {
                eq = false;
                break;
            }
        }
        if (eq) {
            return 0;
        }
    }

    auto [target_base, target_digits] = split_trailing_digits(target);
    auto [cand_base, cand_digits] = split_trailing_digits(candidate);

    // If both have trailing digits and they differ (e.g. wall1 vs wall2):
    // Different numbered textures in a series should not be treated as typos.
    if (!target_digits.empty() && !cand_digits.empty() && target_digits != cand_digits) {
        return 500;
    }

    const std::string target_collapsed = collapse_consecutive_repeats(target);
    const std::string cand_collapsed = collapse_consecutive_repeats(candidate);
    const std::size_t dist = texture_edit_distance(target, candidate);

    // Single vs double letter difference (e.g. b_wallaper1 vs b_wallpaper1, wood_flor vs wood_floor)
    if (target_collapsed == cand_collapsed) {
        return 5 + static_cast<int>(dist);
    }

    if (dist == 1) {
        return 15;
    }

    if (dist == 2 && std::min(target.size(), candidate.size()) >= 5) {
        return 30;
    }

    if (dist == 3 && std::min(target.size(), candidate.size()) >= 10) {
        return 50;
    }

    return 1000;
}

} // namespace

std::size_t texture_edit_distance(std::string_view a, std::string_view b) {
    const std::size_t m = a.size();
    const std::size_t n = b.size();
    if (m == 0) return n;
    if (n == 0) return m;

    if (m == n) {
        bool equal = true;
        for (std::size_t i = 0; i < m; ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                equal = false;
                break;
            }
        }
        if (equal) return 0;
    }

    std::vector<std::size_t> d((m + 1) * (n + 1), 0);
    const auto at = [&d, n](std::size_t i, std::size_t j) -> std::size_t& {
        return d[i * (n + 1) + j];
    };

    for (std::size_t i = 0; i <= m; ++i) at(i, 0) = i;
    for (std::size_t j = 0; j <= n; ++j) at(0, j) = j;

    for (std::size_t i = 1; i <= m; ++i) {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i - 1])));
        for (std::size_t j = 1; j <= n; ++j) {
            const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j - 1])));
            const std::size_t cost = (ca == cb) ? 0 : 1;

            std::size_t del = at(i - 1, j) + 1;
            std::size_t ins = at(i, j - 1) + 1;
            std::size_t sub = at(i - 1, j - 1) + cost;
            std::size_t best = std::min({del, ins, sub});

            if (i > 1 && j > 1) {
                const char prev_ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i - 2])));
                const char prev_cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j - 2])));
                if (ca == prev_cb && prev_ca == cb) {
                    best = std::min(best, at(i - 2, j - 2) + 1);
                }
            }
            at(i, j) = best;
        }
    }
    return at(m, n);
}

bool are_texture_names_similar(std::string_view a, std::string_view b) {
    return compute_similarity_score(a, b) <= 60;
}

std::string find_similar_texture_name(
    const std::string& normalized_name,
    const std::vector<std::filesystem::path>& roots) {
    if (normalized_name.empty() || roots.empty()) {
        return {};
    }

    std::string clean = normalized_name;
    for (char& c : clean) {
        if (c == '\\') {
            c = '/';
        }
    }

    while (clean.rfind("materials/", 0) == 0 || clean.rfind("Materials/", 0) == 0) {
        clean.erase(0, 10);
    }
    if (clean.size() >= 4) {
        std::string tail = clean.substr(clean.size() - 4);
        for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail == ".vtf") {
            clean.erase(clean.size() - 4);
        }
    }
    while (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == '/') clean.pop_back();

    if (clean.empty()) {
        return {};
    }

    std::size_t last_slash = clean.find_last_of('/');
    std::string dir_part;
    std::string stem_part;
    if (last_slash != std::string::npos) {
        dir_part = clean.substr(0, last_slash);
        stem_part = clean.substr(last_slash + 1);
    } else {
        stem_part = clean;
    }

    int best_score = 1000;
    std::size_t best_dist = 1000;
    std::string best_suggested_name;

    const auto check_dir_files = [&](const std::filesystem::path& dir_path, const std::string& prefix_for_suggestion) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir_path, ec)) {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir_path, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".vtf") continue;

            std::string cand_stem = entry.path().stem().string();
            int score = compute_similarity_score(stem_part, cand_stem);
            if (score <= 60) {
                std::size_t dist = texture_edit_distance(stem_part, cand_stem);
                if (score < best_score || (score == best_score && dist < best_dist)) {
                    best_score = score;
                    best_dist = dist;
                    best_suggested_name = prefix_for_suggestion.empty()
                        ? cand_stem
                        : (prefix_for_suggestion + "/" + cand_stem);
                }
            }
        }
    };

    // 1. Search in the expected directory in each root
    for (const auto& root : roots) {
        std::error_code ec;
        if (root.empty() || !std::filesystem::is_directory(root, ec)) continue;

        if (!dir_part.empty()) {
            check_dir_files(root / "materials" / dir_part, dir_part);
            check_dir_files(root / dir_part, dir_part);
        } else {
            check_dir_files(root / "materials", "");
            check_dir_files(root, "");
        }
    }

    // 2. If no match found and dir_part is not empty, check if dir_part had a typo
    if (best_score > 60 && !dir_part.empty()) {
        std::filesystem::path dir_p(dir_part);
        std::string target_folder = dir_p.filename().string();
        std::filesystem::path parent_rel = dir_p.parent_path();

        for (const auto& root : roots) {
            std::error_code ec;
            std::vector<std::filesystem::path> parent_candidates;
            if (!parent_rel.empty()) {
                parent_candidates.push_back(root / "materials" / parent_rel);
                parent_candidates.push_back(root / parent_rel);
            } else {
                parent_candidates.push_back(root / "materials");
                parent_candidates.push_back(root);
            }

            for (const auto& p_cand : parent_candidates) {
                if (!std::filesystem::is_directory(p_cand, ec)) continue;
                for (const auto& entry : std::filesystem::directory_iterator(p_cand, ec)) {
                    if (ec) break;
                    if (entry.is_directory(ec)) {
                        std::string folder_name = entry.path().filename().string();
                        if (are_texture_names_similar(target_folder, folder_name)) {
                            std::string corrected_prefix = parent_rel.empty()
                                ? folder_name
                                : (parent_rel.generic_string() + "/" + folder_name);
                            check_dir_files(entry.path(), corrected_prefix);
                        }
                    }
                }
            }
        }
    }

    // 3. If no match found and dir_part is empty, check immediate subfolders under materials
    if (best_score > 60 && dir_part.empty()) {
        for (const auto& root : roots) {
            std::error_code ec;
            std::filesystem::path mat_dir = root / "materials";
            if (!std::filesystem::is_directory(mat_dir, ec)) {
                mat_dir = root;
            }
            if (!std::filesystem::is_directory(mat_dir, ec)) continue;
            for (const auto& sub : std::filesystem::directory_iterator(mat_dir, ec)) {
                if (ec) break;
                if (sub.is_directory(ec)) {
                    check_dir_files(sub.path(), sub.path().filename().string());
                }
            }
        }
    }

    if (best_score <= 60) {
        for (char& c : best_suggested_name) {
            if (c == '\\') c = '/';
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return best_suggested_name;
    }

    return {};
}

std::filesystem::path resolve_texture_file_gracefully(
    const std::string& game_path,
    const std::vector<std::filesystem::path>& roots) {
    std::error_code ec;
    if (game_path.empty() || roots.empty()) {
        return {};
    }

    // 1. Check exact match first
    const std::filesystem::path rel(game_path);
    for (const auto& root : roots) {
        if (root.empty()) continue;
        const std::filesystem::path candidate = root / rel;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
        std::string s = game_path;
        if (s.rfind("materials/", 0) == 0 || s.rfind("Materials/", 0) == 0) {
            const std::filesystem::path candidate2 = root / s.substr(10);
            if (std::filesystem::is_regular_file(candidate2, ec)) {
                return candidate2;
            }
        }
    }

    // 2. Not found exactly. Search for similar texture on disk.
    std::string suggested = find_similar_texture_name(game_path, roots);
    if (!suggested.empty()) {
        std::filesystem::path suggested_rel = "materials/" + suggested + ".vtf";
        std::filesystem::path suggested_rel_no_mat = suggested + ".vtf";
        for (const auto& root : roots) {
            if (root.empty()) continue;
            std::filesystem::path cand1 = root / suggested_rel;
            if (std::filesystem::is_regular_file(cand1, ec)) {
                return cand1;
            }
            std::filesystem::path cand2 = root / suggested_rel_no_mat;
            if (std::filesystem::is_regular_file(cand2, ec)) {
                return cand2;
            }
        }
    }

    return {};
}

} // namespace ui
