#include "ut.h"

#include "vmt/schema.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

const char* const kShaderNames[] = {
    "VertexLitGeneric",
    "LightmappedGeneric",
    "UnlitGeneric",
    "UnlitTwoTexture",
    "WorldVertexTransition",
    "Refract",
    "Water",
    "Sprite",
};

// Test (6): malformed JSON must fail at the JSON layer with a position.
void expect_load_error(const char* src) {
    auto r = vmt::SchemaLibrary::load_json(src, "inline.json");
    UT_CHECK(!r.ok());
    if (!r.ok()) {
        UT_CHECK(r.error().code == vmt::ErrorCode::SchemaLoadError);
        UT_CHECK(r.error().file == "inline.json");
        UT_CHECK(r.error().line >= 1u);
        UT_CHECK(r.error().column >= 1u);
        UT_CHECK(!r.error().message.empty());
    }
}

// Test (7): well-formed JSON that violates the schema contract.
void expect_validation_error(const char* src) {
    auto r = vmt::SchemaLibrary::load_json(src, "inline.json");
    UT_CHECK(!r.ok());
    if (!r.ok()) {
        UT_CHECK(r.error().code == vmt::ErrorCode::SchemaValidationError);
        UT_CHECK(r.error().line >= 1u);
        UT_CHECK(r.error().column >= 1u);
        UT_CHECK(!r.error().message.empty());
    }
}

} // namespace

UT_TEST_CASE("schema: all shipped schema files load and validate") {
    auto r = vmt::SchemaLibrary::load_dir(VMT_SCHEMA_DIR);
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    const vmt::SchemaLibrary& lib = r.value();

    UT_CHECK(lib.shader_names().size() == 8u);
    for (const char* name : kShaderNames) {
        UT_CHECK(lib.has_shader(name));
        const vmt::ShaderSchema s = lib.schema_for(name);
        UT_CHECK(!s.empty());
        UT_CHECK(!s.shader().empty());
        UT_CHECK(!s.groups().empty());
        for (const vmt::SchemaGroup& g : s.groups()) {
            UT_CHECK(!g.name.empty());
            UT_CHECK(!g.parameters.empty());
            for (const vmt::SchemaParam& p : g.parameters) {
                UT_CHECK(!p.name.empty());
            }
        }
    }
}

UT_TEST_CASE("schema: lookup is case-insensitive and preserves canonical name") {
    auto r = vmt::SchemaLibrary::load_dir(VMT_SCHEMA_DIR);
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    const vmt::SchemaLibrary& lib = r.value();

    const vmt::ShaderSchema vlg = lib.schema_for("vertexlitgeneric");
    UT_CHECK(vlg.shader() == "VertexLitGeneric");

    const vmt::SchemaParam* base = vlg.find_param("$BASETEXTURE");
    UT_CHECK(base != nullptr);
    if (base != nullptr) {
        UT_CHECK(base->name == "$basetexture");
        UT_CHECK(base->type == vmt::ParamType::Texture);
    }

    const vmt::ShaderSchema lmg = lib.schema_for("LIGHTMAPPEDGENERIC");
    UT_CHECK(lmg.shader() == "LightmappedGeneric");
    const vmt::SchemaGroup* detail = lmg.find_group("DETAIL");
    UT_CHECK(detail != nullptr);
    if (detail != nullptr) {
        UT_CHECK(detail->name == "Detail");
    }
    const vmt::SchemaParam* d = lmg.find_param("$detail");
    UT_CHECK(d != nullptr);
    if (d != nullptr) {
        UT_CHECK(d->type == vmt::ParamType::Texture);
    }

    UT_CHECK(vlg.find_param("$no_such_param_zzz") == nullptr);
    UT_CHECK(vlg.find_group("NoSuchGroupZzz") == nullptr);
}

UT_TEST_CASE("schema: unknown shader returns empty schema without error") {
    auto r = vmt::SchemaLibrary::load_dir(VMT_SCHEMA_DIR);
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    const vmt::SchemaLibrary& lib = r.value();

    const vmt::ShaderSchema s = lib.schema_for("ThisShaderDoesNotExist");
    UT_CHECK(s.empty());
    UT_CHECK(s.shader().empty());
    UT_CHECK(s.groups().empty());
    UT_CHECK(s.needs_review().empty());
    UT_CHECK(!lib.has_shader("ThisShaderDoesNotExist"));

    const vmt::ShaderSchema empty_name = lib.schema_for("");
    UT_CHECK(empty_name.empty());
    UT_CHECK(empty_name.shader().empty());
}

UT_TEST_CASE("schema: grouped parameter list is exposed per shader") {
    auto r = vmt::SchemaLibrary::load_dir(VMT_SCHEMA_DIR);
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    const vmt::ShaderSchema lmg = r.value().schema_for("LightmappedGeneric");

    UT_CHECK(lmg.groups().size() >= 4u);
    std::size_t total = 0;
    for (const vmt::SchemaGroup& g : lmg.groups()) {
        total += g.parameters.size();
    }
    UT_CHECK(total >= 15u);

    const vmt::SchemaGroup* detail = lmg.find_group("Detail");
    UT_CHECK(detail != nullptr);
    if (detail != nullptr) {
        UT_CHECK(detail->parameters.size() >= 3u);
    }

    const vmt::SchemaParam* bms = lmg.find_param("$bumpmapscale");
    UT_CHECK(bms != nullptr);
    if (bms != nullptr) {
        UT_CHECK(bms->type == vmt::ParamType::Float);
    }
}

UT_TEST_CASE("schema: needs_review is separate from confirmed groups") {
    auto r = vmt::SchemaLibrary::load_dir(VMT_SCHEMA_DIR);
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    const vmt::ShaderSchema vlg = r.value().schema_for("VertexLitGeneric");

    UT_CHECK(!vlg.needs_review().empty());
    for (const vmt::NeedsReviewParam& nr : vlg.needs_review()) {
        UT_CHECK(!nr.name.empty());
        if (!nr.name.empty()) {
            const char first = nr.name.front();
            UT_CHECK(first == '$' || first == '%');
        }
        // needs_review entries must never surface as confirmed parameters.
        UT_CHECK(vlg.find_param(nr.name) == nullptr);
    }
}

UT_TEST_CASE("schema: malformed JSON is rejected with position") {
    const char* const cases[] = {
        "{",
        "{\"shader\": \"X\", }",
        "{\"shader\": \"X\" \"groups\": []}",
        "{\"shader\": \"X\", \"groups\": [}",
        "{\"shader\": \"X\", \"shader\": \"Y\"}",
        "{\"n\": 01}",
        "not json",
    };
    for (const char* src : cases) {
        expect_load_error(src);
    }
}

UT_TEST_CASE("schema: malformed schema contents are rejected with precise validation errors") {
    const char* const cases[] = {
        "{}",
        "{\"shader\": \"X\"}",
        "{\"shader\": 123, \"groups\": []}",
        "{\"shader\": \"X\", \"groups\": [], \"bogus\": 1}",
        "{\"shader\": \"X\", \"groups\": [{\"parameters\": []}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$a\"}]}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$a\", \"type\": \"integer\"}]}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$a\", \"type\": \"int\", \"values\": [\"x\"]}]}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$a\", \"type\": \"enum\"}]}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$a\", \"type\": \"bool\", \"min\": 0}]}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$a\", \"type\": \"float\", \"min\": 2, \"max\": 1}]}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$a\", \"type\": \"int\", \"default\": 1}]}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$a\", \"type\": \"int\"}, {\"name\": \"$A\", \"type\": \"int\"}]}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$a\", \"type\": \"int\"}]}], \"needs_review\": [{\"name\": \"$a\"}]}",
        "{\"shader\": \"X\", \"groups\": [{\"name\": \"G\", \"parameters\": [{\"name\": \"$bad name\", \"type\": \"int\"}]}]}",
    };
    for (const char* src : cases) {
        expect_validation_error(src);
    }
}

UT_TEST_CASE("schema: defaults, enum values and ranges parse") {
    const char* const valid =
        R"({"format":"vmt-schema/1","shader":"TestShader","groups":[{"name":"G","parameters":[)"
        R"({"name":"$a","type":"texture","default":"debug/debugempty","description":"a texture"},)"
        R"({"name":"$m","type":"enum","values":["none","mode1"]},)"
        R"({"name":"$r","type":"float","min":0,"max":1}]}],)"
        R"("needs_review":[{"name":"$u","reason":"unverified"}]})";

    auto r = vmt::SchemaLibrary::load_json(valid, "inline.json");
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    const vmt::ShaderSchema s = r.value().schema_for("TestShader");

    UT_CHECK(s.groups().size() == 1u);
    UT_CHECK(s.shader() == "TestShader");

    const vmt::SchemaParam* a = s.find_param("$a");
    UT_CHECK(a != nullptr);
    if (a != nullptr) {
        UT_CHECK(a->type == vmt::ParamType::Texture);
        UT_CHECK(a->has_default);
        UT_CHECK(a->default_value == "debug/debugempty");
        UT_CHECK(a->description == "a texture");
    }

    const vmt::SchemaParam* m = s.find_param("$m");
    UT_CHECK(m != nullptr);
    if (m != nullptr) {
        UT_CHECK(m->type == vmt::ParamType::Enum);
        UT_CHECK(m->values.size() == 2u);
    }

    const vmt::SchemaParam* rf = s.find_param("$r");
    UT_CHECK(rf != nullptr);
    if (rf != nullptr) {
        UT_CHECK(rf->type == vmt::ParamType::Float);
        UT_CHECK(rf->has_range);
        UT_CHECK(rf->min_value == 0.0);
        UT_CHECK(rf->max_value == 1.0);
    }

    UT_CHECK(s.needs_review().size() == 1u);
    if (s.needs_review().size() == 1u) {
        UT_CHECK(s.needs_review()[0].name == "$u");
        UT_CHECK(s.needs_review()[0].reason == "unverified");
    }
}

UT_TEST_CASE("schema: load_dir on a missing directory fails with SchemaNotFound") {
    const std::filesystem::path path =
        std::filesystem::path(VMT_TEST_DATA_DIR) / "no_such_schema_dir_zzz";
    auto r = vmt::SchemaLibrary::load_dir(path);
    UT_CHECK(!r.ok());
    if (!r.ok()) {
        UT_CHECK(r.error().code == vmt::ErrorCode::SchemaNotFound);
    }
}

UT_TEST_CASE("schema: duplicate shader across files is rejected") {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    const std::filesystem::path dir = base / "vmt_schema_dup_test";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    const char* const body = R"({"format":"vmt-schema/1","shader":"DupShader","groups":[]})";
    {
        std::ofstream a(dir / "a.json", std::ios::binary);
        UT_CHECK(a.good());
        if (a.good()) a << body;
    }
    {
        std::ofstream b(dir / "b.json", std::ios::binary);
        UT_CHECK(b.good());
        if (b.good()) b << body;
    }

    auto r = vmt::SchemaLibrary::load_dir(dir);
    UT_CHECK(!r.ok());
    if (!r.ok()) {
        UT_CHECK(r.error().code == vmt::ErrorCode::SchemaValidationError);
        UT_CHECK(r.error().message.find("DupShader") != std::string::npos);
    }

    // Always clean up, even if the checks above failed.
    std::filesystem::remove_all(dir, ec);
    UT_CHECK(!ec);
}

UT_TEST_CASE("schema: fuzz: truncated and random input never crashes") {
    const std::string valid =
        R"({"format":"vmt-schema/1","shader":"FuzzShader","groups":[)"
        R"({"name":"G1","parameters":[)"
        R"({"name":"$basetexture","type":"texture","default":"a/b","description":"d"},)"
        R"({"name":"$flag","type":"bool"},)"
        R"({"name":"$m","type":"enum","values":["a","b"]},)"
        R"({"name":"$r","type":"float","min":0,"max":1}]}],)"
        R"("needs_review":[{"name":"$u","reason":"r"}]})";

    {
        auto full = vmt::SchemaLibrary::load_json(valid, "fuzz.json");
        UT_CHECK(full.ok());
        if (full.ok()) {
            UT_CHECK(!full.value().shader_names().empty());
        }
    }

    for (std::size_t len = 0; len <= valid.size(); len += 3) {
        auto tr = vmt::SchemaLibrary::load_json(valid.substr(0, len), "fuzz.json");
        if (tr.ok()) {
            UT_CHECK(!tr.value().shader_names().empty());
        } else {
            const vmt::ErrorCode code = tr.error().code;
            UT_CHECK(code == vmt::ErrorCode::SchemaLoadError ||
                     code == vmt::ErrorCode::SchemaValidationError);
        }
    }

    std::uint32_t x = 0x9E3779B9u;
    auto rnd = [&x]() -> std::uint32_t {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };
    const std::string alphabet = "\"{}[]:,\\$abcXYZ019.- \t\r\n";
    for (int iter = 0; iter < 2000; ++iter) {
        const std::size_t len = rnd() % 128u;
        std::string s;
        s.reserve(len);
        for (std::size_t i = 0; i < len; ++i) {
            s.push_back(alphabet[rnd() % alphabet.size()]);
        }
        auto fr = vmt::SchemaLibrary::load_json(s, "fuzz.json");
        if (fr.ok()) {
            UT_CHECK(!fr.value().shader_names().empty());
            (void)fr.value().schema_for("X").find_param("$basetexture");
        } else {
            const vmt::ErrorCode code = fr.error().code;
            UT_CHECK(code == vmt::ErrorCode::SchemaLoadError ||
                     code == vmt::ErrorCode::SchemaValidationError);
        }
    }
}

UT_TEST_CASE("schema: advanced is an optional boolean presentation hint") {
    const char* kSource = R"({
        "shader": "Test",
        "groups": [
            {
                "name": "G",
                "parameters": [
                    { "name": "$plain", "type": "bool" },
                    { "name": "$hidden", "type": "float", "advanced": true },
                    { "name": "$explicit_false", "type": "float", "advanced": false }
                ]
            }
        ]
    })";

    auto r = vmt::SchemaLibrary::load_json(kSource, "inline.json");
    UT_CHECK(r.ok());
    if (!r.ok()) {
        std::printf("  %s (line %u)\n", r.error().message.c_str(), r.error().line);
        return;
    }
    const vmt::ShaderSchema schema = r.value().schema_for("Test");
    const vmt::SchemaParam* plain = schema.find_param("$plain");
    const vmt::SchemaParam* hidden = schema.find_param("$hidden");
    const vmt::SchemaParam* explicit_false = schema.find_param("$explicit_false");
    UT_CHECK(plain != nullptr);
    UT_CHECK(hidden != nullptr);
    UT_CHECK(explicit_false != nullptr);
    if (plain != nullptr) {
        UT_CHECK(!plain->advanced);
    }
    if (hidden != nullptr) {
        UT_CHECK(hidden->advanced);
    }
    if (explicit_false != nullptr) {
        UT_CHECK(!explicit_false->advanced);
    }
}

UT_TEST_CASE("schema: a non-boolean advanced field is rejected") {
    expect_validation_error(R"({
        "shader": "Test",
        "groups": [
            { "name": "G", "parameters": [
                { "name": "$a", "type": "bool", "advanced": "yes" }
            ] }
        ]
    })");
}

UT_TEST_CASE("schema: shipped advanced marks are booleans on known parameters") {
    auto r = vmt::SchemaLibrary::load_dir(VMT_SCHEMA_DIR);
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    const vmt::SchemaLibrary& lib = r.value();

    const vmt::ShaderSchema vlg = lib.schema_for("VertexLitGeneric");
    const vmt::SchemaParam* fresnel = vlg.find_param("$phongfresnelranges");
    UT_CHECK(fresnel != nullptr);
    if (fresnel != nullptr) {
        UT_CHECK(fresnel->advanced);
    }
    const vmt::SchemaParam* exponent = vlg.find_param("$phongexponent");
    UT_CHECK(exponent != nullptr);
    if (exponent != nullptr) {
        UT_CHECK(!exponent->advanced);
    }

    const vmt::ShaderSchema lmg = lib.schema_for("LightmappedGeneric");
    const vmt::SchemaParam* seamless = lmg.find_param("$seamless_scale");
    UT_CHECK(seamless != nullptr);
    if (seamless != nullptr) {
        UT_CHECK(seamless->advanced);
    }
}
