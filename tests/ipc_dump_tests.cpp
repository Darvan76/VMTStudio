#include "ut.h"

#include "bridge/ipc_dump.h"

#include <cstddef>
#include <string>
#include <vector>

// Parser tests for the response files of the IPC protocol v2. The dumps are
// hand-written TSV texts matching docs/IPC_PROTOCOL.md; the same entry points
// the engine bridge uses are exercised here.

namespace {

// Header + kind row + the given rows, LF terminated.
std::string dump_text(const char* kind, const std::vector<std::string>& rows) {
    std::string text = "VMTStudio-IPC\t2\nkind\t";
    text += kind;
    text += "\n";
    for (const std::string& row : rows) {
        text += row;
        text += "\n";
    }
    return text;
}

// The same text with CRLF line endings.
std::string with_crlf(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

UT_TEST_CASE("ipc_dump: version detection accepts the current one and reports the rest") {
    const std::string valid =
        dump_text("models", { "model\tmodels/props_c17/oildrum001.mdl\tloose" });
    UT_CHECK_EQ(bridge::dump_protocol_version(valid), 2);
    UT_CHECK_EQ(bridge::dump_protocol_version(with_crlf(valid)), 2);

    UT_CHECK_EQ(bridge::dump_protocol_version(""), 0);
    UT_CHECK_EQ(bridge::dump_protocol_version("kind\tmodels\n"), 0);
    UT_CHECK_EQ(bridge::dump_protocol_version("VMTStudio-IPC\t"), 0);
    UT_CHECK_EQ(bridge::dump_protocol_version("VMTStudio-IPC\t2x\n"), 0);
    UT_CHECK_EQ(bridge::dump_protocol_version("VMTStudio-IPC\t0\n"), 0);
    UT_CHECK_EQ(bridge::dump_protocol_version("VMTStudio-IPC\t-1\n"), 0);
    UT_CHECK_EQ(bridge::dump_protocol_version("VMTStudio-IPC\t3\n"), 3);

    // A dump without a header cannot be parsed.
    const vmt::Result<bridge::ModelList> missing = bridge::parse_models_dump("kind\tmodels\n");
    UT_CHECK(!missing.ok());
    UT_CHECK_EQ(missing.error().code, vmt::ErrorCode::ParseError);

    // A dump from a newer backend is refused with a clear error.
    const vmt::Result<bridge::ModelList> newer =
        bridge::parse_models_dump("VMTStudio-IPC\t3\nkind\tmodels\n");
    UT_CHECK(!newer.ok());
    UT_CHECK_EQ(newer.error().code, vmt::ErrorCode::ParseError);
    UT_CHECK(contains(newer.error().message, "too new"));
}

UT_TEST_CASE("ipc_dump: the kind row is validated") {
    const vmt::Result<bridge::ModelList> wrong_kind =
        bridge::parse_models_dump("VMTStudio-IPC\t2\nkind\tanim_state\n");
    UT_CHECK(!wrong_kind.ok());
    UT_CHECK_EQ(wrong_kind.error().code, vmt::ErrorCode::ParseError);
    UT_CHECK(contains(wrong_kind.error().message, "anim_state"));

    const vmt::Result<bridge::ModelList> no_kind = bridge::parse_models_dump(
        "VMTStudio-IPC\t2\nmodel\tmodels/props_c17/oildrum001.mdl\tloose\n");
    UT_CHECK(!no_kind.ok());
    UT_CHECK_EQ(no_kind.error().code, vmt::ErrorCode::ParseError);
}

UT_TEST_CASE("ipc_dump: models: loose and vpk entries parse") {
    const std::string text = dump_text("models", {
        "model\tmodels/props_c17/oildrum001.mdl\tloose",
        "model\tmodels/props_junk/wood_crate001a.mdl\tvpk",
    });
    const vmt::Result<bridge::ModelList> parsed = bridge::parse_models_dump(text);
    UT_CHECK(parsed.ok());
    if (!parsed.ok()) return;

    const bridge::ModelList& models = parsed.value();
    UT_CHECK_EQ(models.entries.size(), std::size_t{2});
    if (models.entries.size() != std::size_t{2}) return;
    UT_CHECK_EQ(models.entries[0].name, "models/props_c17/oildrum001.mdl");
    UT_CHECK(models.entries[0].loose);
    UT_CHECK_EQ(models.entries[1].name, "models/props_junk/wood_crate001a.mdl");
    UT_CHECK(!models.entries[1].loose);
    UT_CHECK(!models.truncated);
}

UT_TEST_CASE("ipc_dump: models: unknown rows are ignored") {
    const std::string text = dump_text("models", {
        "note\ttruncated after the safety cap",
        "model\tmodels/props_c17/oildrum001.mdl\tloose",
        "future_row\t1\t2\t3\t4\t5",
    });
    const vmt::Result<bridge::ModelList> parsed = bridge::parse_models_dump(text);
    UT_CHECK(parsed.ok());
    if (!parsed.ok()) return;
    UT_CHECK_EQ(parsed.value().entries.size(), std::size_t{1});
}

UT_TEST_CASE("ipc_dump: models: the truncation note is exposed") {
    const vmt::Result<bridge::ModelList> capped = bridge::parse_models_dump(dump_text("models", {
        "model\tmodels/props_c17/oildrum001.mdl\tloose",
        "note\ttruncated",
    }));
    UT_CHECK(capped.ok());
    if (!capped.ok()) return;
    UT_CHECK(capped.value().truncated);
    UT_CHECK_EQ(capped.value().entries.size(), std::size_t{1});

    const vmt::Result<bridge::ModelList> whole = bridge::parse_models_dump(dump_text("models", {
        "model\tmodels/props_c17/oildrum001.mdl\tloose",
    }));
    UT_CHECK(whole.ok());
    if (!whole.ok()) return;
    UT_CHECK(!whole.value().truncated);
}

UT_TEST_CASE("ipc_dump: models: malformed rows fail with ParseError") {
    const vmt::Result<bridge::ModelList> missing_column = bridge::parse_models_dump(dump_text("models", {
        "model\tmodels/props_c17/oildrum001.mdl\tloose",
        "model\tmodels/props_junk/broken.mdl",
    }));
    UT_CHECK(!missing_column.ok());
    UT_CHECK_EQ(missing_column.error().code, vmt::ErrorCode::ParseError);
    UT_CHECK(contains(missing_column.error().message, "line 4"));
    UT_CHECK(contains(missing_column.error().message, "model"));

    const vmt::Result<bridge::ModelList> bad_source = bridge::parse_models_dump(dump_text("models", {
        "model\tmodels/props_junk/broken.mdl\tfloppy",
    }));
    UT_CHECK(!bad_source.ok());
    UT_CHECK_EQ(bad_source.error().code, vmt::ErrorCode::ParseError);
}

UT_TEST_CASE("ipc_dump: models: an empty list is valid") {
    const vmt::Result<bridge::ModelList> parsed =
        bridge::parse_models_dump("VMTStudio-IPC\t2\nkind\tmodels\n");
    UT_CHECK(parsed.ok());
    if (!parsed.ok()) return;
    UT_CHECK(parsed.value().entries.empty());
}

UT_TEST_CASE("ipc_dump: model_scene: every row kind parses") {
    const std::string text = dump_text("model_scene", {
        "info\tname\tmodels/props_c17/oildrum001.mdl",
        "stats\t1234\t7\t21\t3\t4\t-40.5\t-32.25\t0\t40.5\t32.25\t64",
        "material\t0\tprops_c17/oildrum001\tLightmappedGeneric",
        "material\t1\tprops_c17/metal_drum_hose\t",
        "use\t0\t0\t0\toildrum",
        "use\t1\t1\t2\toil drum hose",
        "sequence\t0\tidle\tACT_IDLE\t30.5\t120\t3.5\t0\t1",
        "sequence\t1\twalk\t\t24\t48\t2\t1\t0",
        "bodypart\t0\toildrum\t2\t1",
        "skin\t0\tdefault",
        "skin\t1\tblue",
        "bone\t0\troot\t-1",
        "bone\t1\tupper body\t0",
        "attachment\t0\tmuzzle",
        "pose\t0\tlook_pitch\t-45\t45",
    });
    const vmt::Result<bridge::ModelScene> parsed = bridge::parse_model_scene_dump(text);
    UT_CHECK(parsed.ok());
    if (!parsed.ok()) return;
    const bridge::ModelScene& scene = parsed.value();

    UT_CHECK_EQ(scene.model_name, "models/props_c17/oildrum001.mdl");

    UT_CHECK_EQ(scene.stats.polys, 1234);
    UT_CHECK_EQ(scene.stats.batches, 7);
    UT_CHECK_EQ(scene.stats.bones, 21);
    UT_CHECK_EQ(scene.stats.lods, 3);
    UT_CHECK_EQ(scene.stats.materials, 4);
    UT_CHECK_EQ(scene.stats.bbox_min[0], -40.5f);
    UT_CHECK_EQ(scene.stats.bbox_min[1], -32.25f);
    UT_CHECK_EQ(scene.stats.bbox_min[2], 0.0f);
    UT_CHECK_EQ(scene.stats.bbox_max[0], 40.5f);
    UT_CHECK_EQ(scene.stats.bbox_max[1], 32.25f);
    UT_CHECK_EQ(scene.stats.bbox_max[2], 64.0f);

    UT_CHECK_EQ(scene.materials.size(), std::size_t{2});
    if (scene.materials.size() == std::size_t{2}) {
        UT_CHECK_EQ(scene.materials[0].texture_index, 0);
        UT_CHECK_EQ(scene.materials[0].name, "props_c17/oildrum001");
        UT_CHECK_EQ(scene.materials[0].shader, "LightmappedGeneric");
        UT_CHECK_EQ(scene.materials[1].texture_index, 1);
        UT_CHECK_EQ(scene.materials[1].name, "props_c17/metal_drum_hose");
        UT_CHECK(scene.materials[1].shader.empty());
    }

    UT_CHECK_EQ(scene.material_uses.size(), std::size_t{2});
    if (scene.material_uses.size() == std::size_t{2}) {
        UT_CHECK_EQ(scene.material_uses[0].texture_index, 0);
        UT_CHECK_EQ(scene.material_uses[0].bodypart, 0);
        UT_CHECK_EQ(scene.material_uses[0].submodel, 0);
        UT_CHECK_EQ(scene.material_uses[0].bodypart_name, "oildrum");
        UT_CHECK_EQ(scene.material_uses[1].texture_index, 1);
        UT_CHECK_EQ(scene.material_uses[1].bodypart, 1);
        UT_CHECK_EQ(scene.material_uses[1].submodel, 2);
        UT_CHECK_EQ(scene.material_uses[1].bodypart_name, "oil drum hose");
    }

    UT_CHECK_EQ(scene.sequences.size(), std::size_t{2});
    if (scene.sequences.size() == std::size_t{2}) {
        const bridge::ModelSequence& idle = scene.sequences[0];
        UT_CHECK_EQ(idle.index, 0);
        UT_CHECK_EQ(idle.label, "idle");
        UT_CHECK_EQ(idle.activity, "ACT_IDLE");
        UT_CHECK_EQ(idle.fps, 30.5f);
        UT_CHECK_EQ(idle.frames, 120);
        UT_CHECK_EQ(idle.duration, 3.5f);
        UT_CHECK(!idle.looping);
        UT_CHECK(idle.hidden);

        const bridge::ModelSequence& walk = scene.sequences[1];
        UT_CHECK_EQ(walk.index, 1);
        UT_CHECK_EQ(walk.label, "walk");
        UT_CHECK(walk.activity.empty());
        UT_CHECK_EQ(walk.fps, 24.0f);
        UT_CHECK_EQ(walk.frames, 48);
        UT_CHECK_EQ(walk.duration, 2.0f);
        UT_CHECK(walk.looping);
        UT_CHECK(!walk.hidden);
    }

    UT_CHECK_EQ(scene.bodyparts.size(), std::size_t{1});
    if (scene.bodyparts.size() == std::size_t{1}) {
        UT_CHECK_EQ(scene.bodyparts[0].index, 0);
        UT_CHECK_EQ(scene.bodyparts[0].name, "oildrum");
        UT_CHECK_EQ(scene.bodyparts[0].models, 2);
        UT_CHECK_EQ(scene.bodyparts[0].current, 1);
    }

    UT_CHECK_EQ(scene.skins.size(), std::size_t{2});
    if (scene.skins.size() == std::size_t{2}) {
        UT_CHECK_EQ(scene.skins[0].index, 0);
        UT_CHECK_EQ(scene.skins[0].name, "default");
        UT_CHECK_EQ(scene.skins[1].index, 1);
        UT_CHECK_EQ(scene.skins[1].name, "blue");
    }

    UT_CHECK_EQ(scene.bones.size(), std::size_t{2});
    if (scene.bones.size() == std::size_t{2}) {
        UT_CHECK_EQ(scene.bones[0].index, 0);
        UT_CHECK_EQ(scene.bones[0].name, "root");
        UT_CHECK_EQ(scene.bones[0].parent, -1);
        UT_CHECK_EQ(scene.bones[1].index, 1);
        UT_CHECK_EQ(scene.bones[1].name, "upper body");
        UT_CHECK_EQ(scene.bones[1].parent, 0);
    }

    UT_CHECK_EQ(scene.attachments.size(), std::size_t{1});
    if (scene.attachments.size() == std::size_t{1}) {
        UT_CHECK_EQ(scene.attachments[0].index, 0);
        UT_CHECK_EQ(scene.attachments[0].name, "muzzle");
    }

    UT_CHECK_EQ(scene.pose_parameters.size(), std::size_t{1});
    if (scene.pose_parameters.size() == std::size_t{1}) {
        UT_CHECK_EQ(scene.pose_parameters[0].index, 0);
        UT_CHECK_EQ(scene.pose_parameters[0].name, "look_pitch");
        UT_CHECK_EQ(scene.pose_parameters[0].min_value, -45.0f);
        UT_CHECK_EQ(scene.pose_parameters[0].max_value, 45.0f);
    }
}

UT_TEST_CASE("ipc_dump: model_scene: info without the name column and no model") {
    // The schema line is "info\tname\t<path>"; the short "info\t<path>" row is
    // accepted too, so writers on either side of that ambiguity interoperate.
    const vmt::Result<bridge::ModelScene> short_info = bridge::parse_model_scene_dump(
        dump_text("model_scene", { "info\tmodels/props_junk/watermelon01.mdl" }));
    UT_CHECK(short_info.ok());
    if (short_info.ok()) {
        UT_CHECK_EQ(short_info.value().model_name, "models/props_junk/watermelon01.mdl");
    }

    // An empty dump is valid and means "no model loaded".
    const vmt::Result<bridge::ModelScene> no_model =
        bridge::parse_model_scene_dump("VMTStudio-IPC\t2\nkind\tmodel_scene\n");
    UT_CHECK(no_model.ok());
    if (!no_model.ok()) return;
    UT_CHECK(no_model.value().model_name.empty());
    UT_CHECK(no_model.value().materials.empty());
    UT_CHECK(no_model.value().material_uses.empty());
    UT_CHECK(no_model.value().sequences.empty());
    UT_CHECK(no_model.value().bodyparts.empty());
    UT_CHECK(no_model.value().skins.empty());
    UT_CHECK(no_model.value().bones.empty());
    UT_CHECK(no_model.value().attachments.empty());
    UT_CHECK(no_model.value().pose_parameters.empty());

    // An info row with an empty path is valid as well.
    const vmt::Result<bridge::ModelScene> empty_path =
        bridge::parse_model_scene_dump(dump_text("model_scene", { "info\tname\t" }));
    UT_CHECK(empty_path.ok());
    if (empty_path.ok()) {
        UT_CHECK(empty_path.value().model_name.empty());
    }
}

UT_TEST_CASE("ipc_dump: model_scene: unknown rows are ignored") {
    const vmt::Result<bridge::ModelScene> parsed =
        bridge::parse_model_scene_dump(dump_text("model_scene", {
            "note\tmaterial table truncated",
            "info\tname\tmodels/props_c17/oildrum001.mdl",
            "totally_new\t1\t2\t3",
        }));
    UT_CHECK(parsed.ok());
    if (!parsed.ok()) return;
    UT_CHECK_EQ(parsed.value().model_name, "models/props_c17/oildrum001.mdl");
    UT_CHECK(parsed.value().materials.empty());
}

UT_TEST_CASE("ipc_dump: model_scene: malformed rows fail with ParseError") {
    const vmt::Result<bridge::ModelScene> missing_column =
        bridge::parse_model_scene_dump(dump_text("model_scene", {
            "material\t0\tprops_c17/oildrum001\tLightmappedGeneric",
            "sequence\t0\tidle\tACT_IDLE\t30\t120\t3.5\t0",
        }));
    UT_CHECK(!missing_column.ok());
    UT_CHECK_EQ(missing_column.error().code, vmt::ErrorCode::ParseError);
    UT_CHECK(contains(missing_column.error().message, "line 4"));
    UT_CHECK(contains(missing_column.error().message, "sequence"));

    const vmt::Result<bridge::ModelScene> bad_fps =
        bridge::parse_model_scene_dump(dump_text("model_scene", {
            "sequence\t0\tidle\t\tfast\t120\t3.5\t0\t1",
        }));
    UT_CHECK(!bad_fps.ok());
    UT_CHECK_EQ(bad_fps.error().code, vmt::ErrorCode::ParseError);
    UT_CHECK(contains(bad_fps.error().message, "fps"));

    const vmt::Result<bridge::ModelScene> bad_stats =
        bridge::parse_model_scene_dump(dump_text("model_scene", {
            "stats\t1\t2\t3\t4\t5\t0\t0\t0\t1\t1",
        }));
    UT_CHECK(!bad_stats.ok());
    UT_CHECK_EQ(bad_stats.error().code, vmt::ErrorCode::ParseError);
}

UT_TEST_CASE("ipc_dump: anim_state: happy path and paused flag") {
    const vmt::Result<bridge::AnimState> running =
        bridge::parse_anim_state_dump(dump_text("anim_state", {
            "state\t2\t42.5\t0.35\t1.5\t120\t0",
        }));
    UT_CHECK(running.ok());
    if (running.ok()) {
        UT_CHECK_EQ(running.value().sequence, 2);
        UT_CHECK_EQ(running.value().frame, 42.5f);
        UT_CHECK_EQ(running.value().cycle, 0.35f);
        UT_CHECK_EQ(running.value().speed, 1.5f);
        UT_CHECK_EQ(running.value().ground_speed, 120.0f);
        UT_CHECK(!running.value().paused);
    }

    const vmt::Result<bridge::AnimState> paused =
        bridge::parse_anim_state_dump(dump_text("anim_state", {
            "state\t0\t0\t0\t1\t0\t1",
        }));
    UT_CHECK(paused.ok());
    if (paused.ok()) {
        UT_CHECK(paused.value().paused);
    }

    // A dump without a state row keeps the neutral defaults (frame 0, running).
    const vmt::Result<bridge::AnimState> empty =
        bridge::parse_anim_state_dump("VMTStudio-IPC\t2\nkind\tanim_state\n");
    UT_CHECK(empty.ok());
    if (empty.ok()) {
        UT_CHECK_EQ(empty.value().sequence, 0);
        UT_CHECK_EQ(empty.value().speed, 1.0f);
        UT_CHECK(!empty.value().paused);
    }
}

UT_TEST_CASE("ipc_dump: anim_state: malformed rows fail with ParseError") {
    const vmt::Result<bridge::AnimState> missing_column =
        bridge::parse_anim_state_dump(dump_text("anim_state", {
            "state\t0\t0\t0\t1\t0",
        }));
    UT_CHECK(!missing_column.ok());
    UT_CHECK_EQ(missing_column.error().code, vmt::ErrorCode::ParseError);
    UT_CHECK(contains(missing_column.error().message, "state"));

    const vmt::Result<bridge::AnimState> bad_flag =
        bridge::parse_anim_state_dump(dump_text("anim_state", {
            "state\t0\t0\t0\t1\t0\tyes",
        }));
    UT_CHECK(!bad_flag.ok());
    UT_CHECK_EQ(bad_flag.error().code, vmt::ErrorCode::ParseError);
    UT_CHECK(contains(bad_flag.error().message, "paused"));
}

UT_TEST_CASE("ipc_dump: CRLF line endings and names with spaces") {
    const std::string text = with_crlf(dump_text("models", {
        "model\tmodels/props c17/oil drum.mdl\tloose",
    }));
    const vmt::Result<bridge::ModelList> parsed = bridge::parse_models_dump(text);
    UT_CHECK(parsed.ok());
    if (!parsed.ok()) return;
    UT_CHECK_EQ(parsed.value().entries.size(), std::size_t{1});
    if (parsed.value().entries.size() == std::size_t{1}) {
        // The \r of the CRLF ending must not leak into the last field.
        UT_CHECK_EQ(parsed.value().entries[0].name, "models/props c17/oil drum.mdl");
    }

    const std::string scene_text = with_crlf(dump_text("model_scene", {
        "info\tname\tmodels/rock wall/rock wall.mdl",
        "bone\t0\tleft arm\t-1",
    }));
    const vmt::Result<bridge::ModelScene> scene = bridge::parse_model_scene_dump(scene_text);
    UT_CHECK(scene.ok());
    if (!scene.ok()) return;
    UT_CHECK_EQ(scene.value().model_name, "models/rock wall/rock wall.mdl");
    UT_CHECK_EQ(scene.value().bones.size(), std::size_t{1});
    if (scene.value().bones.size() == std::size_t{1}) {
        UT_CHECK_EQ(scene.value().bones[0].name, "left arm");
        UT_CHECK_EQ(scene.value().bones[0].parent, -1);
    }
}
