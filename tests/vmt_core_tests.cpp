#include "ut.h"

#include "vmt/document.h"
#include "vmt/value.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::filesystem::path> data_files() {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const std::filesystem::path dir = VMT_TEST_DATA_DIR;
    std::filesystem::directory_iterator it(dir, ec);
    for (const auto& entry : it) {
        if (entry.is_regular_file(ec) && entry.path().extension().string() == ".vmt") {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(s.substr(start));
    return out;
}

int changed_line_count(const std::string& a, const std::string& b) {
    const auto la = split_lines(a);
    const auto lb = split_lines(b);
    if (la.size() != lb.size()) return -1;
    int changed = 0;
    for (std::size_t i = 0; i < la.size(); ++i) {
        if (la[i] != lb[i]) ++changed;
    }
    return changed;
}

} // namespace

UT_TEST_CASE("roundtrip: every fixture serializes byte-identically") {
    const auto files = data_files();
    UT_CHECK(files.size() >= 12);
    for (const auto& p : files) {
        const std::string bytes = read_file(p);
        auto r = vmt::Document::parse(bytes, p.string());
        UT_CHECK(r.ok());
        if (r.ok()) {
            UT_CHECK(r.value().serialize() == bytes);
        }
    }
}

UT_TEST_CASE("edit: replacing one parameter changes exactly one line") {
    for (const auto& p : data_files()) {
        const std::string bytes = read_file(p);
        auto r = vmt::Document::parse(bytes, p.string());
        if (!r.ok()) continue;
        vmt::Document& doc = r.value();
        const vmt::Node* first = nullptr;
        for (const auto& n : doc.nodes()) {
            if (n.is_parameter()) { first = &n; break; }
        }
        if (first == nullptr) continue;
        const std::string key(first->key());
        UT_CHECK(doc.set_parameter(key, "edited_by_test/value").ok());
        const int changed = changed_line_count(bytes, doc.serialize());
        UT_CHECK_EQ(changed, 1);
    }
}

UT_TEST_CASE("edit: adding a parameter adds exactly one line in order") {
    for (const auto& p : data_files()) {
        const std::string bytes = read_file(p);
        auto r = vmt::Document::parse(bytes, p.string());
        if (!r.ok()) continue;
        vmt::Document& doc = r.value();
        if (!doc.diagnostics().empty()) continue; // skip malformed
        const auto before = split_lines(bytes);
        UT_CHECK(doc.set_parameter("$vmt_test_added", "hello/world").ok());
        const auto after = split_lines(doc.serialize());
        UT_CHECK_EQ(after.size(), before.size() + 1);
        std::size_t j = 0;
        for (std::size_t i = 0; i < after.size() && j < before.size(); ++i) {
            if (after[i] == before[j]) ++j;
        }
        UT_CHECK(j == before.size());
        bool found = false;
        for (const auto& line : after) {
            if (line.find("$vmt_test_added") != std::string::npos) found = true;
        }
        UT_CHECK(found);
    }
}

UT_TEST_CASE("remove: removing a parameter removes exactly its line") {
    for (const auto& p : data_files()) {
        const std::string bytes = read_file(p);
        auto r = vmt::Document::parse(bytes, p.string());
        if (!r.ok()) continue;
        vmt::Document& doc = r.value();
        if (!doc.diagnostics().empty()) continue;
        const vmt::Node* first = nullptr;
        for (const auto& n : doc.nodes()) {
            if (n.is_parameter()) { first = &n; break; }
        }
        if (first == nullptr) continue;
        const std::string key(first->key());
        const auto before = split_lines(bytes);
        UT_CHECK(doc.remove_parameter(key).ok());
        const auto after = split_lines(doc.serialize());
        UT_CHECK_EQ(after.size(), before.size() - 1);
    }
}

UT_TEST_CASE("lookup: case-insensitive with original case preserved") {
    const auto path = std::filesystem::path(VMT_TEST_DATA_DIR) / "mixed_case.vmt";
    auto r = vmt::Document::parse(read_file(path), path.string());
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();

    const vmt::Node* n = doc.find("$basetexture");
    UT_CHECK(n != nullptr);
    if (n != nullptr) {
        UT_CHECK_EQ(std::string(n->key()), std::string("$BaseTexture"));
        UT_CHECK_EQ(std::string(n->value()), std::string("wood/wood1"));
    }
    UT_CHECK(doc.has("$phong"));
    UT_CHECK(doc.has("$PHONG"));
    UT_CHECK(doc.find("$doesnotexist") == nullptr);
}

UT_TEST_CASE("typed: parse and format primitives") {
    auto i = vmt::parse_int("42");
    UT_CHECK(i.ok() && i.value() == 42);
    auto ineg = vmt::parse_int("-7");
    UT_CHECK(ineg.ok() && ineg.value() == -7);
    UT_CHECK(!vmt::parse_int("4x").ok());

    auto f = vmt::parse_float(".5");
    UT_CHECK(f.ok() && f.value() == 0.5f);
    auto f2 = vmt::parse_float("1.0");
    UT_CHECK(f2.ok() && f2.value() == 1.0f);

    auto b1 = vmt::parse_bool("1");
    UT_CHECK(b1.ok() && b1.value() == true);
    auto b0 = vmt::parse_bool("0");
    UT_CHECK(b0.ok() && b0.value() == false);
    auto bt = vmt::parse_bool("true");
    UT_CHECK(bt.ok() && bt.value() == true);

    UT_CHECK(vmt::format_int(42) == "42");
    UT_CHECK(vmt::format_bool(true) == "1");
    UT_CHECK(vmt::format_float(1.0f) == "1");
}

UT_TEST_CASE("typed: vector, color and matrix round-trip") {
    auto v = vmt::parse_vec3("[1 0 0]");
    UT_CHECK(v.ok());
    if (v.ok()) {
        const vmt::Vec3 expected_v{ 1.0f, 0.0f, 0.0f };
        UT_CHECK(v.value() == expected_v);
    }
    {
        const vmt::Vec3 one{ 1.0f, 0.0f, 0.0f };
        UT_CHECK(vmt::format_vec3(one) == "[1 0 0]");
    }

    auto c = vmt::parse_color("{255 128 64}");
    UT_CHECK(c.ok());
    if (c.ok()) {
        const vmt::Color expected_c{ 255, 128, 64, 255 };
        UT_CHECK(c.value() == expected_c);
    }
    {
        const vmt::Color white{ 255, 255, 255, 255 };
        UT_CHECK(vmt::format_color(white) == "{255 255 255}");
    }

    auto m = vmt::parse_matrix("center .5 .5 scale 1 1 rotate 0 translate 0 0");
    UT_CHECK(m.ok());
    if (m.ok()) {
        UT_CHECK(m.value().center[0] == 0.5f && m.value().center[1] == 0.5f);
        UT_CHECK(m.value().scale[0] == 1.0f && m.value().scale[1] == 1.0f);
        UT_CHECK(m.value().rotate == 0.0f);
    }
    const vmt::Matrix identity{};
    UT_CHECK(vmt::format_matrix(identity) == "center 0 0 scale 1 1 rotate 0 translate 0 0");
}

UT_TEST_CASE("typed: setting an unchanged value keeps the file byte-identical") {
    const auto path = std::filesystem::path(VMT_TEST_DATA_DIR) / "lightmappedgeneric_bumpmap.vmt";
    const std::string bytes = read_file(path);
    auto r = vmt::Document::parse(bytes, path.string());
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();
    UT_CHECK(doc.set_float("$bumpmapscale", 1.0f).ok());
    UT_CHECK(doc.set_int("$detailscale", 2).ok());
    UT_CHECK(doc.serialize() == bytes);
    UT_CHECK(!doc.edited());
}

UT_TEST_CASE("typed: setting a changed value updates the parameter") {
    const auto path = std::filesystem::path(VMT_TEST_DATA_DIR) / "lightmappedgeneric_bumpmap.vmt";
    const std::string bytes = read_file(path);
    auto r = vmt::Document::parse(bytes, path.string());
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();
    UT_CHECK(doc.set_float("$bumpmapscale", 2.0f).ok());
    const vmt::Node* n = doc.find("$bumpmapscale");
    UT_CHECK(n != nullptr);
    if (n != nullptr) {
        auto v = vmt::parse_float(n->value());
        UT_CHECK(v.ok() && v.value() == 2.0f);
    }
    UT_CHECK(doc.serialize() != bytes);
}

UT_TEST_CASE("blocks: conditionals are classified and preserved") {
    const auto path = std::filesystem::path(VMT_TEST_DATA_DIR) / "conditionals.vmt";
    const std::string bytes = read_file(path);
    auto r = vmt::Document::parse(bytes, path.string());
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();

    std::vector<std::string> conds;
    for (const auto& n : doc.nodes()) {
        if (n.kind() == vmt::NodeKind::Block && n.block_kind() == vmt::BlockKind::Conditional) {
            conds.push_back(std::string(n.key()));
        }
    }
    UT_CHECK(std::find(conds.begin(), conds.end(), ">=DX90") != conds.end());
    UT_CHECK(std::find(conds.begin(), conds.end(), "<DX90") != conds.end());
    UT_CHECK(std::find(conds.begin(), conds.end(), "$WIN32") != conds.end());
    UT_CHECK(doc.serialize() == bytes);
}

UT_TEST_CASE("blocks: proxies and nested blocks are accessible") {
    const auto path = std::filesystem::path(VMT_TEST_DATA_DIR) / "unlitgeneric_proxies.vmt";
    auto r = vmt::Document::parse(read_file(path), path.string());
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();

    const vmt::Node* proxies = doc.find_block("proxies");
    UT_CHECK(proxies != nullptr);
    if (proxies != nullptr) {
        UT_CHECK_EQ(int(proxies->block_kind()), int(vmt::BlockKind::Proxies));
        const vmt::Node* scroll = nullptr;
        for (const auto& c : proxies->children()) {
            if (c.is_block() && c.key() == "TextureScroll") scroll = &c;
        }
        UT_CHECK(scroll != nullptr);
        if (scroll != nullptr) {
            UT_CHECK_EQ(int(scroll->block_kind()), int(vmt::BlockKind::Proxy));
            bool has_rate = false;
            for (const auto& c : scroll->children()) {
                if (c.is_parameter() && c.key() == "texturescrollrate" && c.value() == "0.1") has_rate = true;
            }
            UT_CHECK(has_rate);
        }
    }
}

UT_TEST_CASE("comments: enumerated and preserved") {
    const auto path = std::filesystem::path(VMT_TEST_DATA_DIR) / "comments_everywhere.vmt";
    const std::string bytes = read_file(path);
    auto r = vmt::Document::parse(bytes, path.string());
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();
    int comments = 0;
    int blanks = 0;
    for (const auto& n : doc.nodes()) {
        if (n.kind() == vmt::NodeKind::Comment) ++comments;
        if (n.kind() == vmt::NodeKind::BlankLine) ++blanks;
    }
    UT_CHECK(comments >= 3);
    UT_CHECK(blanks >= 1);
    UT_CHECK(doc.serialize() == bytes);
}

UT_TEST_CASE("encoding: CRLF and UTF-8 BOM are detected and preserved") {
    const auto path = std::filesystem::path(VMT_TEST_DATA_DIR) / "crlf_bom.vmt";
    const std::string bytes = read_file(path);
    UT_CHECK(bytes.size() >= 3);
    UT_CHECK(static_cast<unsigned char>(bytes[0]) == 0xEF);
    UT_CHECK(static_cast<unsigned char>(bytes[1]) == 0xBB);
    UT_CHECK(static_cast<unsigned char>(bytes[2]) == 0xBF);
    UT_CHECK(bytes.find("\r\n") != std::string::npos);

    auto r = vmt::Document::parse(bytes, path.string());
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();
    UT_CHECK_EQ(std::string(doc.shader()), std::string("VertexLitGeneric"));
    UT_CHECK(doc.serialize() == bytes);

    UT_CHECK(doc.set_parameter("$vmt_test_added", "x/y").ok());
    const std::string out = doc.serialize();
    UT_CHECK(out.compare(0, 3, "\xEF\xBB\xBF") == 0);
    UT_CHECK(out.find("$vmt_test_added") != std::string::npos);
    UT_CHECK(out.find("\"x/y\"\r\n") != std::string::npos);
}

UT_TEST_CASE("encoding: tabs are treated as whitespace and preserved") {
    const std::string src = "\"VertexLitGeneric\"\n{\n\t\"$basetexture\" \"a/b\"\n}\n";
    auto r = vmt::Document::parse(src);
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();
    UT_CHECK(doc.serialize() == src);
    UT_CHECK(doc.find("$basetexture") != nullptr);
}

UT_TEST_CASE("errors: malformed input yields diagnostics with position") {
    const auto path = std::filesystem::path(VMT_TEST_DATA_DIR) / "malformed.vmt";
    const std::string bytes = read_file(path);
    auto r = vmt::Document::parse(bytes, path.string());
    UT_CHECK(r.ok()); // recoverable, not fatal
    if (!r.ok()) return;
    vmt::Document& doc = r.value();
    UT_CHECK(!doc.diagnostics().empty());
    bool has_position = false;
    for (const auto& d : doc.diagnostics()) {
        if (d.location.line > 0 && d.location.column > 0) has_position = true;
    }
    UT_CHECK(has_position);
    UT_CHECK(doc.serialize() == bytes);
}

UT_TEST_CASE("fuzz: truncated and random input never crashes or hangs") {
    for (const auto& p : data_files()) {
        const std::string bytes = read_file(p);
        for (std::size_t len = 0; len <= bytes.size(); len += 3) {
            const std::string t = bytes.substr(0, len);
            auto r = vmt::Document::parse(t);
            UT_CHECK(r.ok());
            if (r.ok()) {
                (void)r.value().serialize();
                (void)r.value().find("$basetexture");
                for (const auto& n : r.value().nodes()) (void)n.key();
            }
        }
    }

    std::uint32_t x = 0x9E3779B9u;
    auto rnd = [&x]() -> std::uint32_t {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };
    const std::string alphabet = "abcABC${}\"/=<> \t\r\n\\01.";
    for (int iter = 0; iter < 3000; ++iter) {
        const std::size_t len = rnd() % 256;
        std::string s;
        s.reserve(len);
        for (std::size_t i = 0; i < len; ++i) {
            s.push_back(alphabet[rnd() % alphabet.size()]);
        }
        auto r = vmt::Document::parse(s);
        UT_CHECK(r.ok());
        if (r.ok()) {
            (void)r.value().serialize();
            for (const auto& n : r.value().nodes()) (void)n.key();
        }
    }
}

UT_TEST_CASE("set_shader: replaces the name token and keeps the body byte-identical") {
    const std::string src =
        "VertexLitGeneric\r\n"
        "{\r\n"
        "\t\"$basetexture\" \"backrooms/structures/b_wallpaper1\"\r\n"
        "\t\"$vertexcolor\" 1\r\n"
        "}\r\n";
    auto r = vmt::Document::parse(src);
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();

    const std::string before = doc.serialize();
    UT_CHECK(doc.set_shader("UnlitGeneric").ok());
    UT_CHECK_EQ(std::string(doc.shader()), std::string("UnlitGeneric"));

    // Only the header name changed: everything from the brace onwards is the
    // same bytes, including CRLF line endings.
    const std::string after = doc.serialize();
    UT_CHECK(after != before);
    UT_CHECK_EQ(after.substr(after.find('{')), before.substr(before.find('{')));
    UT_CHECK(after.find("VertexLitGeneric") == std::string::npos);

    // The body survived intact and is still addressable by name.
    const vmt::Node* base = doc.find("$basetexture");
    UT_CHECK(base != nullptr);
    if (base != nullptr) {
        UT_CHECK_EQ(std::string(base->value()), std::string("backrooms/structures/b_wallpaper1"));
    }
}

UT_TEST_CASE("set_shader: preserves the quoting style of the original name") {
    {
        const std::string src = "\"VertexLitGeneric\"\n{\n\t\"$basetexture\" \"a/b\"\n}\n";
        auto r = vmt::Document::parse(src);
        UT_CHECK(r.ok());
        if (!r.ok()) return;
        vmt::Document& doc = r.value();
        UT_CHECK(doc.set_shader("LightmappedGeneric").ok());
        UT_CHECK_EQ(doc.serialize(),
                    std::string("\"LightmappedGeneric\"\n{\n\t\"$basetexture\" \"a/b\"\n}\n"));
    }
    {
        // Bare in, bare out.
        const std::string src = "UnlitGeneric\n{\n}\n";
        auto r = vmt::Document::parse(src);
        UT_CHECK(r.ok());
        if (!r.ok()) return;
        vmt::Document& doc = r.value();
        UT_CHECK(doc.set_shader("VertexColorMaterial").ok());
        UT_CHECK_EQ(doc.serialize(), std::string("VertexColorMaterial\n{\n}\n"));
    }
}

UT_TEST_CASE("set_shader: undoing restores the previous shader") {
    const std::string src = "\"VertexLitGeneric\"\n{\n\t\"$basetexture\" \"a/b\"\n}\n";
    auto r = vmt::Document::parse(src);
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();
    UT_CHECK(doc.set_shader("UnlitGeneric").ok());
    UT_CHECK(doc.revert_edits().ok());
    UT_CHECK_EQ(doc.serialize(), src);
}

UT_TEST_CASE("set_shader: rejects names that are not a single token") {
    auto r = vmt::Document::parse("VertexLitGeneric\n{\n}\n");
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();

    for (const char* bad : {"", "Vertex Lit Generic", "VertexLit{", "VertexLit}",
                            "Vertex\"Lit", "Vertex/Lit", "Vertex\tLit"}) {
        const vmt::Result<void> result = doc.set_shader(bad);
        UT_CHECK(!result.ok());
        if (!result.ok()) {
            UT_CHECK(result.error().code != vmt::ErrorCode::Ok);
        }
    }
    // A rejected name must leave the document exactly as it was.
    UT_CHECK_EQ(doc.serialize(), std::string("VertexLitGeneric\n{\n}\n"));
    UT_CHECK_EQ(std::string(doc.shader()), std::string("VertexLitGeneric"));
}

UT_TEST_CASE("set_shader: refuses a document that has no shader name") {
    auto r = vmt::Document::parse("{\n\t\"$basetexture\" \"a/b\"\n}\n");
    UT_CHECK(r.ok());
    if (!r.ok()) return;
    vmt::Document& doc = r.value();
    UT_CHECK(doc.shader().empty());
    const vmt::Result<void> result = doc.set_shader("UnlitGeneric");
    UT_CHECK(!result.ok());
    if (!result.ok()) {
        UT_CHECK(result.error().code == vmt::ErrorCode::ParseError);
    }
}