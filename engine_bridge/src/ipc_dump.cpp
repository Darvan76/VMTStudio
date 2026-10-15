#include "bridge/ipc_dump.h"

#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// Parser for the response files of the IPC protocol v2 (docs/IPC_PROTOCOL.md).
// The structure is strict - a known row with a wrong column count or a field
// that does not parse fails the whole dump with a message naming the line and
// the row kind - while the content is forward compatible: unknown row kinds
// are ignored so the backend can add information without breaking older UIs.
// The module is Windows-free and never throws.

namespace bridge {
namespace {

constexpr int kSupportedProtocolVersion = 2;
constexpr char kHeaderPrefix[] = "VMTStudio-IPC\t";
constexpr std::size_t kHeaderPrefixLength = sizeof(kHeaderPrefix) - 1;

// --- Errors -----------------------------------------------------------------

vmt::Error protocol_error(const char* dump_kind, std::string message) {
    return vmt::make_error(
        vmt::ErrorCode::ParseError,
        std::string("ipc_dump: ") + dump_kind + ": " + std::move(message),
        "Update VMT-Studio so the UI and hlmv_backend speak the same IPC protocol");
}

// Error of one broken row; both the 1-based line number and the row kind are
// named so the failing row can be found in the file.
vmt::Error row_error(const char* dump_kind, std::size_t line, const std::string& row_kind,
                     std::string message) {
    return protocol_error(dump_kind, "line " + std::to_string(line) + ", row \"" + row_kind +
                                         "\": " + std::move(message));
}

// --- Strict scalar readers ---------------------------------------------------
//
// std::from_chars is locale independent and does not throw. The wrappers
// require the whole field to be consumed: no whitespace, no trailing text.

bool parse_int(const std::string& text, int& out) {
    if (text.empty()) return false;
    int value = 0;
    const std::from_chars_result result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

bool parse_float(const std::string& text, float& out) {
    if (text.empty()) return false;
    bool has_digit = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            has_digit = true;
        } else if (c != '.' && c != '-' && c != '+' && c != 'e' && c != 'E') {
            return false;  // rejects "inf"/"nan", hex and any locale input
        }
    }
    if (!has_digit) return false;
    float value = 0.0f;
    const std::from_chars_result result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

bool parse_flag(const std::string& text, bool& out) {
    int value = 0;
    if (!parse_int(text, value) || (value != 0 && value != 1)) return false;
    out = value != 0;
    return true;
}

// --- Lines and rows ----------------------------------------------------------

// First line of the text without its terminator ("" for empty text).
std::string first_line(const std::string& text) {
    std::size_t end = text.find('\n');
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

// Splits on LF and drops a trailing CR (CRLF tolerance). A final LF does not
// produce an extra empty line.
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

// Splits on TAB. Consecutive tabs produce empty fields, which several rows
// allow ("shader or empty", "activity or empty").
std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

struct DumpRow {
    std::string text;
    std::size_t line = 0;  // 1-based line number in the file
};

// Validates the protocol header and the `kind` row and returns the payload
// rows with their line numbers. A file from a newer backend is refused with a
// clear "the backend is too new" error.
vmt::Result<std::vector<DumpRow>> dump_rows(const char* dump_kind, const std::string& text) {
    const int version = dump_protocol_version(text);
    if (version == 0) {
        return vmt::Err<std::vector<DumpRow>>(protocol_error(
            dump_kind, "missing or malformed protocol header (expected \"VMTStudio-IPC\\t2\")"));
    }
    if (version > kSupportedProtocolVersion) {
        return vmt::Err<std::vector<DumpRow>>(protocol_error(
            dump_kind, "protocol version " + std::to_string(version) +
                           " is newer than this build supports (" +
                           std::to_string(kSupportedProtocolVersion) + "): the backend is too new"));
    }

    const std::vector<std::string> lines = split_lines(text);
    std::vector<DumpRow> rows;
    bool kind_seen = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) continue;  // blank filler between rows
        if (!kind_seen) {
            const std::vector<std::string> fields = split_tabs(line);
            if (fields.size() != 2 || fields[0] != "kind") {
                return vmt::Err<std::vector<DumpRow>>(protocol_error(
                    dump_kind, "line " + std::to_string(i + 1) +
                                   ": expected the \"kind\" row, got \"" + line + "\""));
            }
            if (fields[1] != dump_kind) {
                return vmt::Err<std::vector<DumpRow>>(protocol_error(
                    dump_kind, "line " + std::to_string(i + 1) + ": dump kind is \"" + fields[1] +
                                   "\", expected \"" + dump_kind + "\""));
            }
            kind_seen = true;
            continue;
        }
        rows.push_back(DumpRow{line, i + 1});
    }
    if (!kind_seen) {
        return vmt::Err<std::vector<DumpRow>>(
            protocol_error(dump_kind, "missing the \"kind\" row"));
    }
    return vmt::Ok(std::move(rows));
}

// Strict reader over the fields of one known row; the three dump parsers share
// its column and field error messages.
struct RowReader {
    RowReader(const char* dump_kind_, std::size_t line_, const std::vector<std::string>& fields_,
              vmt::Error& error_)
        : dump_kind(dump_kind_), line(line_), fields(fields_), error(error_) {}

    // The row must have exactly `count` TAB separated fields.
    bool expect(std::size_t count) const {
        if (fields.size() == count) return true;
        error = row_error(dump_kind, line, fields[0],
                          "expected " + std::to_string(count) + " fields, got " +
                              std::to_string(fields.size()));
        return false;
    }

    bool as_int(std::size_t index, const char* name, int& out) const {
        if (parse_int(fields[index], out)) return true;
        error = bad_value(index, name, "an integer");
        return false;
    }

    bool as_float(std::size_t index, const char* name, float& out) const {
        if (parse_float(fields[index], out)) return true;
        error = bad_value(index, name, "a number");
        return false;
    }

    bool as_flag(std::size_t index, const char* name, bool& out) const {
        if (parse_flag(fields[index], out)) return true;
        error = bad_value(index, name, "0 or 1");
        return false;
    }

    // "loose" / "vpk" of a `model` row.
    bool as_source(std::size_t index, bool& loose) const {
        if (fields[index] == "loose") {
            loose = true;
            return true;
        }
        if (fields[index] == "vpk") {
            loose = false;
            return true;
        }
        error = bad_value(index, "source", "\"loose\" or \"vpk\"");
        return false;
    }

private:
    vmt::Error bad_value(std::size_t index, const char* name, const char* expected) const {
        return row_error(dump_kind, line, fields[0], std::string("field \"") + name + "\" is not " +
                                                      expected + ": \"" + fields[index] + "\"");
    }

    const char* dump_kind;
    std::size_t line;
    const std::vector<std::string>& fields;
    vmt::Error& error;
};

} // namespace

int dump_protocol_version(const std::string& text) {
    const std::string line = first_line(text);
    if (line.size() <= kHeaderPrefixLength) return 0;
    if (line.compare(0, kHeaderPrefixLength, kHeaderPrefix) != 0) return 0;

    int version = 0;
    if (!parse_int(line.substr(kHeaderPrefixLength), version)) return 0;
    if (version < 1) return 0;  // 0 / negative: no usable version
    return version;
}

vmt::Result<ModelList> parse_models_dump(const std::string& text) {
    vmt::Result<std::vector<DumpRow>> rows = dump_rows("models", text);
    if (!rows.ok()) {
        return vmt::Err<ModelList>(rows.error());
    }

    ModelList models;
    for (const DumpRow& row : rows.value()) {
        const std::vector<std::string> fields = split_tabs(row.text);
        if (fields[0] != "model") continue;  // forward compatible: ignore unknown rows

        vmt::Error error;
        const RowReader reader("models", row.line, fields, error);
        ModelEntry model;
        if (!reader.expect(3) || !reader.as_source(2, model.loose)) {
            return vmt::Err<ModelList>(std::move(error));
        }
        model.name = fields[1];
        models.entries.push_back(std::move(model));
    }
    return vmt::Ok(std::move(models));
}

vmt::Result<ModelScene> parse_model_scene_dump(const std::string& text) {
    vmt::Result<std::vector<DumpRow>> rows = dump_rows("model_scene", text);
    if (!rows.ok()) {
        return vmt::Err<ModelScene>(rows.error());
    }

    ModelScene scene;
    for (const DumpRow& row : rows.value()) {
        const std::vector<std::string> fields = split_tabs(row.text);
        const std::string& kind = fields[0];
        vmt::Error error;
        const RowReader reader("model_scene", row.line, fields, error);

        if (kind == "info") {
            // The schema writes "info\tname\t<path>"; the literal "name"
            // column and the short "info\t<path>" row are both accepted so
            // writers on either side of that ambiguity interoperate. An empty
            // path is valid and means "no model loaded".
            if (fields.size() == 3 && fields[1] == "name") {
                scene.model_name = fields[2];
            } else if (fields.size() == 2) {
                scene.model_name = fields[1];
            } else {
                error = row_error("model_scene", row.line, kind,
                                  "expected \"info<tab>name<tab><path>\" or \"info<tab><path>\"");
                return vmt::Err<ModelScene>(std::move(error));
            }
        } else if (kind == "stats") {
            ModelStats stats;
            if (!reader.expect(12) || !reader.as_int(1, "polys", stats.polys) ||
                !reader.as_int(2, "batches", stats.batches) ||
                !reader.as_int(3, "bones", stats.bones) ||
                !reader.as_int(4, "lods", stats.lods) ||
                !reader.as_int(5, "materials", stats.materials) ||
                !reader.as_float(6, "min x", stats.bbox_min[0]) ||
                !reader.as_float(7, "min y", stats.bbox_min[1]) ||
                !reader.as_float(8, "min z", stats.bbox_min[2]) ||
                !reader.as_float(9, "max x", stats.bbox_max[0]) ||
                !reader.as_float(10, "max y", stats.bbox_max[1]) ||
                !reader.as_float(11, "max z", stats.bbox_max[2])) {
                return vmt::Err<ModelScene>(std::move(error));
            }
            scene.stats = stats;
        } else if (kind == "material") {
            ModelMaterial material;
            if (!reader.expect(4) || !reader.as_int(1, "texture index", material.texture_index)) {
                return vmt::Err<ModelScene>(std::move(error));
            }
            material.name = fields[2];
            material.shader = fields[3];
            scene.materials.push_back(std::move(material));
        } else if (kind == "use") {
            ModelMaterialUse use;
            if (!reader.expect(5) || !reader.as_int(1, "texture index", use.texture_index) ||
                !reader.as_int(2, "bodypart index", use.bodypart) ||
                !reader.as_int(3, "submodel index", use.submodel)) {
                return vmt::Err<ModelScene>(std::move(error));
            }
            use.bodypart_name = fields[4];
            scene.material_uses.push_back(std::move(use));
        } else if (kind == "sequence") {
            ModelSequence sequence;
            if (!reader.expect(9) || !reader.as_int(1, "index", sequence.index) ||
                !reader.as_float(4, "fps", sequence.fps) ||
                !reader.as_int(5, "frames", sequence.frames) ||
                !reader.as_float(6, "duration", sequence.duration) ||
                !reader.as_flag(7, "looping", sequence.looping) ||
                !reader.as_flag(8, "hidden", sequence.hidden)) {
                return vmt::Err<ModelScene>(std::move(error));
            }
            sequence.label = fields[2];
            sequence.activity = fields[3];
            scene.sequences.push_back(std::move(sequence));
        } else if (kind == "bodypart") {
            ModelBodypart bodypart;
            if (!reader.expect(5) || !reader.as_int(1, "index", bodypart.index) ||
                !reader.as_int(3, "submodel count", bodypart.models) ||
                !reader.as_int(4, "current submodel", bodypart.current)) {
                return vmt::Err<ModelScene>(std::move(error));
            }
            bodypart.name = fields[2];
            scene.bodyparts.push_back(std::move(bodypart));
        } else if (kind == "skin") {
            ModelSkin skin;
            if (!reader.expect(3) || !reader.as_int(1, "index", skin.index)) {
                return vmt::Err<ModelScene>(std::move(error));
            }
            skin.name = fields[2];
            scene.skins.push_back(std::move(skin));
        } else if (kind == "bone") {
            ModelBone bone;
            if (!reader.expect(4) || !reader.as_int(1, "index", bone.index) ||
                !reader.as_int(3, "parent index", bone.parent)) {
                return vmt::Err<ModelScene>(std::move(error));
            }
            bone.name = fields[2];
            scene.bones.push_back(std::move(bone));
        } else if (kind == "attachment") {
            ModelAttachment attachment;
            if (!reader.expect(3) || !reader.as_int(1, "index", attachment.index)) {
                return vmt::Err<ModelScene>(std::move(error));
            }
            attachment.name = fields[2];
            scene.attachments.push_back(std::move(attachment));
        } else if (kind == "pose") {
            ModelPoseParameter pose;
            if (!reader.expect(5) || !reader.as_int(1, "index", pose.index) ||
                !reader.as_float(3, "min value", pose.min_value) ||
                !reader.as_float(4, "max value", pose.max_value)) {
                return vmt::Err<ModelScene>(std::move(error));
            }
            pose.name = fields[2];
            scene.pose_parameters.push_back(std::move(pose));
        }
        // Unknown row kinds (for example "note") are ignored.
    }
    return vmt::Ok(std::move(scene));
}

vmt::Result<AnimState> parse_anim_state_dump(const std::string& text) {
    vmt::Result<std::vector<DumpRow>> rows = dump_rows("anim_state", text);
    if (!rows.ok()) {
        return vmt::Err<AnimState>(rows.error());
    }

    AnimState state;  // defaults: frame 0, paused
    for (const DumpRow& row : rows.value()) {
        const std::vector<std::string> fields = split_tabs(row.text);
        if (fields[0] != "state") continue;

        vmt::Error error;
        const RowReader reader("anim_state", row.line, fields, error);
        AnimState parsed;
        if (!reader.expect(7) || !reader.as_int(1, "sequence", parsed.sequence) ||
            !reader.as_float(2, "frame", parsed.frame) ||
            !reader.as_float(3, "cycle", parsed.cycle) ||
            !reader.as_float(4, "speed", parsed.speed) ||
            !reader.as_float(5, "ground speed", parsed.ground_speed) ||
            !reader.as_flag(6, "paused", parsed.paused)) {
            return vmt::Err<AnimState>(std::move(error));
        }
        state = parsed;
    }
    return vmt::Ok(std::move(state));
}

} // namespace bridge
