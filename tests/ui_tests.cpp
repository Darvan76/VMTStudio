#include "ut.h"

#include "ui/asset_cache.h"
#include "ui/asset_index.h"
#include "ui/asset_pack.h"
#include "ui/connection_model.h"
#include "ui/material_browser.h"
#include "ui/material_info.h"
#include "ui/panel_model.h"
#include "ui/preview_options.h"
#include "ui/preview_sync.h"
#include "ui/session.h"
#include "ui/settings.h"
#include "ui/shader_catalog.h"
#include "ui/text_encoding.h"
#include "ui/texture_thumbnail.h"
#include "ui/validate.h"
#include "ui/version.h"
#include "vmt/schema.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path data_path(const char* name) {
    return fs::path(VMT_TEST_DATA_DIR) / name;
}

std::string read_all(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return content;
}

fs::path fresh_temp_dir() {
    static unsigned counter = 0;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) /
        ("vmtviewer_ui_tests_" + std::to_string(GetCurrentProcessId()) + "_" +
         std::to_string(++counter));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return result;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    return lines;
}

std::size_t count_changed_lines(const std::string& before, const std::string& after) {
    const std::vector<std::string> a = split_lines(before);
    const std::vector<std::string> b = split_lines(after);
    const std::size_t count = std::max(a.size(), b.size());
    std::size_t changed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& left = i < a.size() ? a[i] : std::string();
        const std::string& right = i < b.size() ? b[i] : std::string();
        if (left != right) {
            ++changed;
        }
    }
    return changed;
}

const ui::PanelGroup* find_group(const std::vector<ui::PanelGroup>& groups, const std::string& name) {
    for (const ui::PanelGroup& group : groups) {
        if (group.name == name) {
            return &group;
        }
    }
    return nullptr;
}

const ui::PanelRow* find_row(const ui::PanelGroup* group, const std::string& name) {
    if (group == nullptr) {
        return nullptr;
    }
    for (const ui::PanelRow& row : group->rows) {
        if (row.name == name) {
            return &row;
        }
    }
    return nullptr;
}

vmt::SchemaLibrary load_schemas() {
    vmt::Result<vmt::SchemaLibrary> library =
        vmt::SchemaLibrary::load_dir(std::filesystem::u8path(VMT_SCHEMA_DIR));
    UT_CHECK(library.ok());
    if (library.ok()) {
        return std::move(library.value());
    }
    return vmt::SchemaLibrary();
}

std::string phong_fixture_with_extra_params() {
    std::string text = read_all(data_path("vertexlitgeneric_phong.vmt"));
    const std::string anchor = "\"$phong\" \"1\"\n";
    const std::size_t position = text.find(anchor);
    if (position == std::string::npos) {
        return text;
    }
    const std::string extra = "    \"$custom_extra\" \"1\"\n    \"$seamless_scale\" \"2\"\n";
    text.insert(position + anchor.size(), extra);
    return text;
}

} // namespace

UT_TEST_CASE("ui session: opening a fixture preserves its bytes and reports clean") {
    ui::EditorSession session;
    const fs::path path = data_path("vertexlitgeneric_phong.vmt");
    const vmt::Result<void> result = session.open_file(path);
    UT_CHECK(result.ok());
    UT_CHECK(session.has_document());
    UT_CHECK(session.text_is_valid());
    UT_CHECK(!session.dirty());
    UT_CHECK_EQ(session.text(), read_all(path));
    UT_CHECK(session.document() != nullptr);
    if (session.document() != nullptr) {
        UT_CHECK(session.document()->shader() == "VertexLitGeneric");
    }
}

UT_TEST_CASE("ui session: a fresh session is clean and restoring the file is clean too") {
    ui::EditorSession fresh;
    UT_CHECK(!fresh.has_document());
    UT_CHECK(!fresh.dirty());
    UT_CHECK(fresh.text().empty());

    ui::EditorSession session;
    const fs::path path = data_path("vertexlitgeneric_phong.vmt");
    UT_CHECK(session.open_file(path).ok());

    // Editing the text and then restoring the exact file content is clean.
    std::string edited = session.text();
    edited += "\r\n";
    UT_CHECK(session.set_text(edited, session.display_name()).ok());
    UT_CHECK(session.dirty());
    UT_CHECK(session.set_text(read_all(path), session.display_name()).ok());
    UT_CHECK(!session.dirty());
}

UT_TEST_CASE("ui session: a panel edit changes exactly one line") {
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());

    const std::string before = session.text();
    const vmt::Result<void> edit = session.set_parameter("$phongexponent", "60");
    UT_CHECK(edit.ok());
    UT_CHECK(session.dirty());

    const std::string after = session.text();
    UT_CHECK_EQ(count_changed_lines(before, after), std::size_t(1));
    UT_CHECK_EQ(split_lines(before).size(), split_lines(after).size());

    // The one changed line carries the parameter and the new value.
    const std::vector<std::string> lines_a = split_lines(before);
    const std::vector<std::string> lines_b = split_lines(after);
    for (std::size_t i = 0; i < lines_a.size() && i < lines_b.size(); ++i) {
        if (lines_a[i] != lines_b[i]) {
            UT_CHECK(lines_b[i].find("$phongexponent") != std::string::npos);
            UT_CHECK(lines_b[i].find("60") != std::string::npos);
        }
    }
}

UT_TEST_CASE("ui session: setting the same value keeps the text byte-identical") {
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());
    const std::string before = session.text();

    const vmt::Result<void> edit = session.set_parameter("$phongexponent", "25");
    UT_CHECK(edit.ok());
    UT_CHECK_EQ(session.text(), before);
    UT_CHECK(!session.dirty());
}

UT_TEST_CASE("ui session: a text edit reparses and updates the document") {
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());

    std::string text = session.text();
    const std::size_t position = text.find("\"$phongexponent\" \"25\"");
    UT_CHECK(position != std::string::npos);
    if (position != std::string::npos) {
        text.replace(position, std::string("\"$phongexponent\" \"25\"").size(),
                     "\"$phongexponent\" \"50\"");
    }

    const vmt::Result<void> result = session.set_text(text, session.display_name());
    UT_CHECK(result.ok());
    UT_CHECK(session.dirty());
    UT_CHECK(session.document() != nullptr);
    if (session.document() != nullptr) {
        const vmt::Node* node = session.document()->find("$phongexponent");
        UT_CHECK(node != nullptr);
        if (node != nullptr) {
            UT_CHECK(node->value() == "50");
        }
    }
}

UT_TEST_CASE("ui session: malformed text stays lossless and reports diagnostics") {
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("malformed.vmt")).ok());
    UT_CHECK(session.text_is_valid());
    UT_CHECK(session.document() != nullptr);
    if (session.document() != nullptr) {
        UT_CHECK(!session.document()->diagnostics().empty());
    }
    UT_CHECK_EQ(session.text(), read_all(data_path("malformed.vmt")));

    // Saving malformed-but-recoverable input must stay byte-lossless.
    const fs::path dir = fresh_temp_dir();
    const fs::path out = dir / "malformed_out.vmt";
    UT_CHECK(session.save_as(out).ok());
    UT_CHECK_EQ(read_all(out), session.text());
}

UT_TEST_CASE("ui session: save writes exactly the session text") {
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());
    UT_CHECK(session.set_parameter("$phongboost", "5").ok());

    const fs::path dir = fresh_temp_dir();
    const fs::path out = dir / "saved.vmt";
    UT_CHECK(session.save_as(out).ok());
    UT_CHECK(!session.dirty());
    UT_CHECK_EQ(read_all(out), session.text());
    UT_CHECK_EQ(session.path(), out);

    UT_CHECK(session.set_parameter("$phongboost", "6").ok());
    UT_CHECK(session.dirty());
    UT_CHECK(session.save().ok());
    UT_CHECK(!session.dirty());
    UT_CHECK_EQ(read_all(out), session.text());
}

UT_TEST_CASE("ui session: undo and redo cover typing bursts and panel edits") {
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());
    UT_CHECK(!session.can_undo());
    UT_CHECK(!session.can_redo());
    const std::string original = session.text();

    const std::string marker = "\"$phongexponent\" \"25\"";
    UT_CHECK(original.find(marker) != std::string::npos);
    std::string edit1 = original;
    edit1.replace(edit1.find(marker), marker.size(), "\"$phongexponent\" \"40\"");
    const std::string marker2 = "\"$phongexponent\" \"40\"";
    std::string edit2 = edit1;
    edit2.replace(edit2.find(marker2), marker2.size(), "\"$phongexponent\" \"50\"");

    // Two rapid text edits (a typing burst) coalesce into one undo step.
    UT_CHECK(session.set_text(edit1, session.display_name()).ok());
    UT_CHECK(session.set_text(edit2, session.display_name()).ok());
    UT_CHECK_EQ(session.text(), edit2);
    UT_CHECK(session.dirty());
    UT_CHECK(session.can_undo());
    UT_CHECK(session.undo().ok());
    UT_CHECK_EQ(session.text(), original);
    UT_CHECK(!session.dirty());
    UT_CHECK(!session.can_undo());
    UT_CHECK(session.can_redo());
    UT_CHECK(session.redo().ok());
    UT_CHECK_EQ(session.text(), edit2);

    // A panel edit is its own step even right after the burst.
    UT_CHECK(session.set_parameter("$phongboost", "5").ok());
    const std::string with_boost = session.text();
    UT_CHECK(session.undo().ok());
    UT_CHECK_EQ(session.text(), edit2);
    UT_CHECK(session.undo().ok());
    UT_CHECK_EQ(session.text(), original);
    UT_CHECK(!session.can_undo());
    UT_CHECK(session.redo().ok());
    UT_CHECK_EQ(session.text(), edit2);
    UT_CHECK(session.redo().ok());
    UT_CHECK_EQ(session.text(), with_boost);
    UT_CHECK(!session.can_redo());

    // A degenerate edit (empty text) is still a full history step and comes
    // back exactly; the parser reports it through diagnostics, not failure.
    const std::string good = session.text();
    UT_CHECK(session.set_text("", session.display_name()).ok());
    UT_CHECK(session.text().empty());
    UT_CHECK(session.undo().ok());
    UT_CHECK_EQ(session.text(), good);
    UT_CHECK(session.text_is_valid());
    UT_CHECK(session.can_redo());
    UT_CHECK(session.redo().ok());
    UT_CHECK(session.text().empty());
}

UT_TEST_CASE("ui session: revert restores the last saved revision and is undoable") {
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());
    const fs::path dir = fresh_temp_dir();
    const fs::path out = dir / "revert.vmt";
    UT_CHECK(session.save_as(out).ok());
    const std::string saved = session.text();
    UT_CHECK(!session.can_revert());

    UT_CHECK(session.set_parameter("$phongboost", "7").ok());
    UT_CHECK(session.dirty());
    UT_CHECK(session.can_revert());
    UT_CHECK(session.revert().ok());
    UT_CHECK(!session.dirty());
    UT_CHECK_EQ(session.text(), saved);
    UT_CHECK(!session.can_revert());
    UT_CHECK(session.can_undo());
    UT_CHECK(session.undo().ok());
    UT_CHECK(session.dirty());
    UT_CHECK(session.text() != saved);
}

UT_TEST_CASE("ui session: opening a material clears the edit history") {
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());
    UT_CHECK(session.set_parameter("$phongboost", "5").ok());
    UT_CHECK(session.can_undo());
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());
    UT_CHECK(!session.can_undo());
    UT_CHECK(!session.can_redo());
    UT_CHECK(!session.dirty());
}

UT_TEST_CASE("ui session: saving is atomic and leaves no temp file") {
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());
    UT_CHECK(session.set_parameter("$phongboost", "5").ok());

    const fs::path dir = fresh_temp_dir();
    const fs::path out = dir / "atomic.vmt";
    UT_CHECK(session.save_as(out).ok());
    UT_CHECK_EQ(read_all(out), session.text());
    fs::path temp = out;
    temp += L".tmp";
    UT_CHECK(!fs::exists(temp));

    // Overwriting exercises the remove + retry rename path.
    UT_CHECK(session.set_parameter("$phongboost", "6").ok());
    UT_CHECK(session.save().ok());
    UT_CHECK_EQ(read_all(out), session.text());
    UT_CHECK(!fs::exists(temp));
}

UT_TEST_CASE("ui panel: groups, values and defaults come from schema and document") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    const vmt::ShaderSchema schema = schemas.schema_for("VertexLitGeneric");
    const std::vector<ui::PanelGroup> groups =
        ui::build_panel_model(session.document(), schema, /*show_advanced=*/false);

    const ui::PanelGroup* base = find_group(groups, "Base");
    UT_CHECK(base != nullptr);
    const ui::PanelRow* base_texture = find_row(base, "$basetexture");
    UT_CHECK(base_texture != nullptr);
    if (base_texture != nullptr) {
        UT_CHECK(base_texture->present);
        UT_CHECK(base_texture->value == "models/props/cs_office/computer");
        UT_CHECK(base_texture->type == vmt::ParamType::Texture);
    }

    const ui::PanelGroup* phong = find_group(groups, "Phong");
    UT_CHECK(phong != nullptr);
    const ui::PanelRow* exponent = find_row(phong, "$phongexponent");
    UT_CHECK(exponent != nullptr);
    if (exponent != nullptr) {
        UT_CHECK(exponent->present);
        UT_CHECK(exponent->value == "25");
        UT_CHECK(exponent->type == vmt::ParamType::Float);
        UT_CHECK(!exponent->advanced);
    }
}

UT_TEST_CASE("ui panel: unrecognized parameters get warning rows") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    const std::string text = phong_fixture_with_extra_params();
    UT_CHECK(session.set_text(text, std::string("inline.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    const vmt::ShaderSchema schema = schemas.schema_for("VertexLitGeneric");
    const std::vector<ui::PanelGroup> groups =
        ui::build_panel_model(session.document(), schema, /*show_advanced=*/true);

    const ui::PanelGroup* unrecognized = find_group(groups, ui::kUnrecognizedGroupName);
    UT_CHECK(unrecognized != nullptr);
    if (unrecognized == nullptr) {
        return;
    }
    UT_CHECK_EQ(unrecognized->rows.size(), std::size_t(2));

    const ui::PanelRow* custom = find_row(unrecognized, "$custom_extra");
    UT_CHECK(custom != nullptr);
    if (custom != nullptr) {
        UT_CHECK(custom->kind == ui::PanelRow::Kind::Unrecognized);
        UT_CHECK(custom->warning.find("not described") != std::string::npos);
    }

    const ui::PanelRow* seamless = find_row(unrecognized, "$seamless_scale");
    UT_CHECK(seamless != nullptr);
    if (seamless != nullptr) {
        UT_CHECK(seamless->warning.find("unverified") != std::string::npos);
    }
}

UT_TEST_CASE("ui panel: advanced parameters follow the toggle") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("vertexlitgeneric_phong.vmt")).ok());
    if (!session.has_document()) {
        return;
    }
    const vmt::ShaderSchema schema = schemas.schema_for("VertexLitGeneric");

    const std::vector<ui::PanelGroup> hidden =
        ui::build_panel_model(session.document(), schema, /*show_advanced=*/false);
    UT_CHECK(find_row(find_group(hidden, "Phong"), "$phongfresnelranges") == nullptr);
    // An advanced parameter present in the document is hidden, not dumped into
    // "Unrecognized".
    UT_CHECK(find_group(hidden, ui::kUnrecognizedGroupName) == nullptr);

    const std::vector<ui::PanelGroup> shown =
        ui::build_panel_model(session.document(), schema, /*show_advanced=*/true);
    const ui::PanelRow* fresnel = find_row(find_group(shown, "Phong"), "$phongfresnelranges");
    UT_CHECK(fresnel != nullptr);
    if (fresnel != nullptr) {
        UT_CHECK(fresnel->advanced);
        UT_CHECK(fresnel->present);
        UT_CHECK(fresnel->value == "[.25 .5 1]");
    }
    UT_CHECK(find_group(shown, ui::kUnrecognizedGroupName) == nullptr);
}

UT_TEST_CASE("ui settings: roundtrip, dedupe and cap") {
    const fs::path dir = fresh_temp_dir();
    const fs::path file = dir / "settings.ini";

    ui::UISettings settings;
    settings.show_advanced = true;
    settings.last_bin_dir = "D:/Games/GarrysMod/bin";
    settings.last_game_dir = "D:/Games/GarrysMod/garrysmod";
    settings.remember_file("D:/maps/a.vmt");
    settings.remember_file("D:/maps/b.vmt");
    settings.remember_file("D:/maps/a.vmt");
    UT_CHECK_EQ(settings.recent_files.size(), std::size_t(2));
    UT_CHECK_EQ(settings.recent_files[0], std::string("D:/maps/a.vmt"));
    UT_CHECK_EQ(settings.recent_files[1], std::string("D:/maps/b.vmt"));

    UT_CHECK(settings.save(file));
    const ui::UISettings loaded = ui::UISettings::load(file);
    UT_CHECK(loaded.show_advanced);
    UT_CHECK_EQ(loaded.last_bin_dir, settings.last_bin_dir);
    UT_CHECK_EQ(loaded.last_game_dir, settings.last_game_dir);
    UT_CHECK_EQ(loaded.recent_files.size(), settings.recent_files.size());
    if (loaded.recent_files.size() == settings.recent_files.size()) {
        for (std::size_t i = 0; i < loaded.recent_files.size(); ++i) {
            UT_CHECK_EQ(loaded.recent_files[i], settings.recent_files[i]);
        }
    }

    ui::UISettings capped;
    for (int i = 0; i < 14; ++i) {
        capped.remember_file("D:/maps/" + std::to_string(i) + ".vmt");
    }
    UT_CHECK_EQ(capped.recent_files.size(), ui::UISettings::kMaxRecentFiles);
    UT_CHECK_EQ(capped.recent_files.front(), std::string("D:/maps/13.vmt"));

    const ui::UISettings missing = ui::UISettings::load(dir / "does_not_exist.ini");
    UT_CHECK(missing.recent_files.empty());
    UT_CHECK(!missing.show_advanced);
}

UT_TEST_CASE("ui encoding: utf8, ansi and invalid buffers behave") {
    // "cafÃ© âœ“" as explicit UTF-8 bytes (source escapes are charset-dependent).
    std::string utf8;
    utf8 += "caf";
    utf8.push_back(static_cast<char>(0xC3));
    utf8.push_back(static_cast<char>(0xA9));
    utf8.push_back(' ');
    utf8.push_back(static_cast<char>(0xE2));
    utf8.push_back(static_cast<char>(0x9C));
    utf8.push_back(static_cast<char>(0x93));
    UT_CHECK(ui::is_valid_utf8(utf8));

    ui::TextEncoding encoding = ui::TextEncoding::Ansi;
    const std::wstring wide = ui::decode_for_display(utf8, encoding);
    UT_CHECK(encoding == ui::TextEncoding::Utf8);
    UT_CHECK_EQ(ui::encode_from_display(wide, encoding), utf8);

    // Bytes produced from the process ANSI code page roundtrip exactly whether
    // the code page is UTF-8 or a legacy one.
    const wchar_t* text = L"r\u00E9sum\u00E9 \u20AC";
    const int length = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
    UT_CHECK(length > 1);
    std::string ansi(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, text, -1, ansi.data(), length, nullptr, nullptr);
    ui::TextEncoding ansi_encoding = ui::TextEncoding::Utf8;
    const std::wstring ansi_wide = ui::decode_for_display(ansi, ansi_encoding);
    UT_CHECK_EQ(ui::encode_from_display(ansi_wide, ansi_encoding), ansi);

    std::string invalid;
    invalid.push_back(static_cast<char>(0xFF));
    invalid.push_back(static_cast<char>(0xFE));
    invalid.push_back('A');
    UT_CHECK(!ui::is_valid_utf8(invalid));
    ui::TextEncoding fallback = ui::TextEncoding::Utf8;
    const std::wstring invalid_wide = ui::decode_for_display(invalid, fallback);
    UT_CHECK(fallback == ui::TextEncoding::Ansi);
    // The ANSI fallback cannot guarantee byte equality for arbitrary garbage,
    // but it must decode without crashing and produce something.
    (void)invalid_wide;

    UT_CHECK(ui::is_valid_utf8(std::string()));
    ui::TextEncoding empty_encoding = ui::TextEncoding::Ansi;
    UT_CHECK(ui::decode_for_display(std::string(), empty_encoding).empty());
    UT_CHECK(empty_encoding == ui::TextEncoding::Utf8);
}

UT_TEST_CASE("ui connection: a missing game folder fails cleanly") {
    const fs::path dir = fresh_temp_dir();
    const ui::ConnectionCheck check = ui::test_connection(dir);
    UT_CHECK(!check.connected);
    UT_CHECK_EQ(check.lines.size(), std::size_t(5));
    UT_CHECK(check.architecture == "unknown");
    UT_CHECK(!check.summary.empty());
    for (const std::string& line : check.lines) {
        UT_CHECK(line.find("[FAIL]") != std::string::npos);
    }
}

UT_TEST_CASE("ui connection: detect_game_dir finds the folder with gameinfo.txt") {
    const fs::path dir = fresh_temp_dir();
    std::error_code ec;

    // Mod folder next to /bin: <root>/hl2/gameinfo.txt.
    const fs::path root = dir / "game";
    fs::create_directories(root / "bin", ec);
    fs::create_directories(root / "hl2", ec);
    {
        std::ofstream out(root / "hl2" / "gameinfo.txt");
        out << "// gameinfo";
    }
    UT_CHECK_EQ(ui::detect_game_dir(root / "bin"), root / "hl2");

    // gameinfo.txt directly in the parent of /bin.
    const fs::path root2 = dir / "game2";
    fs::create_directories(root2 / "bin", ec);
    {
        std::ofstream out(root2 / "gameinfo.txt");
        out << "// gameinfo";
    }
    UT_CHECK_EQ(ui::detect_game_dir(root2 / "bin"), root2);

    // Nothing to find.
    const fs::path root3 = dir / "game3";
    fs::create_directories(root3 / "bin", ec);
    UT_CHECK(ui::detect_game_dir(root3 / "bin").empty());

    // Empty input.
    UT_CHECK(ui::detect_game_dir(fs::path()).empty());
}

UT_TEST_CASE("ui connection: detect_bin_dir resolves mods, game roots and libraries") {
    const fs::path dir = fresh_temp_dir();
    std::error_code ec;

    auto make_bin = [&](const fs::path& path) {
        fs::create_directories(path, ec);
        std::ofstream(path / "tier0.dll") << "x";
        std::ofstream(path / "vstdlib.dll") << "x";
    };

    // A mod folder with gameinfo.txt resolves to its own /bin.
    const fs::path mod = dir / "mod";
    make_bin(mod / "bin");
    {
        std::ofstream out(mod / "gameinfo.txt");
        out << "// gameinfo";
    }
    UT_CHECK_EQ(ui::detect_bin_dir(mod), mod / "bin");

    // A Source module without a local /bin falls back to the enclosing game
    // root (Source SDK layout).
    const fs::path root = dir / "sdk";
    fs::create_directories(root / "hl2", ec);
    {
        std::ofstream out(root / "hl2" / "gameinfo.txt");
        out << "// gameinfo";
    }
    make_bin(root / "bin");
    UT_CHECK_EQ(ui::detect_bin_dir(root / "hl2"), root / "bin");

    // A Steam library root is resolved through its immediate children.
    const fs::path library = dir / "library";
    make_bin(library / "gmod" / "bin");
    UT_CHECK_EQ(ui::detect_bin_dir(library), library / "gmod" / "bin");

    // tier0.dll alone is not enough: a real /bin also needs vstdlib.dll.
    const fs::path incomplete = dir / "incomplete";
    fs::create_directories(incomplete / "bin", ec);
    {
        std::ofstream out(incomplete / "bin" / "tier0.dll");
        out << "x";
    }
    UT_CHECK(ui::detect_bin_dir(incomplete).empty());

    // Empty input.
    UT_CHECK(ui::detect_bin_dir(fs::path()).empty());
}

// --- Phase 7: debounced engine-preview sync ---------------------------------

UT_TEST_CASE("ui preview: rapid edits coalesce into one write and reload") {
    int writes = 0;
    int reloads = 0;
    std::string written;
    ui::PreviewSync sync;
    sync.configure(
        [&](const std::string& text) {
            ++writes;
            written = text;
            return true;
        },
        [&]() {
            ++reloads;
            return true;
        });
    sync.set_active(true);

    for (int i = 0; i < 5; ++i) {
        UT_CHECK(sync.document_changed(true));
    }
    UT_CHECK(sync.update_pending());
    UT_CHECK_EQ(writes, 0); // debounce: nothing reaches the disk yet
    UT_CHECK(sync.flush("$phongboost 2").ok());
    UT_CHECK_EQ(writes, 1);
    UT_CHECK_EQ(reloads, 1);
    UT_CHECK_EQ(written, std::string("$phongboost 2"));
    UT_CHECK(!sync.update_pending());
    UT_CHECK(!sync.stale());
    UT_CHECK(sync.last_error().code == vmt::ErrorCode::Ok);
}

UT_TEST_CASE("ui preview: invalid text keeps the last good render") {
    int writes = 0;
    int reloads = 0;
    std::string written;
    ui::PreviewSync sync;
    sync.configure(
        [&](const std::string& text) {
            ++writes;
            written = text;
            return true;
        },
        [&]() {
            ++reloads;
            return true;
        });
    sync.set_active(true);
    UT_CHECK(sync.flush_now("good", true).ok());
    UT_CHECK_EQ(written, std::string("good"));

    const vmt::Error parse_error =
        vmt::make_error(vmt::ErrorCode::ParseError, "unexpected '}'", "fix it",
                        "material.vmt", 7, 1);
    UT_CHECK(!sync.document_changed(false, parse_error));
    UT_CHECK(!sync.update_pending());
    UT_CHECK(sync.stale());
    UT_CHECK_EQ(sync.last_error().code, vmt::ErrorCode::ParseError);

    // A stray flush must not push the invalid state to the engine.
    UT_CHECK(sync.flush("broken {").ok());
    UT_CHECK_EQ(writes, 1);
    UT_CHECK_EQ(reloads, 1);
    UT_CHECK_EQ(written, std::string("good"));

    // Fixing the text clears staleness and schedules the next update.
    UT_CHECK(sync.document_changed(true));
    UT_CHECK(sync.flush("$color2 \"{1 0.5 0.25}\"").ok());
    UT_CHECK(!sync.stale());
    UT_CHECK_EQ(written, std::string("$color2 \"{1 0.5 0.25}\""));
}

UT_TEST_CASE("ui preview: a rejected reload rolls back to the last good text") {
    std::string written;
    int reloads = 0;
    bool engine_accepts = true;
    ui::PreviewSync sync;
    sync.configure(
        [&](const std::string& text) {
            written = text;
            return true;
        },
        [&]() {
            ++reloads;
            return engine_accepts;
        });
    sync.set_active(true);
    UT_CHECK(sync.flush_now("good", true).ok());
    UT_CHECK_EQ(reloads, 1);

    engine_accepts = false;
    UT_CHECK(sync.document_changed(true));
    const vmt::Result<void> flushed = sync.flush("bad shader name");
    UT_CHECK(!flushed.ok());
    UT_CHECK(sync.stale());
    UT_CHECK_EQ(sync.last_error().code, vmt::ErrorCode::RenderFailed);
    // Reload #2 is the rejected edit, #3 restores the last good text.
    UT_CHECK_EQ(reloads, 3);
    UT_CHECK_EQ(written, std::string("good"));
}

UT_TEST_CASE("ui preview: save flushes immediately and invalid flush_now is refused") {
    std::string written;
    ui::PreviewSync sync;
    sync.configure([&](const std::string& text) {
                       written = text;
                       return true;
                   },
                   []() { return true; });
    sync.set_active(true);

    // Save while a debounce is pending: the write happens now.
    UT_CHECK(sync.document_changed(true));
    UT_CHECK(sync.update_pending());
    UT_CHECK(sync.flush_now("$phongboost 3", true).ok());
    UT_CHECK(!sync.update_pending());
    UT_CHECK_EQ(written, std::string("$phongboost 3"));

    // Saving a syntactically invalid buffer must not touch the engine.
    const vmt::Result<void> refused =
        sync.flush_now("broken {", false, vmt::make_error(vmt::ErrorCode::ParseError, "eof"));
    UT_CHECK(!refused.ok());
    UT_CHECK(sync.stale());
    UT_CHECK_EQ(sync.last_error().code, vmt::ErrorCode::ParseError);
    UT_CHECK_EQ(written, std::string("$phongboost 3"));
}

UT_TEST_CASE("ui preview: the material file replaces cleanly under materials/") {
    const fs::path root = fresh_temp_dir() / "preview";
    UT_CHECK(ui::write_preview_material(root, "first").ok());
    const fs::path file = root / "materials" / "vmtviewer_preview.vmt";
    UT_CHECK(fs::exists(file));
    UT_CHECK_EQ(read_all(file), std::string("first"));

    UT_CHECK(ui::write_preview_material(root, "second").ok());
    UT_CHECK_EQ(read_all(file), std::string("second"));
    UT_CHECK(!fs::exists(root / "materials" / "vmtviewer_preview.vmt.tmp"));
}

UT_TEST_CASE("ui preview: 200 edit/reload cycles stay bounded and current") {
    const fs::path root = fresh_temp_dir() / "preview";
    int writes = 0;
    int reloads = 0;
    ui::PreviewSync sync;
    sync.configure(
        [&](const std::string& text) {
            ++writes;
            return ui::write_preview_material(root, text).ok();
        },
        [&]() {
            ++reloads;
            return true;
        });
    sync.set_active(true);

    // Warm up the writer, then measure OS handles across the 200 cycles.
    for (int i = 0; i < 10; ++i) {
        UT_CHECK(sync.flush_now("warmup", true).ok());
    }
    DWORD handles_before = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles_before);

    std::string text = "$phongboost 0";
    for (int i = 0; i < 200; ++i) {
        text = "\"VertexLitGeneric\"\n{\n\t\"$phongboost\" \"" + std::to_string(i) +
               "\"\n\t\"$color2\" \"{1 0.5 0.25}\"\n}\n";
        UT_CHECK(sync.document_changed(true));
        UT_CHECK(sync.flush(text).ok());
    }

    DWORD handles_after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles_after);
    UT_CHECK(handles_after <= handles_before + 4);
    UT_CHECK_EQ(writes, 210);
    UT_CHECK_EQ(reloads, 210);
    UT_CHECK(!sync.update_pending());
    UT_CHECK(!sync.stale());

    const fs::path file = root / "materials" / "vmtviewer_preview.vmt";
    UT_CHECK(fs::exists(file));
    UT_CHECK_EQ(read_all(file), text);
    UT_CHECK(!fs::exists(root / "materials" / "vmtviewer_preview.vmt.tmp"));
}

// --- Phase 8: validation rules ----------------------------------------------

namespace {

const ui::Finding* find_finding(const std::vector<ui::Finding>& findings,
                                const std::string& code) {
    for (const ui::Finding& finding : findings) {
        if (finding.code == code) {
            return &finding;
        }
    }
    return nullptr;
}

std::size_t count_findings(const std::vector<ui::Finding>& findings, const std::string& code) {
    std::size_t count = 0;
    for (const ui::Finding& finding : findings) {
        if (finding.code == code) {
            ++count;
        }
    }
    return count;
}

ui::TextureResolver all_found() {
    return [](const std::string&) { return ui::TextureStatus::Found; };
}

} // namespace

UT_TEST_CASE("ui validate: missing texture is an error with its line") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("validate_missing_texture.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    ui::ValidationOptions options;
    options.schemas = &schemas;
    options.resolve_texture = [](const std::string& name) {
        return name == "vmtviewer_tests/definitely_missing_texture"
                   ? ui::TextureStatus::Missing
                   : ui::TextureStatus::Found;
    };

    const std::vector<ui::Finding> findings =
        ui::validate_document(*session.document(), options);
    UT_CHECK_EQ(findings.size(), std::size_t(1));
    const ui::Finding* missing = find_finding(findings, "missing-texture");
    UT_CHECK(missing != nullptr);
    if (missing != nullptr) {
        UT_CHECK(missing->severity == ui::Severity::Error);
        UT_CHECK_EQ(missing->line, std::uint32_t(3));
        UT_CHECK(missing->message.find("materials/vmtviewer_tests/definitely_missing_texture.vtf") !=
                 std::string::npos);
    }
}

UT_TEST_CASE("ui validate: unknown shader warns and skips parameter checks") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("validate_unknown_shader.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    ui::ValidationOptions options;
    options.schemas = &schemas;
    options.resolve_texture = all_found();

    const std::vector<ui::Finding> findings =
        ui::validate_document(*session.document(), options);
    UT_CHECK_EQ(findings.size(), std::size_t(1));
    const ui::Finding* unknown = find_finding(findings, "unknown-shader");
    UT_CHECK(unknown != nullptr);
    if (unknown != nullptr) {
        UT_CHECK(unknown->severity == ui::Severity::Warning);
        UT_CHECK_EQ(unknown->line, std::uint32_t(1));
    }
}

UT_TEST_CASE("ui validate: unknown parameter is a warning at its line") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("validate_unknown_param.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    ui::ValidationOptions options;
    options.schemas = &schemas;

    const std::vector<ui::Finding> findings =
        ui::validate_document(*session.document(), options);
    UT_CHECK_EQ(findings.size(), std::size_t(1));
    const ui::Finding* unknown = find_finding(findings, "unknown-parameter");
    UT_CHECK(unknown != nullptr);
    if (unknown != nullptr) {
        UT_CHECK(unknown->severity == ui::Severity::Warning);
        UT_CHECK_EQ(unknown->line, std::uint32_t(3));
    }
}

UT_TEST_CASE("ui validate: value type mismatch against the schema") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("validate_type_mismatch.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    ui::ValidationOptions options;
    options.schemas = &schemas;

    const std::vector<ui::Finding> findings =
        ui::validate_document(*session.document(), options);
    UT_CHECK_EQ(findings.size(), std::size_t(1));
    const ui::Finding* mismatch = find_finding(findings, "type-mismatch");
    UT_CHECK(mismatch != nullptr);
    if (mismatch != nullptr) {
        UT_CHECK(mismatch->severity == ui::Severity::Warning);
        UT_CHECK_EQ(mismatch->line, std::uint32_t(4));
    }
}

UT_TEST_CASE("ui validate: range and enum mismatches from an inline schema") {
    const char* schema_json =
        "{"
        "\"format\": \"vmt-schema/1\","
        "\"shader\": \"ValidateProbe\","
        "\"groups\": [{ \"name\": \"N\", \"parameters\": ["
        "  { \"name\": \"$count\", \"type\": \"int\", \"min\": 1, \"max\": 10 },"
        "  { \"name\": \"$scale\", \"type\": \"float\", \"min\": 0, \"max\": 1 },"
        "  { \"name\": \"$mode\", \"type\": \"enum\", \"values\": [\"alpha\", \"beta\"] }"
        "]}]"
        "}";
    vmt::Result<vmt::SchemaLibrary> library = vmt::SchemaLibrary::load_json(schema_json, "inline");
    UT_CHECK(library.ok());
    if (!library.ok()) {
        return;
    }

    const std::string text =
        "\"ValidateProbe\"\n"
        "{\n"
        "    \"$count\" \"42\"\n"
        "    \"$scale\" \"-0.5\"\n"
        "    \"$mode\" \"gamma\"\n"
        "}\n";

    ui::ValidationOptions options;
    options.schemas = &library.value();
    const std::vector<ui::Finding> findings = ui::validate_text(text, options);

    UT_CHECK_EQ(findings.size(), std::size_t(3));
    UT_CHECK_EQ(count_findings(findings, "range-mismatch"), std::size_t(2));
    const ui::Finding* mode = find_finding(findings, "type-mismatch");
    UT_CHECK(mode != nullptr);
    if (mode != nullptr) {
        UT_CHECK_EQ(mode->line, std::uint32_t(5));
        UT_CHECK(mode->message.find("alpha") != std::string::npos);
        UT_CHECK(mode->message.find("beta") != std::string::npos);
    }
}

UT_TEST_CASE("ui validate: phong parameters without $phong warn") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("validate_dependency.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    ui::ValidationOptions options;
    options.schemas = &schemas;

    const std::vector<ui::Finding> findings =
        ui::validate_document(*session.document(), options);
    UT_CHECK_EQ(findings.size(), std::size_t(2));
    UT_CHECK_EQ(count_findings(findings, "missing-dependency"), std::size_t(2));
    for (const ui::Finding& finding : findings) {
        UT_CHECK(finding.severity == ui::Severity::Warning);
        UT_CHECK(finding.message.find("$phong") != std::string::npos);
    }
}

UT_TEST_CASE("ui validate: path style findings (backslash, extension, case)") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("validate_paths.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    ui::ValidationOptions options;
    options.schemas = &schemas;
    options.resolve_texture = [](const std::string& name) {
        UT_CHECK_EQ(name, std::string("models/props/computer"));
        return ui::TextureStatus::Found;
    };

    const std::vector<ui::Finding> findings =
        ui::validate_document(*session.document(), options);
    UT_CHECK_EQ(findings.size(), std::size_t(3));
    UT_CHECK_EQ(count_findings(findings, "path-backslash"), std::size_t(1));
    UT_CHECK_EQ(count_findings(findings, "path-extension"), std::size_t(1));
    UT_CHECK_EQ(count_findings(findings, "path-case"), std::size_t(1));
    for (const ui::Finding& finding : findings) {
        UT_CHECK_EQ(finding.line, std::uint32_t(3));
    }
    const ui::Finding* case_finding = find_finding(findings, "path-case");
    UT_CHECK(case_finding != nullptr);
    if (case_finding != nullptr) {
        UT_CHECK(case_finding->severity == ui::Severity::Info);
    }
}

UT_TEST_CASE("ui validate: duplicate parameter points at the second occurrence") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("validate_duplicates.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    ui::ValidationOptions options;
    options.schemas = &schemas;
    options.resolve_texture = all_found();

    const std::vector<ui::Finding> findings =
        ui::validate_document(*session.document(), options);
    UT_CHECK_EQ(findings.size(), std::size_t(1));
    const ui::Finding* duplicate = find_finding(findings, "duplicate-parameter");
    UT_CHECK(duplicate != nullptr);
    if (duplicate != nullptr) {
        UT_CHECK(duplicate->severity == ui::Severity::Warning);
        UT_CHECK_EQ(duplicate->line, std::uint32_t(4));
        UT_CHECK(duplicate->message.find("line 3") != std::string::npos);
    }
}

UT_TEST_CASE("ui validate: clean material passes; without engine textures are reported skipped") {
    const vmt::SchemaLibrary schemas = load_schemas();
    ui::EditorSession session;
    UT_CHECK(session.open_file(data_path("validate_clean.vmt")).ok());
    if (!session.has_document()) {
        return;
    }

    ui::ValidationOptions connected;
    connected.schemas = &schemas;
    connected.resolve_texture = all_found();
    const std::vector<ui::Finding> clean =
        ui::validate_document(*session.document(), connected);
    UT_CHECK(clean.empty());

    ui::ValidationOptions offline;
    offline.schemas = &schemas;
    const std::vector<ui::Finding> skipped =
        ui::validate_document(*session.document(), offline);
    UT_CHECK_EQ(skipped.size(), std::size_t(1));
    const ui::Finding* note = find_finding(skipped, "texture-check-skipped");
    UT_CHECK(note != nullptr);
    if (note != nullptr) {
        UT_CHECK(note->severity == ui::Severity::Info);
    }
}

UT_TEST_CASE("ui validate: render targets and env_cubemap are exempt from lookups") {
    const vmt::SchemaLibrary schemas = load_schemas();
    const std::string text =
        "\"VertexLitGeneric\"\n"
        "{\n"
        "    \"$basetexture\" \"_rt_Camera\"\n"
        "    \"$envmap\" \"env_cubemap\"\n"
        "}\n";
    ui::ValidationOptions options;
    options.schemas = &schemas;
    options.resolve_texture = [](const std::string&) {
        return ui::TextureStatus::Missing; // must never be called for the two values above
    };
    const std::vector<ui::Finding> findings = ui::validate_text(text, options);
    UT_CHECK(findings.empty());
}

UT_TEST_CASE("ui validate: invalid text reports its parse diagnostics as errors") {
    const vmt::SchemaLibrary schemas = load_schemas();
    const std::string text =
        "\"VertexLitGeneric\"\n"
        "{\n"
        "    \"$basetexture\" \"brick/brick05\"\n";
    ui::ValidationOptions options;
    options.schemas = &schemas;
    const std::vector<ui::Finding> findings = ui::validate_text(text, options);
    const ui::Finding* parse = find_finding(findings, "parse-error");
    UT_CHECK(parse != nullptr);
    if (parse != nullptr) {
        UT_CHECK(parse->severity == ui::Severity::Error);
        UT_CHECK(parse->line != 0);
        UT_CHECK(parse->message.find("unterminated") != std::string::npos ||
                 parse->message.find("missing") != std::string::npos);
    }
}

// --- Phase 9: material browser ------------------------------------------------

UT_TEST_CASE("ui browser: game paths normalize to material names") {
    UT_CHECK_EQ(ui::material_name_from_game_path("materials\\brick\\brickwall001a.vmt"),
                std::string("brick/brickwall001a"));
    UT_CHECK_EQ(ui::material_name_from_game_path("materials/foo/bar.VMT"),
                std::string("foo/bar"));
    UT_CHECK_EQ(ui::material_name_from_game_path("materials/materials/dev/dev_measuregeneric01.vmt"),
                std::string("dev/dev_measuregeneric01"));
    UT_CHECK_EQ(ui::material_name_from_game_path("Materials/Foo/Bar.VMT"),
                std::string("Foo/Bar"));
    UT_CHECK_EQ(ui::material_name_from_game_path("materials/"), std::string());
}

UT_TEST_CASE("ui browser: folder scan finds vmt files, hides dot directories") {
    const fs::path root = fresh_temp_dir() / "scan";
    std::error_code ec;
    fs::create_directories(root / "materials" / "brick", ec);
    fs::create_directories(root / "materials" / ".hidden", ec);
    fs::create_directories(root / "materials" / "wood", ec);
    {
        std::ofstream(root / "materials" / "brick" / "brickwall001a.vmt") << "\"UnlitGeneric\"\n{\n}\n";
        std::ofstream(root / "materials" / "wood" / "plank.vmt") << "\"UnlitGeneric\"\n{\n}\n";
        std::ofstream(root / "materials" / ".hidden" / "hidden.vmt") << "x";
        std::ofstream(root / "materials" / "readme.txt") << "x";
    }

    ui::MaterialIndex index;
    const std::size_t added = index.add_directory(root);
    UT_CHECK_EQ(added, static_cast<std::size_t>(2));
    UT_CHECK_EQ(index.size(), static_cast<std::size_t>(2));
    UT_CHECK_EQ(index.entries()[0].name, std::string("brick/brickwall001a"));
    UT_CHECK_EQ(index.entries()[0].display, std::string("brick/brickwall001a.vmt"));
    UT_CHECK_EQ(index.entries()[1].name, std::string("wood/plank"));
    UT_CHECK(!index.entries()[0].from_engine);
    UT_CHECK(!index.entries()[0].disk_path.empty());
}

UT_TEST_CASE("ui browser: engine names dedupe against the folder scan") {
    const fs::path root = fresh_temp_dir() / "dedupe";
    std::error_code ec;
    fs::create_directories(root / "materials" / "brick", ec);
    std::ofstream(root / "materials" / "brick" / "brickwall001a.vmt") << "x";

    ui::MaterialIndex index;
    index.add_directory(root);
    index.add_engine_names({ "materials/brick/brickwall001a.vmt", "materials/dev/dev_measuregeneric01.vmt" });
    UT_CHECK_EQ(index.size(), static_cast<std::size_t>(2));

    bool found_disk = false;
    bool found_engine = false;
    for (const ui::MaterialEntry& entry : index.entries()) {
        if (entry.name == "brick/brickwall001a") {
            found_disk = true;
            UT_CHECK(!entry.from_engine); // the disk entry won the dedupe
        }
        if (entry.name == "dev/dev_measuregeneric01") {
            found_engine = true;
            UT_CHECK(entry.from_engine);
        }
    }
    UT_CHECK(found_disk);
    UT_CHECK(found_engine);
}

UT_TEST_CASE("ui browser: search is case-insensitive and filters") {
    ui::MaterialIndex index;
    index.add_engine_names({ "materials/brick/brickwall001a.vmt", "materials/wood/plank.vmt",
                             "materials/dev/dev_measuregeneric01.vmt" });
    UT_CHECK_EQ(index.search("").size(), static_cast<std::size_t>(3));
    UT_CHECK_EQ(index.search("BRICK").size(), static_cast<std::size_t>(1));
    UT_CHECK_EQ(index.search("dev_").size(), static_cast<std::size_t>(1));
    UT_CHECK_EQ(index.search("wall").size(), static_cast<std::size_t>(1));
    UT_CHECK_EQ(index.search("nope").size(), static_cast<std::size_t>(0));
    const std::vector<std::size_t> wood = index.search("plank");
    UT_CHECK_EQ(wood.size(), static_cast<std::size_t>(1));
    if (!wood.empty()) {
        UT_CHECK_EQ(index.entries()[wood[0]].name, std::string("wood/plank"));
    }
}

UT_TEST_CASE("ui browser: thumbnail cache name is stable and a bmp round-trips") {
    const std::string first = ui::thumbnail_file_name("Brick/Brickwall001A");
    UT_CHECK_EQ(first, ui::thumbnail_file_name("brick/brickwall001a"));
    UT_CHECK(first.find(".bmp") != std::string::npos);
    UT_CHECK(first.find('/') == std::string::npos);
    UT_CHECK(first.find('\\') == std::string::npos);

    const fs::path root = fresh_temp_dir() / "thumbs";
    const fs::path bmp = ui::thumbnail_cache_path(root, "brick/brickwall001a");
    std::vector<unsigned char> pixels(4 * 4 * 4, 0);
    for (int i = 0; i < 16; ++i) {
        pixels[i * 4 + 0] = static_cast<unsigned char>(i * 4);     // B
        pixels[i * 4 + 1] = static_cast<unsigned char>(i * 4 + 1); // G
        pixels[i * 4 + 2] = static_cast<unsigned char>(i * 4 + 2); // R
        pixels[i * 4 + 3] = 255;
    }
    UT_CHECK(ui::write_bmp_from_bgra(bmp, 4, 4, pixels.data(), true));
    int width = 0;
    int height = 0;
    UT_CHECK(ui::read_bmp_size(bmp, width, height));
    UT_CHECK_EQ(width, 4);
    UT_CHECK_EQ(height, 4);

    // Reading a non-BMP file fails cleanly.
    const fs::path text = root / "not_a_bmp.txt";
    std::ofstream(text) << "hello";
    UT_CHECK(!ui::read_bmp_size(text, width, height));
}

// --- texture thumbnails (VTF decoding without the engine) ----------------------

namespace {

// TextureSource backed by an in-memory map, so the tests never touch the game
// files. Records every path it was asked for.
class FakeTextureSource final : public ui::TextureSource {
public:
    std::vector<std::pair<std::string, std::vector<unsigned char>>> files;
    mutable std::vector<std::string> requests;

    void add(const std::string& path, std::vector<unsigned char> bytes) {
        files.emplace_back(path, std::move(bytes));
    }

    vmt::Result<std::vector<unsigned char>> read(const std::string& game_path) override {
        requests.push_back(game_path);
        for (const auto& entry : files) {
            if (entry.first == game_path) {
                return vmt::Ok(entry.second);
            }
        }
        return vmt::Err<std::vector<unsigned char>>(
            vmt::make_error(vmt::ErrorCode::NotFound, "not found: " + game_path));
    }
};

// Builds a minimal but genuinely decodable DXT1 VTF so the tests do not depend
// on a binary fixture existing.
//
// Field offsets follow SVTFHeader_70 as VTFLib++ declares it (VTFFormat.h):
// the 16-byte base header (signature, version[2], header size) is followed by
// Width/Height as UShort, then a 32-bit Flags, Frames, StartFrame, the
// 16-byte-aligned reflectivity block, and the format/mip fields. The struct is
// padded to 16 bytes, so a VTF 7.0 header is 64 bytes and the image payload
// starts there.
//
// `width`/`height` must be multiples of 4; `blocks_wide` blocks are emitted per
// row so 16x4 textures work.
std::vector<unsigned char> make_dxt1_vtf(std::uint16_t width, std::uint16_t height,
                                         int blocks_wide = 1) {
    constexpr std::size_t kHeaderSize = 64;
    const std::size_t blocks = static_cast<std::size_t>(blocks_wide) * (height / 4);
    std::vector<unsigned char> bytes(kHeaderSize + blocks * 8, 0);

    const auto put16 = [&bytes](std::size_t at, std::uint16_t value) {
        bytes[at + 0] = static_cast<unsigned char>(value & 0xFFu);
        bytes[at + 1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
    };
    const auto put32 = [&bytes](std::size_t at, std::uint32_t value) {
        bytes[at + 0] = static_cast<unsigned char>(value & 0xFFu);
        bytes[at + 1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
        bytes[at + 2] = static_cast<unsigned char>((value >> 16) & 0xFFu);
        bytes[at + 3] = static_cast<unsigned char>((value >> 24) & 0xFFu);
    };

    bytes[0] = 'V';
    bytes[1] = 'T';
    bytes[2] = 'F';
    bytes[3] = 0;
    put32(4, 7);              // version major
    put32(8, 0);              // version minor
    put32(12, kHeaderSize);   // header size
    put16(16, width);         // width
    put16(18, height);        // height
    put32(20, 4);             // flags: TEXTUREFLAGS_NOMIPMAPS
    put16(24, 1);             // frames
    put16(26, 0);             // start frame
    put32(28, 0);             // padding (16-byte alignment)
    // Reflectivity[3] floats, Padding1 and BumpScale occupy 32..51; zero is a
    // valid neutral value.
    // VTFImageFormat is a plain C enum, so MSVC gives it 4 bytes, not 2. That
    // is what shifts every later field down: with a 16-bit format value, VTFLib++
    // reads the mip count as the format and reports garbage.
    put32(52, 13);            // ImageFormat: IMAGE_FORMAT_DXT1
    bytes[56] = 1;            // MipCount
    put32(57, 0);             // LowResImageFormat (none)
    bytes[61] = 0;            // LowResImageWidth
    bytes[62] = 0;            // LowResImageHeight

    // Each DXT1 block in RGB565: colour0 = 0xF800 (pure red), colour1 = 0x001F
    // (pure blue), and all four 2-bit indices 0, so every texel decodes to
    // opaque red.
    for (std::size_t block = 0; block < blocks; ++block) {
        const std::size_t at = kHeaderSize + block * 8;
        put16(at + 0, 0xF800);      // colour0 = red
        put16(at + 2, 0x001F);      // colour1 = blue
        put32(at + 4, 0x00000000);  // indices all 0 -> colour0
    }
    return bytes;
}

std::vector<unsigned char> make_dxt1_4x4() {
    return make_dxt1_vtf(4, 4, 1);
}

std::string vmt_with_base_texture(const std::string& texture) {
    return "\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"" + texture + "\"\n}\n";
}

} // namespace

UT_TEST_CASE("ui texture: texture references are normalized like the engine") {
    const std::vector<std::string> refs =
        ui::texture_references_from_vmt(vmt_with_base_texture("brick/Brick05"));
    UT_CHECK_EQ(refs.size(), static_cast<std::size_t>(1));
    if (!refs.empty()) {
        UT_CHECK_EQ(refs[0], std::string("materials/brick/brick05.vtf"));
    }

    // Already-prefixed and extension-carrying values must not be mangled.
    const std::vector<std::string> prefixed =
        ui::texture_references_from_vmt(vmt_with_base_texture("materials/dev/dev01.vtf"));
    UT_CHECK_EQ(prefixed.size(), static_cast<std::size_t>(1));
    if (!prefixed.empty()) {
        UT_CHECK_EQ(prefixed[0], std::string("materials/dev/dev01.vtf"));
    }

    // Backslashes are the most common authoring mistake; accept them.
    const std::vector<std::string> slashes =
        ui::texture_references_from_vmt(vmt_with_base_texture("brick\\brick05"));
    UT_CHECK_EQ(slashes.size(), static_cast<std::size_t>(1));
    if (!slashes.empty()) {
        UT_CHECK_EQ(slashes[0], std::string("materials/brick/brick05.vtf"));
    }
}

UT_TEST_CASE("ui texture: $basetexture wins over $texture2") {
    const std::string vmt = "\"UnlitGeneric\"\n{\n"
                            "\t\"$texture2\" \"decals/decals001\"\n"
                            "\t\"$basetexture\" \"brick/brick05\"\n"
                            "\t\"$texture3\" \"metal/metalwall001\"\n"
                            "}\n";
    const std::vector<std::string> refs = ui::texture_references_from_vmt(vmt);
    UT_CHECK_EQ(refs.size(), static_cast<std::size_t>(3));
    if (refs.size() == 3) {
        UT_CHECK_EQ(refs[0], std::string("materials/brick/brick05.vtf"));
        UT_CHECK_EQ(refs[1], std::string("materials/decals/decals001.vtf"));
        UT_CHECK_EQ(refs[2], std::string("materials/metal/metalwall001.vtf"));
    }
}

UT_TEST_CASE("ui texture: render targets and env_cubemap are skipped") {
    const std::string vmt = "\"UnlitGeneric\"\n{\n"
                            "\t\"$basetexture\" \"_rt_fullscreen\"\n"
                            "\t\"$texture2\" \"env_cubemap\"\n"
                            "}\n";
    UT_CHECK(ui::texture_references_from_vmt(vmt).empty());
}

UT_TEST_CASE("ui texture: a duplicated texture reference is only listed once") {
    const std::string vmt = "\"UnlitGeneric\"\n{\n"
                            "\t\"$basetexture\" \"brick/brick05\"\n"
                            "\t\"$texture2\" \"BRICK/Brick05\"\n"
                            "}\n";
    UT_CHECK_EQ(ui::texture_references_from_vmt(vmt).size(), static_cast<std::size_t>(1));
}

UT_TEST_CASE("ui texture: a material with no texture reports NotFound") {
    FakeTextureSource source;
    const auto thumbnail =
        ui::make_texture_thumbnail("\"UnlitGeneric\"\n{\n}\n", source, 64);
    UT_CHECK(!thumbnail.ok());
    if (!thumbnail.ok()) {
        UT_CHECK_EQ(static_cast<int>(thumbnail.error().code),
                    static_cast<int>(vmt::ErrorCode::NotFound));
    }
}

UT_TEST_CASE("ui texture: a real VTF decodes to a non-empty BGRA thumbnail") {
    FakeTextureSource source;
    source.add("materials/brick/test05.vtf", make_dxt1_4x4());

    const auto thumbnail =
        ui::make_texture_thumbnail(vmt_with_base_texture("brick/test05"), source, 64);
    UT_CHECK(thumbnail.ok());
    if (!thumbnail.ok()) {
        return;
    }

    const ui::TextureThumbnail& image = thumbnail.value();
    UT_CHECK_EQ(image.width, 4);
    UT_CHECK_EQ(image.height, 4);
    UT_CHECK_EQ(image.bgra.size(), static_cast<std::size_t>(4 * 4 * 4));
    UT_CHECK_EQ(image.texture_name, std::string("materials/brick/test05.vtf"));

    // DXT1 block above is colour0=red, colour1=blue, all indices 0, so every
    // texel must decode to opaque red. An all-zero buffer would mean the
    // channel order or the decode went wrong, which is exactly the failure this
    // pipeline replaces.
    std::size_t red_pixels = 0;
    for (std::size_t i = 0; i + 3 < image.bgra.size(); i += 4) {
        if (image.bgra[i + 0] == 0 && image.bgra[i + 1] == 0 && image.bgra[i + 2] == 255 &&
            image.bgra[i + 3] == 255) {
            ++red_pixels;
        }
    }
    UT_CHECK_EQ(red_pixels, image.bgra.size() / 4);
    UT_CHECK(!image.has_alpha);
}

UT_TEST_CASE("ui texture: a decoded thumbnail writes a valid BMP") {
    FakeTextureSource source;
    source.add("materials/brick/test05.vtf", make_dxt1_4x4());
    const auto thumbnail =
        ui::make_texture_thumbnail(vmt_with_base_texture("brick/test05"), source, 64);
    UT_CHECK(thumbnail.ok());
    if (!thumbnail.ok()) {
        return;
    }

    const fs::path bmp = fresh_temp_dir() / "thumb.bmp";
    const ui::TextureThumbnail& image = thumbnail.value();
    UT_CHECK(ui::write_bmp_from_bgra(bmp, image.width, image.height, image.bgra.data(), true));

    int width = 0;
    int height = 0;
    UT_CHECK(ui::read_bmp_size(bmp, width, height));
    UT_CHECK_EQ(width, 4);
    UT_CHECK_EQ(height, 4);
}

UT_TEST_CASE("ui texture: a missing texture reports NotFound, an empty buffer does not crash") {
    FakeTextureSource source;
    source.add("materials/brick/test05.vtf", {});

    const auto missing =
        ui::make_texture_thumbnail(vmt_with_base_texture("brick/nothere"), source, 64);
    UT_CHECK(!missing.ok());
    if (!missing.ok()) {
        UT_CHECK_EQ(static_cast<int>(missing.error().code),
                    static_cast<int>(vmt::ErrorCode::NotFound));
    }

    // An empty file is a real failure and must be reported, not dereferenced.
    const auto empty =
        ui::make_texture_thumbnail(vmt_with_base_texture("brick/test05"), source, 64);
    UT_CHECK(!empty.ok());
    if (!empty.ok()) {
        UT_CHECK(!empty.error().message.empty());
    }
}

UT_TEST_CASE("ui texture: a failure names every texture it attempted") {
    // The browser decides whether to fall back to the engine render from this
    // message, so it has to name each file and say why that one failed. The
    // "why" text belongs to whichever layer rejected it (the source for a read
    // failure, the decoder for a bad header), so the test only pins the parts
    // this module owns: both paths listed, and each carrying its own reason.
    FakeTextureSource source;
    const std::string vmt = "\"UnlitGeneric\"\n{\n"
                            "\t\"$basetexture\" \"hdr/skies_hdr\"\n"
                            "\t\"$texture2\" \"gone/missing\"\n"
                            "}\n";
    const auto thumbnail = ui::make_texture_thumbnail(vmt, source, 64);
    UT_CHECK(!thumbnail.ok());
    if (!thumbnail.ok()) {
        const std::string& message = thumbnail.error().message;
        UT_CHECK(message.find("hdr/skies_hdr") != std::string::npos);
        UT_CHECK(message.find("gone/missing") != std::string::npos);
        // The unreadable texture contributes the source's own "not found".
        UT_CHECK(message.find("not found") != std::string::npos);
        // The present-but-broken one contributes a reason too, so the two are
        // distinguishable in the status line.
        UT_CHECK(message.find("hdr/skies_hdr") < message.find("gone/missing"));
    }
}

UT_TEST_CASE("ui texture: a corrupt texture does not stop a later valid one") {
    // $basetexture is corrupt; $texture2 is fine. The thumbnail must come from
    // $texture2 rather than reporting failure.
    FakeTextureSource source;
    std::vector<unsigned char> garbage(128, 0xAB);
    source.add("materials/broken/broken05.vtf", garbage);
    source.add("materials/brick/test05.vtf", make_dxt1_4x4());

    const std::string vmt = "\"UnlitGeneric\"\n{\n"
                            "\t\"$basetexture\" \"broken/broken05\"\n"
                            "\t\"$texture2\" \"brick/test05\"\n"
                            "}\n";
    const auto thumbnail = ui::make_texture_thumbnail(vmt, source, 64);
    UT_CHECK(thumbnail.ok());
    if (thumbnail.ok()) {
        UT_CHECK_EQ(thumbnail.value().texture_name,
                    std::string("materials/brick/test05.vtf"));
    }
}

UT_TEST_CASE("ui texture: references are tried in order and stop at the first hit") {
    FakeTextureSource source;
    source.add("materials/decals/decals001.vtf", make_dxt1_4x4());

    const std::string vmt = "\"UnlitGeneric\"\n{\n"
                            "\t\"$basetexture\" \"brick/absent\"\n"
                            "\t\"$texture2\" \"decals/decals001\"\n"
                            "}\n";
    const auto thumbnail = ui::make_texture_thumbnail(vmt, source, 64);
    UT_CHECK(thumbnail.ok());
    if (thumbnail.ok()) {
        UT_CHECK_EQ(thumbnail.value().texture_name,
                    std::string("materials/decals/decals001.vtf"));
    }
    // It must not keep asking after a success.
    UT_CHECK_EQ(source.requests.size(), static_cast<std::size_t>(2));
}

UT_TEST_CASE("ui texture: max_size downscales while preserving aspect") {
    // 16x4 texture, so an 8-pixel cap must yield 8x2 and not 8x8.
    FakeTextureSource source;
    source.add("materials/wide/wide05.vtf", make_dxt1_vtf(16, 4, 4));
    const auto thumbnail =
        ui::make_texture_thumbnail(vmt_with_base_texture("wide/wide05"), source, 8);
    UT_CHECK(thumbnail.ok());
    if (thumbnail.ok()) {
        UT_CHECK_EQ(thumbnail.value().width, 8);
        UT_CHECK_EQ(thumbnail.value().height, 2);
        UT_CHECK_EQ(thumbnail.value().bgra.size(), static_cast<std::size_t>(8 * 2 * 4));
    }
}

// --- material info panel -------------------------------------------------------

UT_TEST_CASE("ui info: non-base texture parameters are discovered too") {
    // $basetexture alone is not enough: bump/normal/detail/envmap all reference
    // files the user wants sized. The schema drives this when available, and the
    // name heuristic covers shaders with no schema.
    FakeTextureSource source;
    source.add("materials/cleantextures/brick01.vtf", make_dxt1_vtf(8, 8, 2));
    source.add("materials/cleantextures/brickbumpmap.vtf", make_dxt1_vtf(8, 8, 2));

    const std::string vmt = "\"LightmappedGeneric\"\n{\n"
                            "\t\"$basetexture\" \"cleantextures/brick01\"\n"
                            "\t\"$bumpmap\" \"cleantextures/brickbumpmap\"\n"
                            "\t\"$surfaceprop\" \"brick\"\n"
                            "}\n";
    const ui::MaterialInfo info = ui::collect_material_info(vmt, source, false);
    UT_CHECK_EQ(info.textures.size(), static_cast<std::size_t>(2));
    if (info.textures.size() == 2) {
        UT_CHECK_EQ(info.textures[0].parameter, std::string("$basetexture"));
        UT_CHECK_EQ(info.textures[1].parameter, std::string("$bumpmap"));
    }
    // $surfaceprop must not be mistaken for a texture.
    UT_CHECK(!ui::is_texture_parameter_name("$surfaceprop"));
    UT_CHECK(ui::is_texture_parameter_name("$bumpmap"));
    UT_CHECK(ui::is_texture_parameter_name("$normalmap"));
    UT_CHECK(ui::is_texture_parameter_name("$detail"));
    UT_CHECK(ui::is_texture_parameter_name("$basetexture2"));
    UT_CHECK(!ui::is_texture_parameter_name("$color"));
    UT_CHECK(!ui::is_texture_parameter_name("$phong"));
}

UT_TEST_CASE("ui info: schema-declared textures are followed even without a known suffix") {
    // A schema can name a texture parameter that the heuristic would miss.
    FakeTextureSource source;
    source.add("materials/a/alpha.vtf", make_dxt1_vtf(8, 8, 2));

    const std::string vmt = "\"LightmappedGeneric\"\n{\n"
                            "\t\"$alphamaskmodulate\" \"a/alpha\"\n"
                            "}\n";
    const std::set<std::string> schema = { "$alphamaskmodulate" };
    const ui::MaterialInfo info = ui::collect_material_info(vmt, source, true, schema);
    UT_CHECK_EQ(info.textures.size(), static_cast<std::size_t>(1));
    if (!info.textures.empty()) {
        UT_CHECK_EQ(info.textures[0].parameter, std::string("$alphamaskmodulate"));
        UT_CHECK(info.textures[0].found);
    }
}

UT_TEST_CASE("ui info: a plain material reports its shader and one texture") {
    FakeTextureSource source;
    source.add("materials/brick/test05.vtf", make_dxt1_vtf(16, 16, 4));

    const ui::MaterialInfo info =
        ui::collect_material_info(vmt_with_base_texture("brick/test05"), source, true);
    UT_CHECK_EQ(info.shader, std::string("UnlitGeneric"));
    UT_CHECK(info.shader_known);
    UT_CHECK_EQ(info.textures.size(), static_cast<std::size_t>(1));
    UT_CHECK(!info.any_texture_missing);
    UT_CHECK(!info.any_texture_undecodable);

    const ui::TextureInfo& texture = info.textures[0];
    UT_CHECK_EQ(texture.parameter, std::string("$basetexture"));
    UT_CHECK_EQ(texture.game_path, std::string("materials/brick/test05.vtf"));
    UT_CHECK(texture.found);
    UT_CHECK(texture.decoded);
    UT_CHECK_EQ(texture.format_name, std::string("DXT1"));
    UT_CHECK_EQ(texture.width, 16u);
    UT_CHECK_EQ(texture.height, 16u);
    UT_CHECK_EQ(texture.vram_size, 16u * 16u * 4u);
    UT_CHECK(!texture.is_cubemap);
    UT_CHECK(!texture.is_hdr);
}

UT_TEST_CASE("ui info: every referenced texture is described, not just the first") {
    FakeTextureSource source;
    source.add("materials/brick/test05.vtf", make_dxt1_vtf(8, 8, 2));
    source.add("materials/decals/decals001.vtf", make_dxt1_vtf(8, 8, 2));

    const std::string vmt = "\"UnlitGeneric\"\n{\n"
                            "\t\"$basetexture\" \"brick/test05\"\n"
                            "\t\"$texture2\" \"decals/decals001\"\n"
                            "}\n";
    const ui::MaterialInfo info = ui::collect_material_info(vmt, source, true);
    UT_CHECK_EQ(info.textures.size(), static_cast<std::size_t>(2));
    if (info.textures.size() == 2) {
        // The parameter each texture came from must survive, since that is how
        // the panel explains what the material is doing.
        UT_CHECK_EQ(info.textures[0].parameter, std::string("$basetexture"));
        UT_CHECK_EQ(info.textures[1].parameter, std::string("$texture2"));
        UT_CHECK(info.textures[1].found);
    }
    UT_CHECK_EQ(info.vram_total, 8u * 8u * 4u * 2u);
}

UT_TEST_CASE("ui info: a missing texture is reported but does not fail the scan") {
    FakeTextureSource source;
    source.add("materials/brick/test05.vtf", make_dxt1_vtf(8, 8, 2));

    const std::string vmt = "\"UnlitGeneric\"\n{\n"
                            "\t\"$basetexture\" \"brick/test05\"\n"
                            "\t\"$texture2\" \"gone/absent\"\n"
                            "}\n";
    const ui::MaterialInfo info = ui::collect_material_info(vmt, source, true);
    UT_CHECK_EQ(info.textures.size(), static_cast<std::size_t>(2));
    if (info.textures.size() == 2) {
        UT_CHECK(info.textures[0].found);
        UT_CHECK(!info.textures[1].found);
        UT_CHECK(!info.textures[1].note.empty());
    }
    UT_CHECK(info.any_texture_missing);
    // The good texture still contributes its VRAM.
    UT_CHECK(info.vram_total > 0);
}

UT_TEST_CASE("ui info: a corrupt texture keeps its size but is marked undecodable") {
    FakeTextureSource source;
    std::vector<unsigned char> garbage(2048, 0x5A);
    source.add("materials/broken/broken05.vtf", garbage);

    const ui::MaterialInfo info =
        ui::collect_material_info(vmt_with_base_texture("broken/broken05"), source, true);
    UT_CHECK_EQ(info.textures.size(), static_cast<std::size_t>(1));
    if (!info.textures.empty()) {
        const ui::TextureInfo& texture = info.textures[0];
        UT_CHECK(texture.found);
        UT_CHECK(!texture.decoded);
        UT_CHECK(!texture.note.empty());
        UT_CHECK_EQ(texture.file_size, static_cast<std::uint64_t>(garbage.size()));
    }
    UT_CHECK(info.any_texture_undecodable);
}

UT_TEST_CASE("ui info: a material with no texture still reports its shader") {
    FakeTextureSource source;
    const ui::MaterialInfo info =
        ui::collect_material_info("\"UnlitGeneric\"\n{\n\t\"$color\" \"[1 1 1]\"\n}\n",
                                  source, true);
    UT_CHECK_EQ(info.shader, std::string("UnlitGeneric"));
    UT_CHECK(info.textures.empty());
    UT_CHECK_EQ(info.parameter_count, static_cast<std::size_t>(1));
    UT_CHECK(!info.parameter_keys.empty());
}

UT_TEST_CASE("ui info: an unknown shader is flagged") {
    FakeTextureSource source;
    const ui::MaterialInfo info =
        ui::collect_material_info("\"SomeCustomShader\"\n{\n}\n", source, false);
    UT_CHECK_EQ(info.shader, std::string("SomeCustomShader"));
    UT_CHECK(!info.shader_known);
}

UT_TEST_CASE("ui info: parse diagnostics are counted") {
    FakeTextureSource source;
    // An unterminated string is a parse diagnostic; the parser still recovers.
    const ui::MaterialInfo info =
        ui::collect_material_info("\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"oops\n}\n",
                                  source, true);
    UT_CHECK(info.diagnostic_count > 0);
}

UT_TEST_CASE("ui info: formatting includes size, format and the texture path") {
    FakeTextureSource source;
    source.add("materials/brick/test05.vtf", make_dxt1_vtf(16, 16, 4));

    const ui::MaterialInfo info =
        ui::collect_material_info(vmt_with_base_texture("brick/test05"), source, true);
    const std::string text = ui::format_material_info(info);

    UT_CHECK(text.find("UnlitGeneric") != std::string::npos);
    UT_CHECK(text.find("materials/brick/test05.vtf") != std::string::npos);
    UT_CHECK(text.find("$basetexture") != std::string::npos);
    UT_CHECK(text.find("16x16") != std::string::npos);
    UT_CHECK(text.find("DXT1") != std::string::npos);
    UT_CHECK(text.find("VTF 7") != std::string::npos);
    // The whole point of the panel: a human-readable size for both disk and VRAM.
    UT_CHECK(text.find("file ") != std::string::npos);
    UT_CHECK(text.find("vram ") != std::string::npos);
    UT_CHECK(text.find("VRAM") != std::string::npos);
}

UT_TEST_CASE("ui info: byte formatting switches units") {
    UT_CHECK_EQ(ui::format_bytes(0), std::string("0 B"));
    UT_CHECK_EQ(ui::format_bytes(312), std::string("312 B"));
    UT_CHECK_EQ(ui::format_bytes(1024), std::string("1 KB"));
    UT_CHECK_EQ(ui::format_bytes(853504), std::string("834 KB"));
    // 1.5 MB keeps one decimal; 12 MB does not need one.
    UT_CHECK_EQ(ui::format_bytes(1572864), std::string("1.5 MB"));
    UT_CHECK_EQ(ui::format_bytes(12582912), std::string("12 MB"));
}

UT_TEST_CASE("ui info: a material without textures formats without crashing") {
    FakeTextureSource source;
    const ui::MaterialInfo info =
        ui::collect_material_info("\"UnlitGeneric\"\n{\n}\n", source, true);
    const std::string text = ui::format_material_info(info);
    UT_CHECK(text.find("Textures") != std::string::npos);
    UT_CHECK(!text.empty());
}

// --- Phase 9: preview options --------------------------------------------------

UT_TEST_CASE("ui preview options: environments pick rigs and directions") {
    ui::PreviewOptions options;
    ui::apply_environment(options, ui::PreviewEnvironment::Outdoor);
    UT_CHECK(options.environment == ui::PreviewEnvironment::Outdoor);
    UT_CHECK(options.rig.ambient > 0.3f);
    // The key direction points away from the light, so it must be non-zero.
    const float length = options.lighting.key_dir[0] * options.lighting.key_dir[0] +
                         options.lighting.key_dir[1] * options.lighting.key_dir[1] +
                         options.lighting.key_dir[2] * options.lighting.key_dir[2];
    UT_CHECK(length > 0.9f && length < 1.1f);

    ui::apply_environment(options, ui::PreviewEnvironment::Flat);
    UT_CHECK(options.lighting.key_color[0] == 0.0f); // no directional key
    UT_CHECK(options.lighting.ambient[0] > 0.8f);

    // A hand-edited rig switches to Custom and clamps out-of-range values.
    ui::PreviewRig rig;
    rig.ambient = 4.0f;
    rig.key_azimuth = 900.0f;
    rig.key_elevation = -400.0f;
    rig.key_intensity = 5.0f;
    rig.background = -1.0f;
    ui::apply_rig(options, rig);
    UT_CHECK(options.environment == ui::PreviewEnvironment::Custom);
    UT_CHECK_EQ(options.rig.ambient, 1.0f);
    UT_CHECK_EQ(options.rig.key_azimuth, 180.0f);
    UT_CHECK_EQ(options.rig.key_elevation, -90.0f);
    UT_CHECK_EQ(options.rig.key_intensity, 2.0f);
    UT_CHECK_EQ(options.rig.background, 0.0f);
}

UT_TEST_CASE("ui preview options: ids and settings round-trip") {
    ui::PreviewOptions options;
    options.shape = ui::PreviewShape::Cylinder;
    ui::apply_environment(options, ui::PreviewEnvironment::Outdoor);

    ui::UISettings settings;
    ui::options_to_settings(options, settings);
    UT_CHECK_EQ(settings.preview_shape, std::string("cylinder"));
    UT_CHECK_EQ(settings.preview_environment, std::string("outdoor"));

    const ui::PreviewOptions restored = ui::options_from_settings(settings);
    UT_CHECK(restored.shape == ui::PreviewShape::Cylinder);
    UT_CHECK(restored.environment == ui::PreviewEnvironment::Outdoor);
    UT_CHECK(restored.lighting.key_color[0] == options.lighting.key_color[0]);

    // A malformed rig line keeps the defaults instead of failing.
    ui::UISettings broken;
    broken.preview_rig = "not numbers";
    const ui::PreviewOptions fallback = ui::options_from_settings(broken);
    UT_CHECK(fallback.environment == ui::PreviewEnvironment::StudioDark);

    ui::PreviewShape shape;
    UT_CHECK(ui::parse_shape("plane", shape) && shape == ui::PreviewShape::Plane);
    UT_CHECK(ui::parse_shape("brush_wall", shape) && shape == ui::PreviewShape::BrushWall);
    UT_CHECK(ui::parse_shape("brush_floor", shape) && shape == ui::PreviewShape::BrushFloor);
    UT_CHECK(ui::parse_shape("brush_cube", shape) && shape == ui::PreviewShape::BrushCube);
    UT_CHECK(ui::parse_shape("brush_displacement", shape) && shape == ui::PreviewShape::BrushDisplacement);
    UT_CHECK(!ui::parse_shape("dodecahedron", shape));
}

UT_TEST_CASE("ui preview options: is_brush_shape and is_model_shape") {
    UT_CHECK(ui::is_brush_shape(ui::PreviewShape::BrushWall));
    UT_CHECK(ui::is_brush_shape(ui::PreviewShape::BrushFloor));
    UT_CHECK(ui::is_brush_shape(ui::PreviewShape::BrushCube));
    UT_CHECK(ui::is_brush_shape(ui::PreviewShape::BrushDisplacement));
    UT_CHECK(!ui::is_brush_shape(ui::PreviewShape::Sphere));
    UT_CHECK(!ui::is_brush_shape(ui::PreviewShape::Cube));
    UT_CHECK(!ui::is_brush_shape(ui::PreviewShape::Cylinder));
    UT_CHECK(!ui::is_brush_shape(ui::PreviewShape::Plane));

    UT_CHECK(!ui::is_model_shape(ui::PreviewShape::BrushWall));
    UT_CHECK(!ui::is_model_shape(ui::PreviewShape::BrushFloor));
    UT_CHECK(!ui::is_model_shape(ui::PreviewShape::BrushCube));
    UT_CHECK(!ui::is_model_shape(ui::PreviewShape::BrushDisplacement));
    UT_CHECK(ui::is_model_shape(ui::PreviewShape::Sphere));
    UT_CHECK(ui::is_model_shape(ui::PreviewShape::Cube));
    UT_CHECK(ui::is_model_shape(ui::PreviewShape::Cylinder));
    UT_CHECK(ui::is_model_shape(ui::PreviewShape::Plane));
}

UT_TEST_CASE("ui shader catalog: is_brush_only_shader") {
    // Lightmapped shaders
    UT_CHECK(ui::is_brush_only_shader("LightmappedGeneric"));
    UT_CHECK(ui::is_brush_only_shader("lightmappedgeneric"));
    UT_CHECK(ui::is_brush_only_shader("LIGHTMAPPEDGENERIC"));
    UT_CHECK(ui::is_brush_only_shader("Lightmapped_4WayBlend"));
    UT_CHECK(ui::is_brush_only_shader("LightmappedReflective"));

    // World surface shaders
    UT_CHECK(ui::is_brush_only_shader("WorldVertexTransition"));
    UT_CHECK(ui::is_brush_only_shader("worldvertextransition"));
    UT_CHECK(ui::is_brush_only_shader("WorldTwoTextureBlend"));
    UT_CHECK(ui::is_brush_only_shader("WorldVertexLightmap"));

    // Other brush / Hammer exclusive shaders
    UT_CHECK(ui::is_brush_only_shader("Lightmap"));
    UT_CHECK(ui::is_brush_only_shader("lightmap"));
    UT_CHECK(ui::is_brush_only_shader("UnlitGenericLightmap"));
    UT_CHECK(ui::is_brush_only_shader("Multiblend"));
    UT_CHECK(ui::is_brush_only_shader("Water"));
    UT_CHECK(ui::is_brush_only_shader("water"));
    UT_CHECK(ui::is_brush_only_shader("WaterRefract"));
    UT_CHECK(ui::is_brush_only_shader("WaterReflect"));
    UT_CHECK(ui::is_brush_only_shader("ShatteredGlass"));
    UT_CHECK(ui::is_brush_only_shader("VolumeFog"));
    UT_CHECK(ui::is_brush_only_shader("Lava"));
    UT_CHECK(ui::is_brush_only_shader("Slime"));

    // Model and non-brush shaders must return false
    UT_CHECK(!ui::is_brush_only_shader("VertexLitGeneric"));
    UT_CHECK(!ui::is_brush_only_shader("vertexlitgeneric"));
    UT_CHECK(!ui::is_brush_only_shader("UnlitGeneric"));
    UT_CHECK(!ui::is_brush_only_shader("unlitgeneric"));
    UT_CHECK(!ui::is_brush_only_shader("UnlitTwoTexture"));
    UT_CHECK(!ui::is_brush_only_shader("Refract"));
    UT_CHECK(!ui::is_brush_only_shader("Sprite"));
    UT_CHECK(!ui::is_brush_only_shader("PbrModel"));
    UT_CHECK(!ui::is_brush_only_shader("PostProcess"));
    UT_CHECK(!ui::is_brush_only_shader(""));
}

// --- Phase 9: settings persistence ---------------------------------------------

UT_TEST_CASE("ui settings: extended keys round-trip and save is atomic") {
    const fs::path file = fresh_temp_dir() / "settings.ini";
    ui::UISettings settings;
    settings.preview_shape = "cube";
    settings.preview_environment = "flat";
    settings.preview_rig = "0.5000 10.0000 -20.0000 1.5000 0.7500";
    settings.browser_root = "D:\\games\\hl2\\materials";
    settings.browser_scan_game = true;
    settings.thumbnail_flip = false;
    settings.asset_kind = 2;
    settings.asset_source = 2;
    settings.asset_grid = true;
    settings.asset_sort = "3,desc";
    settings.asset_folders = {"D:\\mods\\one\\materials", "D:\\mods\\two\\materials"};
    settings.asset_geometry = "QUJD";

    UT_CHECK(settings.save(file));
    const ui::UISettings loaded = ui::UISettings::load(file);
    UT_CHECK_EQ(loaded.preview_shape, std::string("cube"));
    UT_CHECK_EQ(loaded.preview_environment, std::string("flat"));
    UT_CHECK_EQ(loaded.preview_rig, settings.preview_rig);
    UT_CHECK_EQ(loaded.browser_root, settings.browser_root);
    UT_CHECK(loaded.browser_scan_game);
    UT_CHECK(!loaded.thumbnail_flip);
    UT_CHECK_EQ(loaded.asset_kind, 2);
    UT_CHECK_EQ(loaded.asset_source, 2);
    UT_CHECK(loaded.asset_grid);
    UT_CHECK_EQ(loaded.asset_sort, std::string("3,desc"));
    UT_CHECK_EQ(loaded.asset_folders, settings.asset_folders);
    UT_CHECK_EQ(loaded.asset_geometry, std::string("QUJD"));

    // The atomic write leaves no temporary file behind.
    UT_CHECK(!fs::exists(fs::path(file.string() + ".tmp")));
}

// --- Phase 9: virtual session + preview override path ---------------------------

UT_TEST_CASE("ui session: open_text keeps a virtual name and forces Save As") {
    ui::EditorSession session;
    const std::string text =
        "\"LightmappedGeneric\"\n"
        "{\n"
        "    \"$basetexture\" \"brick/brick05\"\n"
        "}\n";
    UT_CHECK(session.open_text(text, "brick/brickwall001a.vmt"));
    UT_CHECK(session.has_document());
    UT_CHECK(session.text_is_valid());
    UT_CHECK(session.path().empty());
    UT_CHECK_EQ(session.display_name(), std::string("brick/brickwall001a.vmt"));
    UT_CHECK(!session.dirty());

    // An edit is dirty, and plain Save cannot target a virtual material.
    UT_CHECK(session.set_parameter("$basetexture", "brick/brick06"));
    UT_CHECK(session.dirty());
    const vmt::Result<void> saved = session.save();
    UT_CHECK(!saved.ok());

    const fs::path target = fresh_temp_dir() / "brickwall001a.vmt";
    UT_CHECK(session.save_as(target));
    UT_CHECK(!session.dirty());
    UT_CHECK_EQ(session.display_name(), std::string("brickwall001a.vmt"));
}

UT_TEST_CASE("ui preview: override writes at the material's own path and is atomic") {
    const fs::path root = fresh_temp_dir() / "override";
    UT_CHECK(ui::write_preview_material_at(root, "brick/brickwall001a", "\"UnlitGeneric\"\n{}\n"));
    const fs::path written = root / "materials" / "brick" / "brickwall001a.vmt";
    UT_CHECK(fs::exists(written));
    UT_CHECK_EQ(read_all(written), std::string("\"UnlitGeneric\"\n{}\n"));
    UT_CHECK(!fs::exists(fs::path(written.string() + ".tmp")));

    // Unsafe names are rejected without touching the disk.
    UT_CHECK(!ui::write_preview_material_at(root, "..\\..\\evil", "x").ok());
    UT_CHECK(!ui::write_preview_material_at(root, "C:\\windows\\evil", "x").ok());
    UT_CHECK(!ui::write_preview_material_at(root, "", "x").ok());
    UT_CHECK(!fs::exists(root / "evil.vmt"));
}

UT_TEST_CASE("ui session: set_shader rewrites the header and is undoable") {
    ui::EditorSession session;
    const std::string text =
        "\"VertexLitGeneric\"\n"
        "{\n"
        "    \"$basetexture\" \"brick/brick05\"\n"
        "    \"$vertexcolor\" 1\n"
        "}\n";
    UT_CHECK(session.open_text(text, "brick/test.vmt"));
    UT_CHECK(!session.dirty());

    UT_CHECK(session.set_shader("UnlitGeneric"));
    UT_CHECK(session.dirty());
    UT_CHECK(session.text_is_valid());
    UT_CHECK(session.document() != nullptr);
    if (session.document() != nullptr) {
        UT_CHECK(session.document()->shader() == "UnlitGeneric");
    }
    // Parameters survive the shader switch: the switch never deletes data.
    UT_CHECK(session.text().find("\"$basetexture\" \"brick/brick05\"") != std::string::npos);
    UT_CHECK(session.text().find("VertexLitGeneric") == std::string::npos);

    UT_CHECK(session.undo());
    UT_CHECK_EQ(session.text(), text);
    if (session.document() != nullptr) {
        UT_CHECK(session.document()->shader() == "VertexLitGeneric");
    }
    UT_CHECK(session.redo());
    if (session.document() != nullptr) {
        UT_CHECK(session.document()->shader() == "UnlitGeneric");
    }
}

UT_TEST_CASE("ui session: set_shader refuses names that are not a single token") {
    ui::EditorSession session;
    const std::string text = "\"VertexLitGeneric\"\n{\n}\n";
    UT_CHECK(session.open_text(text, "a.vmt"));

    UT_CHECK(!session.set_shader("Vertex Lit").ok());
    UT_CHECK(!session.set_shader("").ok());
    UT_CHECK(!session.set_shader("Vertex{Lit").ok());

    // A rejected name leaves the document, the text and the dirty flag alone.
    if (session.document() != nullptr) {
        UT_CHECK(session.document()->shader() == "VertexLitGeneric");
    }
    UT_CHECK_EQ(session.text(), text);
    UT_CHECK(!session.dirty());
    UT_CHECK(session.last_error().code != vmt::ErrorCode::Ok);

    // ...and a valid one still works afterwards.
    UT_CHECK(session.set_shader("UnlitGeneric"));
    if (session.document() != nullptr) {
        UT_CHECK(session.document()->shader() == "UnlitGeneric");
    }
}

UT_TEST_CASE("shader catalog: names are sorted, unique and cover the schema set") {
    const std::vector<std::string>& known = ui::known_shader_names();
    UT_CHECK(known.size() > 20);

    // Sorted case-insensitively and free of duplicates, as documented.
    std::string previous_key;
    for (std::size_t i = 0; i < known.size(); ++i) {
        UT_CHECK(!known[i].empty());
        const std::string key = lower_ascii(known[i]);
        if (i > 0) {
            UT_CHECK(previous_key < key);
        }
        previous_key = key;
    }

    UT_CHECK(ui::find_shader_picker_index(known, "VertexLitGeneric") >= 0);
    UT_CHECK(ui::find_shader_picker_index(known, "vertexlitgeneric") >= 0);
    UT_CHECK_EQ(ui::find_shader_picker_index(known, "vertexlitgeneric"),
                ui::find_shader_picker_index(known, "VertexLitGeneric"));
    UT_CHECK_EQ(ui::find_shader_picker_index(known, "NotAShader"), -1);
    UT_CHECK_EQ(ui::find_shader_picker_index(known, ""), -1);
}

UT_TEST_CASE("shader catalog: the picker merges the loaded schema shaders in") {
    const auto library = vmt::SchemaLibrary::load_dir(fs::path(VMT_SCHEMA_DIR));
    UT_CHECK(library.ok());
    if (!library.ok()) return;

    const std::vector<std::string> picker = ui::shader_picker_names(&library.value());
    UT_CHECK(picker.size() >= ui::known_shader_names().size());
    for (const std::string& name : library.value().shader_names()) {
        UT_CHECK(ui::find_shader_picker_index(picker, name) >= 0);
        // A shader the editor has a schema for must also be offered without the
        // schema library loaded, i.e. the catalog itself covers it.
        UT_CHECK(ui::find_shader_picker_index(ui::known_shader_names(), name) >= 0);
    }

    // Sorted and free of duplicates, case-insensitively.
    std::string previous_key;
    for (std::size_t i = 0; i < picker.size(); ++i) {
        const std::string key = lower_ascii(picker[i]);
        if (i > 0) {
            UT_CHECK(previous_key < key);
        }
        previous_key = key;
    }

    // A null library is valid and yields the bare catalog.
    const std::vector<std::string> bare = ui::shader_picker_names(nullptr);
    UT_CHECK_EQ(bare.size(), ui::known_shader_names().size());
}

UT_TEST_CASE("shader catalog: applying a shader repopulates the panel from its schema") {
    auto library = vmt::SchemaLibrary::load_dir(fs::path(VMT_SCHEMA_DIR));
    UT_CHECK(library.ok());
    if (!library.ok()) return;

    ui::EditorSession session;
    UT_CHECK(session.open_text(
        "\"UnlitGeneric\"\n"
        "{\n"
        "    \"$basetexture\" \"stone/stonefloor\"\n"
        "    \"$translucent\" 1\n"
        "}\n", "test.vmt"));

    UT_CHECK(session.set_shader("VertexLitGeneric"));
    const vmt::ShaderSchema schema =
        library.value().schema_for(session.document()->shader());

    const std::vector<ui::PanelGroup> groups = ui::build_panel_model(
        session.document(), schema, /*show_advanced=*/false);

    // The panel now offers VertexLitGeneric's parameters, and the parameters
    // carried over from the previous shader are still there.
    bool has_selfillum = false;
    bool shows_basetexture = false;
    for (const ui::PanelGroup& group : groups) {
        for (const ui::PanelRow& row : group.rows) {
            if (row.name == "$selfillum") has_selfillum = true;
            if (row.name == "$basetexture" && row.present) shows_basetexture = true;
        }
    }
    UT_CHECK(has_selfillum);
    UT_CHECK(shows_basetexture);
}

// --- Texture typo detection and graceful resolution tests -------------------

UT_TEST_CASE("ui validate: edit distance and string similarity metrics") {
    // Exact & Case
    UT_CHECK_EQ(ui::texture_edit_distance("b_wallpaper1", "b_wallpaper1"), std::size_t(0));
    UT_CHECK_EQ(ui::texture_edit_distance("B_Wallpaper1", "b_wallpaper1"), std::size_t(0));
    UT_CHECK(ui::are_texture_names_similar("B_Wallpaper1", "b_wallpaper1"));

    // Prompt example: b_wallpaper1 vs b_wallaper1 (single vs double letter, edit distance 1)
    UT_CHECK_EQ(ui::texture_edit_distance("b_wallpaper1", "b_wallaper1"), std::size_t(1));
    UT_CHECK(ui::are_texture_names_similar("b_wallpaper1", "b_wallaper1"));
    UT_CHECK(ui::are_texture_names_similar("b_wallaper1", "b_wallpaper1"));

    // Transposition
    UT_CHECK_EQ(ui::texture_edit_distance("b_wallpaer1", "b_wallpaper1"), std::size_t(1));
    UT_CHECK(ui::are_texture_names_similar("b_wallpaer1", "b_wallpaper1"));

    // Single vs double letters
    UT_CHECK(ui::are_texture_names_similar("wood_flor", "wood_floor"));
    UT_CHECK(ui::are_texture_names_similar("metal_gratte", "metal_grate"));
    UT_CHECK(ui::are_texture_names_similar("wood_plannk", "wood_plank"));

    // Distinct numbered textures should NOT be considered typos for one another
    UT_CHECK(!ui::are_texture_names_similar("wall1", "wall2"));
    UT_CHECK(!ui::are_texture_names_similar("b_wallaper1", "b_wallpaper2"));

    // Unrelated textures
    UT_CHECK(!ui::are_texture_names_similar("concrete", "brick"));
    UT_CHECK(!ui::are_texture_names_similar("dev_measuregeneric01", "metal_grate"));
}

UT_TEST_CASE("ui validate: find_similar_texture_name and graceful resolution on disk") {
    const fs::path root = fresh_temp_dir() / "typo_test";
    std::error_code ec;
    fs::create_directories(root / "materials" / "wallpaper", ec);
    fs::create_directories(root / "materials" / "wood", ec);
    fs::create_directories(root / "materials" / "metal", ec);

    // Create dummy texture files on disk
    std::ofstream(root / "materials" / "b_wallpaper1.vtf") << "vtf";
    std::ofstream(root / "materials" / "wallpaper" / "floral_pattern.vtf") << "vtf";
    std::ofstream(root / "materials" / "wood" / "wood_floor.vtf") << "vtf";
    std::ofstream(root / "materials" / "metal" / "metal_grate.vtf") << "vtf";

    const std::vector<fs::path> roots = { root };

    // 1. Missing 'p': b_wallaper1 -> b_wallpaper1
    UT_CHECK_EQ(ui::find_similar_texture_name("b_wallaper1", roots), std::string("b_wallpaper1"));

    // 2. Extra/omitted letters in subfolder: wood/wood_flor -> wood/wood_floor
    UT_CHECK_EQ(ui::find_similar_texture_name("wood/wood_flor", roots), std::string("wood/wood_floor"));

    // 3. Doubled letter: metal/metal_gratte -> metal/metal_grate
    UT_CHECK_EQ(ui::find_similar_texture_name("metal/metal_gratte", roots), std::string("metal/metal_grate"));

    // 4. Subfolder typo: wallpapers/floral_pattern -> wallpaper/floral_pattern
    UT_CHECK_EQ(ui::find_similar_texture_name("wallpapers/floral_pattern", roots),
                std::string("wallpaper/floral_pattern"));

    // 5. Case difference: B_WALLPAPER1 -> b_wallpaper1
    UT_CHECK_EQ(ui::find_similar_texture_name("B_WALLPAPER1", roots), std::string("b_wallpaper1"));

    // 6. Redundant materials/ and .vtf prefixes:
    UT_CHECK_EQ(ui::find_similar_texture_name("materials/b_wallaper1.vtf", roots),
                std::string("b_wallpaper1"));

    // 7. Graceful resolution to actual filesystem path:
    const fs::path resolved_typo = ui::resolve_texture_file_gracefully("materials/b_wallaper1.vtf", roots);
    UT_CHECK(!resolved_typo.empty());
    UT_CHECK(fs::is_regular_file(resolved_typo, ec));
    UT_CHECK_EQ(resolved_typo.filename().string(), std::string("b_wallpaper1.vtf"));

    const fs::path resolved_exact = ui::resolve_texture_file_gracefully("materials/b_wallpaper1.vtf", roots);
    UT_CHECK(!resolved_exact.empty());
    UT_CHECK(fs::is_regular_file(resolved_exact, ec));

    // 8. Completely missing texture returns empty
    UT_CHECK_EQ(ui::find_similar_texture_name("totally_nonexistent_texture_xyz", roots), std::string());
    UT_CHECK(ui::resolve_texture_file_gracefully("materials/totally_nonexistent.vtf", roots).empty());
}

UT_TEST_CASE("ui validate: missing texture suggestions in validation findings") {
    auto library = vmt::SchemaLibrary::load_dir(fs::path(VMT_SCHEMA_DIR));
    UT_CHECK(library.ok());
    if (!library.ok()) return;

    const fs::path root = fresh_temp_dir() / "val_typo_test";
    std::error_code ec;
    fs::create_directories(root / "materials", ec);
    std::ofstream(root / "materials" / "b_wallpaper1.vtf") << "vtf";

    const std::string text =
        "\"VertexLitGeneric\"\n"
        "{\n"
        "    \"$basetexture\" \"b_wallaper1\"\n"
        "}\n";

    // 1. Validation with material_roots (offline/disk): finds the disk file, detects typo, suggests fix
    ui::ValidationOptions disk_opts;
    disk_opts.schemas = &library.value();
    disk_opts.material_roots = { root };

    const std::vector<ui::Finding> findings = ui::validate_text(text, disk_opts);
    UT_CHECK_EQ(findings.size(), std::size_t(1));
    const ui::Finding* missing = find_finding(findings, "missing-texture");
    UT_CHECK(missing != nullptr);
    if (missing != nullptr) {
        UT_CHECK(missing->severity == ui::Severity::Error);
        UT_CHECK(missing->message.find("materials/b_wallaper1.vtf") != std::string::npos);
        UT_CHECK(missing->message.find("; did you mean \"b_wallpaper1\"?") != std::string::npos);
    }

    // 2. Validation with custom suggest_texture callback
    ui::ValidationOptions custom_opts;
    custom_opts.schemas = &library.value();
    custom_opts.resolve_texture = [](const std::string&) { return ui::TextureStatus::Missing; };
    custom_opts.suggest_texture = [](const std::string& name) {
        return name == "b_wallaper1" ? "b_wallpaper1" : "";
    };

    const std::vector<ui::Finding> custom_findings = ui::validate_text(text, custom_opts);
    UT_CHECK_EQ(custom_findings.size(), std::size_t(1));
    const ui::Finding* custom_missing = find_finding(custom_findings, "missing-texture");
    UT_CHECK(custom_missing != nullptr);
    if (custom_missing != nullptr) {
        UT_CHECK(custom_missing->message.find("; did you mean \"b_wallpaper1\"?") != std::string::npos);
    }

    // 3. When texture is missing and NO similar texture exists on disk, no suggestion is appended
    const std::string missing_text =
        "\"VertexLitGeneric\"\n"
        "{\n"
        "    \"$basetexture\" \"definitely_not_on_disk_xyz\"\n"
        "}\n";

    const std::vector<ui::Finding> no_suggest_findings = ui::validate_text(missing_text, disk_opts);
    UT_CHECK_EQ(no_suggest_findings.size(), std::size_t(1));
    const ui::Finding* no_suggest = find_finding(no_suggest_findings, "missing-texture");
    UT_CHECK(no_suggest != nullptr);
    if (no_suggest != nullptr) {
        UT_CHECK(no_suggest->message.find("materials/definitely_not_on_disk_xyz.vtf") != std::string::npos);
        UT_CHECK(no_suggest->message.find("did you mean") == std::string::npos);
    }
}

// --- Loose material folder and material root detection tests -----------------

UT_TEST_CASE("ui session: detect_material_root detects folder containing materials or file dir") {
    // 1. Path containing "materials" directory
    const fs::path p1 = fs::path("C:/games/hl2/hl2/materials/models/props/box.vmt");
    UT_CHECK_EQ(ui::detect_material_root(p1), fs::path("C:/games/hl2/hl2"));

    // 2. Case-insensitivity ("Materials", "MATERIALS")
    const fs::path p2 = fs::path("D:/my_addon/Materials/brick/wall01.vmt");
    UT_CHECK_EQ(ui::detect_material_root(p2), fs::path("D:/my_addon"));

    const fs::path p3 = fs::path("E:/stuff/MATERIALS/concrete.vmt");
    UT_CHECK_EQ(ui::detect_material_root(p3), fs::path("E:/stuff"));

    // 3. Nested path inside materials
    const fs::path p4 = fs::path("C:/custom/materials/a/b/c/d/deep.vmt");
    UT_CHECK_EQ(ui::detect_material_root(p4), fs::path("C:/custom"));

    // 4. File directory with a "materials" subfolder on disk
    const fs::path temp_dir = fresh_temp_dir() / "root_test";
    const fs::path mats_sub = temp_dir / "materials" / "wood";
    fs::create_directories(mats_sub);
    const fs::path loose_file = temp_dir / "loose.vmt";
    {
        std::ofstream out(loose_file);
        out << "\"LightmappedGeneric\"\n{}\n";
    }
    UT_CHECK_EQ(ui::detect_material_root(loose_file), temp_dir);

    // 5. File directory without "materials" anywhere (returns the file directory itself)
    const fs::path no_mats_dir = fresh_temp_dir() / "isolated" / "textures";
    fs::create_directories(no_mats_dir);
    const fs::path solo_file = no_mats_dir / "test.vmt";
    {
        std::ofstream out(solo_file);
        out << "\"LightmappedGeneric\"\n{}\n";
    }
    UT_CHECK_EQ(ui::detect_material_root(solo_file), no_mats_dir);

    // 6. Empty path returns empty
    UT_CHECK(ui::detect_material_root("").empty());
}

UT_TEST_CASE("ui session: open_file, save_as and open_text maintain material_root") {
    const fs::path base = fresh_temp_dir() / "session_root";
    const fs::path mats_dir = base / "materials" / "metal";
    fs::create_directories(mats_dir);

    const fs::path vmt_file = mats_dir / "iron.vmt";
    const std::string text = "\"VertexLitGeneric\"\n{\n    \"$basetexture\" \"metal/iron\"\n}\n";
    {
        std::ofstream out(vmt_file);
        out << text;
    }

    ui::EditorSession session;
    UT_CHECK(session.open_file(vmt_file).ok());
    UT_CHECK_EQ(session.material_root(), base);

    // open_text clears material_root
    UT_CHECK(session.open_text(text, "virtual.vmt").ok());
    UT_CHECK(session.material_root().empty());

    // save_as updates material_root
    const fs::path save_target = base / "materials" / "saved.vmt";
    UT_CHECK(session.save_as(save_target).ok());
    UT_CHECK_EQ(session.material_root(), base);

    // save_as to a loose folder without materials
    const fs::path loose_dir = fresh_temp_dir() / "loose_dest";
    fs::create_directories(loose_dir);
    const fs::path loose_target = loose_dir / "loose_saved.vmt";
    UT_CHECK(session.save_as(loose_target).ok());
    UT_CHECK_EQ(session.material_root(), loose_dir);
}

// --- Direct VTF opening tests ------------------------------------------------

UT_TEST_CASE("ui session: vtf_texture_path helper extracts correct relative texture paths") {
    // 1. Path with /materials/
    UT_CHECK_EQ(ui::vtf_texture_path("C:/game/materials/brick/wall01.vtf"),
                std::string("brick/wall01"));
    UT_CHECK_EQ(ui::vtf_texture_path("C:\\game\\garrysmod\\materials\\models\\props\\crate.vtf"),
                std::string("models/props/crate"));

    // 2. Path starting with materials/
    UT_CHECK_EQ(ui::vtf_texture_path("materials/decals/blood.vtf"),
                std::string("decals/blood"));
    UT_CHECK_EQ(ui::vtf_texture_path("materials\\custom\\test.vtf"),
                std::string("custom/test"));

    // 3. Redundant leading materials/materials/
    UT_CHECK_EQ(ui::vtf_texture_path("C:/foo/materials/materials/metal/iron.vtf"),
                std::string("metal/iron"));

    // 4. Loose file outside materials/
    UT_CHECK_EQ(ui::vtf_texture_path("C:/Downloads/loose_wood.vtf"),
                std::string("loose_wood"));
    UT_CHECK_EQ(ui::vtf_texture_path("texture.vtf"),
                std::string("texture"));

    // 5. Case-insensitivity for .vtf extension
    UT_CHECK_EQ(ui::vtf_texture_path("C:/game/materials/stone/floor.VTF"),
                std::string("stone/floor"));
    UT_CHECK_EQ(ui::vtf_texture_path("plain.VTF"),
                std::string("plain"));

    // 6. Empty path returns empty
    UT_CHECK(ui::vtf_texture_path("").empty());
}

UT_TEST_CASE("ui session: opening a valid VTF file generates an in-memory VMT document and reads metadata") {
    const fs::path vtf_file = fs::path(VMT_TEST_DATA_DIR) / "vtf" / "dxt1.vtf";
    UT_CHECK(fs::exists(vtf_file));

    ui::EditorSession session;
    UT_CHECK(!session.is_vtf());
    UT_CHECK(session.vtf_info() == nullptr);

    const vmt::Result<void> result = session.open_file(vtf_file);
    UT_CHECK(result.ok());
    UT_CHECK(session.is_vtf());
    UT_CHECK(session.vtf_info() != nullptr);
    if (session.vtf_info() != nullptr) {
        UT_CHECK_EQ(session.vtf_info()->width, 64u);
        UT_CHECK_EQ(session.vtf_info()->height, 64u);
        UT_CHECK_EQ(session.vtf_info()->format_name, std::string("DXT1"));
    }

    UT_CHECK(session.has_document());
    UT_CHECK(session.text_is_valid());
    UT_CHECK(!session.dirty());
    UT_CHECK_EQ(session.display_name(), std::string("dxt1.vtf"));

    const vmt::Document* doc = session.document();
    UT_CHECK(doc != nullptr);
    if (doc != nullptr) {
        UT_CHECK_EQ(std::string(doc->shader()), std::string("UnlitGeneric"));
        const vmt::Node* base = doc->find("$basetexture");
        UT_CHECK(base != nullptr);
        if (base != nullptr) {
            UT_CHECK_EQ(std::string(base->value()), std::string("dxt1"));
        }
    }

    // The text buffer contains the auto-generated VMT content
    UT_CHECK(session.text().find("\"UnlitGeneric\"") != std::string::npos);
    UT_CHECK(session.text().find("\"$basetexture\"") != std::string::npos);
    UT_CHECK(session.text().find("\"dxt1\"") != std::string::npos);
}

UT_TEST_CASE("ui session: opening a VTF in materials directory generates correct relative texture path") {
    const fs::path temp_dir = fresh_temp_dir() / "vtf_test";
    const fs::path mats_dir = temp_dir / "materials" / "models" / "props";
    fs::create_directories(mats_dir);

    const fs::path src_vtf = fs::path(VMT_TEST_DATA_DIR) / "vtf" / "dxt1.vtf";
    const fs::path dst_vtf = mats_dir / "crate01.vtf";
    std::error_code ec;
    fs::copy_file(src_vtf, dst_vtf, fs::copy_options::overwrite_existing, ec);
    UT_CHECK(!ec);

    ui::EditorSession session;
    const vmt::Result<void> result = session.open_file(dst_vtf);
    UT_CHECK(result.ok());
    UT_CHECK(session.is_vtf());
    UT_CHECK_EQ(session.display_name(), std::string("crate01.vtf"));
    UT_CHECK_EQ(session.material_root(), temp_dir);

    const vmt::Document* doc = session.document();
    UT_CHECK(doc != nullptr);
    if (doc != nullptr) {
        UT_CHECK_EQ(std::string(doc->shader()), std::string("UnlitGeneric"));
        const vmt::Node* base = doc->find("$basetexture");
        UT_CHECK(base != nullptr);
        if (base != nullptr) {
            UT_CHECK_EQ(std::string(base->value()), std::string("models/props/crate01"));
        }
    }
}

UT_TEST_CASE("ui session: saving VTF directly is rejected, Save As to VMT succeeds and clears is_vtf") {
    const fs::path vtf_file = fs::path(VMT_TEST_DATA_DIR) / "vtf" / "dxt1.vtf";
    ui::EditorSession session;
    UT_CHECK(session.open_file(vtf_file).ok());
    UT_CHECK(session.is_vtf());

    // Saving directly over a VTF is rejected to protect the binary texture
    const vmt::Result<void> save_res = session.save();
    UT_CHECK(!save_res.ok());
    UT_CHECK_EQ(save_res.error().code, vmt::ErrorCode::InvalidArgument);

    // Saving as .vtf is also rejected
    const fs::path bad_vtf_dest = fresh_temp_dir() / "cannot_save.vtf";
    const vmt::Result<void> bad_res = session.save_as(bad_vtf_dest);
    UT_CHECK(!bad_res.ok());
    UT_CHECK_EQ(bad_res.error().code, vmt::ErrorCode::InvalidArgument);

    // Save As with a .vmt file succeeds
    const fs::path vmt_dest = fresh_temp_dir() / "exported.vmt";
    const vmt::Result<void> good_res = session.save_as(vmt_dest);
    UT_CHECK(good_res.ok());
    UT_CHECK(!session.is_vtf());
    UT_CHECK(session.vtf_info() == nullptr);
    UT_CHECK(!session.dirty());
    UT_CHECK_EQ(session.display_name(), std::string("exported.vmt"));
    UT_CHECK(fs::exists(vmt_dest));
    UT_CHECK_EQ(read_all(vmt_dest), session.text());

    // Subsequent normal save works since it's now a VMT file
    UT_CHECK(session.set_parameter("$basetexture", "new/tex").ok());
    UT_CHECK(session.dirty());
    UT_CHECK(session.save().ok());
    UT_CHECK(!session.dirty());
}

UT_TEST_CASE("ui session: editing auto-generated VMT supports undo, redo and revert") {
    const fs::path vtf_file = fs::path(VMT_TEST_DATA_DIR) / "vtf" / "dxt1.vtf";
    ui::EditorSession session;
    UT_CHECK(session.open_file(vtf_file).ok());

    const std::string initial_text = session.text();
    UT_CHECK(!session.can_undo());

    // Edit parameter
    UT_CHECK(session.set_parameter("$basetexture", "custom/crate").ok());
    UT_CHECK(session.dirty());
    UT_CHECK(session.can_undo());

    // Switch shader
    UT_CHECK(session.set_shader("VertexLitGeneric").ok());
    UT_CHECK_EQ(std::string(session.document()->shader()), std::string("VertexLitGeneric"));

    // Undo shader change
    UT_CHECK(session.undo().ok());
    UT_CHECK_EQ(std::string(session.document()->shader()), std::string("UnlitGeneric"));

    // Undo parameter change -> back to initial generated state
    UT_CHECK(session.undo().ok());
    UT_CHECK_EQ(session.text(), initial_text);
    UT_CHECK(!session.dirty());

    // Redo restores
    UT_CHECK(session.redo().ok());
    UT_CHECK(session.dirty());

    // Revert restores the initial opened revision
    UT_CHECK(session.can_revert());
    UT_CHECK(session.revert().ok());
    UT_CHECK_EQ(session.text(), initial_text);
    UT_CHECK(!session.dirty());
}

UT_TEST_CASE("ui session: opening corrupt or non-VTF files fails cleanly with proper error") {
    const fs::path temp_dir = fresh_temp_dir() / "corrupt_vtf";
    fs::create_directories(temp_dir);

    // 1. File with .vtf extension but bad signature
    const fs::path fake_vtf = temp_dir / "fake.vtf";
    {
        std::ofstream out(fake_vtf, std::ios::binary);
        out << "NOT_A_VTF_FILE_HEADER_DATA_1234567890";
    }
    ui::EditorSession session;
    const vmt::Result<void> res1 = session.open_file(fake_vtf);
    UT_CHECK(!res1.ok());
    UT_CHECK_EQ(res1.error().code, vmt::ErrorCode::VtfInvalidHeader);

    // 2. Truncated .vtf file (< 4 bytes)
    const fs::path truncated_vtf = temp_dir / "truncated.vtf";
    {
        std::ofstream out(truncated_vtf, std::ios::binary);
        out << "VT";
    }
    const vmt::Result<void> res2 = session.open_file(truncated_vtf);
    UT_CHECK(!res2.ok());
    UT_CHECK_EQ(res2.error().code, vmt::ErrorCode::VtfTruncated);
}

UT_TEST_CASE("ui texture: thumbnail decodes gracefully when texture has a typo on disk") {
    const fs::path root = fresh_temp_dir() / "thumb_typo";
    std::error_code ec;
    fs::create_directories(root / "materials" / "wallpaper", ec);

    // Copy valid VTF to b_wallpaper1.vtf
    const fs::path src_vtf = data_path("vtf/dxt1.vtf");
    const fs::path dst_vtf = root / "materials" / "wallpaper" / "b_wallpaper1.vtf";
    fs::copy_file(src_vtf, dst_vtf, fs::copy_options::overwrite_existing, ec);
    UT_CHECK(!ec);

    // TextureSource that implements graceful typo resolution using resolve_texture_file_gracefully
    class TestGracefulSource final : public ui::TextureSource {
    public:
        std::vector<fs::path> roots;
        vmt::Result<std::vector<unsigned char>> read(const std::string& game_path) override {
            const fs::path resolved = ui::resolve_texture_file_gracefully(game_path, roots);
            if (resolved.empty()) {
                return vmt::Err<std::vector<unsigned char>>(
                    vmt::make_error(vmt::ErrorCode::NotFound, "not found"));
            }
            std::ifstream stream(resolved, std::ios::binary);
            if (!stream) {
                return vmt::Err<std::vector<unsigned char>>(
                    vmt::make_error(vmt::ErrorCode::PermissionDenied, "cannot open"));
            }
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)),
                                             std::istreambuf_iterator<char>());
            return vmt::Ok(std::move(bytes));
        }
    } source;
    source.roots = { root };

    // VMT referencing "wallpaper/b_wallaper1" (missing 'p' typo)
    const std::string vmt_text =
        "\"LightmappedGeneric\"\n"
        "{\n"
        "    \"$basetexture\" \"wallpaper/b_wallaper1\"\n"
        "}\n";

    const vmt::Result<ui::TextureThumbnail> thumb =
        ui::make_texture_thumbnail(vmt_text, source, 64);
    UT_CHECK(thumb.ok());
    if (thumb.ok()) {
        UT_CHECK(thumb.value().width > 0);
        UT_CHECK(thumb.value().height > 0);
        UT_CHECK(!thumb.value().bgra.empty());
    }
}

UT_TEST_CASE("ui version: semantic version parsing and comparison") {
    // Current compiled version
    const ui::AppVersion current = ui::AppVersion::current();
    UT_CHECK(current.is_valid());

    // Basic parsing
    const ui::AppVersion v1 = ui::AppVersion::parse("0.1.0");
    UT_CHECK_EQ(v1.major, 0);
    UT_CHECK_EQ(v1.minor, 1);
    UT_CHECK_EQ(v1.patch, 0);
    UT_CHECK_EQ(v1.build, 0);
    UT_CHECK(v1.prerelease.empty());

    // Leading 'v' and whitespace
    const ui::AppVersion v1_tagged = ui::AppVersion::parse("  v0.1.0  ");
    UT_CHECK_EQ(v1, v1_tagged);

    // Prerelease suffix
    const ui::AppVersion v_beta = ui::AppVersion::parse("v0.2.0-beta.1");
    UT_CHECK_EQ(v_beta.major, 0);
    UT_CHECK_EQ(v_beta.minor, 2);
    UT_CHECK_EQ(v_beta.patch, 0);
    UT_CHECK_EQ(v_beta.prerelease, std::string("beta.1"));

    // Comparisons
    UT_CHECK(ui::AppVersion::parse("v0.1.1") > ui::AppVersion::parse("0.1.0"));
    UT_CHECK(ui::AppVersion::parse("v0.2.0") > ui::AppVersion::parse("v0.1.9"));
    UT_CHECK(ui::AppVersion::parse("v1.0.0") > ui::AppVersion::parse("0.9.99"));
    UT_CHECK(ui::AppVersion::parse("0.1.0") < ui::AppVersion::parse("0.1.1"));
    UT_CHECK(ui::AppVersion::parse("0.2.0") == ui::AppVersion::parse("v0.2.0"));
    UT_CHECK(ui::AppVersion::parse("0.2.0") != ui::AppVersion::parse("0.2.1"));

    // SemVer: release without prerelease is newer than one with prerelease
    UT_CHECK(ui::AppVersion::parse("0.2.0") > ui::AppVersion::parse("0.2.0-rc1"));
    UT_CHECK(ui::AppVersion::parse("0.2.0-rc2") > ui::AppVersion::parse("0.2.0-rc1"));

    // SemVer 2.0.0: numeric prerelease identifiers compared numerically ("rc.10" > "rc.2")
    UT_CHECK(ui::AppVersion::parse("0.2.0-rc.10") > ui::AppVersion::parse("0.2.0-rc.2"));
    UT_CHECK(ui::AppVersion::parse("0.2.0-alpha") < ui::AppVersion::parse("0.2.0-alpha.1"));
    UT_CHECK(ui::AppVersion::parse("0.2.0-alpha.1") < ui::AppVersion::parse("0.2.0-alpha.beta"));
    UT_CHECK(ui::AppVersion::parse("0.2.0-alpha.beta") < ui::AppVersion::parse("0.2.0-beta"));

    // SemVer 2.0.0: build metadata (+...) is stripped and ignored for precedence
    const ui::AppVersion v_build = ui::AppVersion::parse("1.0.0+20261015");
    UT_CHECK(v_build.prerelease.empty());
    UT_CHECK(v_build == ui::AppVersion::parse("1.0.0"));
    UT_CHECK(!(v_build < ui::AppVersion::parse("1.0.0")));
    UT_CHECK(!(ui::AppVersion::parse("1.0.0") < v_build));

    const ui::AppVersion v_pre_build = ui::AppVersion::parse("1.0.0-rc.1+sha.abcdef");
    UT_CHECK_EQ(v_pre_build.prerelease, std::string("rc.1"));
    UT_CHECK(v_pre_build == ui::AppVersion::parse("1.0.0-rc.1"));

    // Serialization
    UT_CHECK_EQ(v1.to_string(), std::string("0.1.0"));
    UT_CHECK_EQ(v_beta.to_string(), std::string("0.2.0-beta.1"));
    UT_CHECK_EQ(ui::AppVersion::current(), ui::AppVersion::parse("0.2.0"));
}

UT_TEST_CASE("ui settings: updater options roundtrip") {
    const fs::path dir = fresh_temp_dir();
    const fs::path file = dir / "settings_updater.ini";

    ui::UISettings settings;
    settings.ignored_update_version = "v0.3.5";
    settings.check_for_updates = false;

    UT_CHECK(settings.save(file));

    const ui::UISettings loaded = ui::UISettings::load(file);
    UT_CHECK_EQ(loaded.ignored_update_version, std::string("v0.3.5"));
    UT_CHECK(!loaded.check_for_updates);
}

// --- asset index and its on-disk cache (asset browser core) -------------------

UT_TEST_CASE("ui assets: extension classification and labels") {
    using ui::AssetKind;
    UT_CHECK(ui::asset_kind_from_path("materials/brick/brick001.vmt") == AssetKind::Material);
    UT_CHECK(ui::asset_kind_from_path("materials/brick/brick001.VMT") == AssetKind::Material);
    UT_CHECK(ui::asset_kind_from_path("materials/brick/brick001.vtf") == AssetKind::Texture);
    UT_CHECK(ui::asset_kind_from_path("models/props_c17/oildrum001.mdl") == AssetKind::Model);
    UT_CHECK(ui::asset_kind_from_path("models/props_c17/oildrum001.MDL") == AssetKind::Model);
    UT_CHECK(ui::asset_kind_from_path("sound/ambience/wind.wav") == AssetKind::Other);
    UT_CHECK(ui::asset_kind_from_path("materials/no_extension") == AssetKind::Other);
    UT_CHECK(ui::asset_kind_from_path("materials/brick/brick001.vmt.bak") == AssetKind::Other);
    UT_CHECK(ui::asset_kind_from_path("") == AssetKind::Other);

    UT_CHECK_EQ(ui::asset_type_label(AssetKind::Material), std::string("Material"));
    UT_CHECK_EQ(ui::asset_type_label(AssetKind::Texture), std::string("Texture"));
    UT_CHECK_EQ(ui::asset_type_label(AssetKind::Model), std::string("Model"));
    UT_CHECK_EQ(ui::asset_type_label(AssetKind::Other), std::string("Other"));
}

UT_TEST_CASE("ui assets: scanned and folder sources dedupe and merge") {
    const fs::path root = fresh_temp_dir() / "asset_merge";
    std::error_code ec;
    fs::create_directories(root / "materials" / "brick", ec);
    fs::create_directories(root / "materials" / "wood", ec);
    UT_CHECK(!ec);
    {
        std::ofstream(root / "materials" / "brick" / "brick001.vmt") << "\"UnlitGeneric\"\n{\n}\n";
        std::ofstream(root / "materials" / "wood" / "plank.vmt") << "\"UnlitGeneric\"\n{\n}\n";
    }

    ui::AssetIndex index;
    index.add_scanned_assets({ { "materials/brick/brick001.vmt", false },
                               { "materials/wood/plank.vmt", true } },
                             ui::SourcePacked);
    UT_CHECK_EQ(index.size(), std::size_t{ 2 });

    // Both files already exist as engine rows, so the folder scan only merges
    // the SourceFolder bit and fills disk_path/size.
    UT_CHECK_EQ(index.add_directory(root, ui::SourceFolder), std::size_t{ 0 });
    UT_CHECK_EQ(index.size(), std::size_t{ 2 });

    // entries() is sorted by virtual_path: brick before wood.
    const ui::AssetEntry& brick = index.entries()[0];
    UT_CHECK_EQ(brick.virtual_path, std::string("materials/brick/brick001.vmt"));
    UT_CHECK_EQ(brick.path_lower, std::string("materials/brick/brick001.vmt"));
    UT_CHECK(brick.kind == ui::AssetKind::Material);
    UT_CHECK_EQ(brick.sources, static_cast<std::uint8_t>(ui::SourcePacked | ui::SourceFolder));
    UT_CHECK(brick.size > 0);
    UT_CHECK(!brick.disk_path.empty());

    const ui::AssetEntry& plank = index.entries()[1];
    UT_CHECK_EQ(plank.virtual_path, std::string("materials/wood/plank.vmt"));
    UT_CHECK_EQ(plank.sources, static_cast<std::uint8_t>(ui::SourceLoose | ui::SourceFolder));
    UT_CHECK(!plank.disk_path.empty());

    // Dedup is case-insensitive and keeps the first spelling for display.
    ui::AssetIndex spelling;
    spelling.add_scanned_assets({ { "Materials/Brick/Brick001.VMT", false },
                                  { "materials/brick/brick001.vmt", false } },
                                ui::SourcePacked);
    UT_CHECK_EQ(spelling.size(), std::size_t{ 1 });
    UT_CHECK_EQ(spelling.entries()[0].virtual_path, std::string("Materials/Brick/Brick001.VMT"));
    UT_CHECK_EQ(spelling.entries()[0].path_lower, std::string("materials/brick/brick001.vmt"));

    // Separator style, duplicate separators and "." components collapse into
    // the same canonical path.
    ui::AssetIndex normalized;
    normalized.add_scanned_assets({ { "materials\\brick//./brick001.vmt", false } },
                                  ui::SourcePacked);
    UT_CHECK_EQ(normalized.size(), std::size_t{ 1 });
    UT_CHECK_EQ(normalized.entries()[0].virtual_path,
                std::string("materials/brick/brick001.vmt"));
}

UT_TEST_CASE("ui assets: directory scan caps entries and reports truncation") {
    const fs::path root = fresh_temp_dir() / "asset_cap";
    std::error_code ec;
    fs::create_directories(root / "materials", ec);
    UT_CHECK(!ec);
    std::ofstream(root / "materials" / "a.vmt") << "x";
    std::ofstream(root / "materials" / "b.vmt") << "x";
    std::ofstream(root / "materials" / "c.vmt") << "x";
    std::ofstream(root / "materials" / "ignored.txt") << "x";

    ui::AssetIndex index;
    UT_CHECK_EQ(index.add_directory(root, ui::SourceFolder, 1), std::size_t{ 1 });
    UT_CHECK_EQ(index.size(), std::size_t{ 1 });
    UT_CHECK(index.truncated());

    index.clear();
    UT_CHECK(index.empty());
    UT_CHECK(!index.truncated());
}

UT_TEST_CASE("ui assets: filters combine kind, source, folder and AND tokens") {
    ui::AssetIndex index;
    index.add_scanned_assets({ { "materials/brick/brick001.vmt", false },
                               { "materials/brick/brick001.vtf", false },
                               { "materials/wood/plank.vmt", true },
                               { "models/props_c17/oildrum001.mdl", false } },
                             ui::SourcePacked);
    UT_CHECK_EQ(index.size(), std::size_t{ 4 });

    // An empty filter is the "All" chip: every entry, in path order.
    const std::vector<std::size_t> all = index.filter(ui::AssetFilter{});
    UT_CHECK_EQ(all.size(), std::size_t{ 4 });
    for (std::size_t i = 0; i < all.size(); ++i) {
        UT_CHECK_EQ(all[i], i);
    }

    ui::AssetFilter materials;
    materials.kinds = 1u << static_cast<std::uint32_t>(ui::AssetKind::Material);
    UT_CHECK_EQ(index.filter(materials).size(), std::size_t{ 2 });

    ui::AssetFilter models;
    models.kinds = 1u << static_cast<std::uint32_t>(ui::AssetKind::Model);
    const std::vector<std::size_t> model_hits = index.filter(models);
    UT_CHECK_EQ(model_hits.size(), std::size_t{ 1 });
    if (!model_hits.empty()) {
        UT_CHECK_EQ(index.entries()[model_hits[0]].virtual_path,
                    std::string("models/props_c17/oildrum001.mdl"));
    }

    ui::AssetFilter loose;
    loose.sources = ui::SourceLoose;
    const std::vector<std::size_t> loose_hits = index.filter(loose);
    UT_CHECK_EQ(loose_hits.size(), std::size_t{ 1 });
    if (!loose_hits.empty()) {
        UT_CHECK_EQ(index.entries()[loose_hits[0]].virtual_path,
                    std::string("materials/wood/plank.vmt"));
    }

    ui::AssetFilter folder;
    folder.folder_prefix = "MATERIALS/BRICK";
    UT_CHECK_EQ(index.filter(folder).size(), std::size_t{ 2 });

    ui::AssetFilter folder_backslash;
    folder_backslash.folder_prefix = "materials\\brick";
    UT_CHECK_EQ(index.filter(folder_backslash).size(), std::size_t{ 2 });

    ui::AssetFilter one_token;
    one_token.tokens = { "brick" };
    UT_CHECK_EQ(index.filter(one_token).size(), std::size_t{ 2 });

    ui::AssetFilter two_tokens;
    two_tokens.tokens = { "BRICK", "vtf" };
    const std::vector<std::size_t> token_hits = index.filter(two_tokens);
    UT_CHECK_EQ(token_hits.size(), std::size_t{ 1 });
    if (!token_hits.empty()) {
        UT_CHECK_EQ(index.entries()[token_hits[0]].virtual_path,
                    std::string("materials/brick/brick001.vtf"));
    }

    ui::AssetFilter no_token;
    no_token.tokens = { "brick", "nope" };
    UT_CHECK_EQ(index.filter(no_token).size(), std::size_t{ 0 });

    // All four stages at once: Material + Packed + materials/ + brick + vmt.
    ui::AssetFilter combined;
    combined.kinds = 1u << static_cast<std::uint32_t>(ui::AssetKind::Material);
    combined.sources = ui::SourcePacked;
    combined.folder_prefix = "materials/";
    combined.tokens = { "brick", "vmt" };
    const std::vector<std::size_t> combined_hits = index.filter(combined);
    UT_CHECK_EQ(combined_hits.size(), std::size_t{ 1 });
    if (!combined_hits.empty()) {
        UT_CHECK_EQ(index.entries()[combined_hits[0]].virtual_path,
                    std::string("materials/brick/brick001.vmt"));
    }
}

UT_TEST_CASE("ui assets: cache round-trips and rejects another game or version") {
    const fs::path root = fresh_temp_dir() / "asset_cache_scan";
    std::error_code ec;
    fs::create_directories(root / "materials" / "brick", ec);
    UT_CHECK(!ec);
    std::ofstream(root / "materials" / "brick" / "brick001.vmt") << "\"UnlitGeneric\"\n{\n}\n";

    ui::AssetIndex index;
    index.add_scanned_assets({ { "materials/brick/brick001.vmt", false },
                               { "materials/wood/plank.vmt", true },
                               { "models/props_c17/oildrum001.mdl", false } },
                             ui::SourcePacked);
    index.add_directory(root, ui::SourceFolder);

    ui::AssetCacheMeta meta;
    meta.game_dir = "C:\\Games\\GarrysMod\\garrysmod";
    meta.generated = 1761000000;
    meta.roots = { "materials", "models" };
    meta.extensions = { ".vmt", ".vtf", ".mdl" };

    // The parent directory does not exist yet: save must create it.
    const fs::path file = fresh_temp_dir() / "cache" / "assets-v1.tsv";
    UT_CHECK(ui::save_asset_cache(file, index, meta));
    UT_CHECK(fs::exists(file));
    fs::path temp_file = file;
    temp_file += ".tmp";
    UT_CHECK(!fs::exists(temp_file));

    // Case, slash style and a trailing separator do not invalidate the cache.
    const ui::AssetCacheLoad loaded =
        ui::load_asset_cache(file, "c:/games/garrysmod/garrysmod/");
    UT_CHECK(loaded.loaded);
    UT_CHECK_EQ(loaded.index.size(), index.size());
    UT_CHECK_EQ(loaded.meta.game_dir, meta.game_dir);
    UT_CHECK_EQ(loaded.meta.generated, meta.generated);
    UT_CHECK(loaded.meta.roots == meta.roots);
    UT_CHECK(loaded.meta.extensions == meta.extensions);
    for (std::size_t i = 0; i < loaded.index.size() && i < index.size(); ++i) {
        UT_CHECK_EQ(loaded.index.entries()[i].virtual_path,
                    index.entries()[i].virtual_path);
        UT_CHECK(loaded.index.entries()[i].kind == index.entries()[i].kind);
        UT_CHECK_EQ(loaded.index.entries()[i].sources, index.entries()[i].sources);
        // The cache stores paths and flags only.
        UT_CHECK(loaded.index.entries()[i].disk_path.empty());
        UT_CHECK_EQ(loaded.index.entries()[i].size, std::uint64_t{ 0 });
    }

    const ui::AssetCacheLoad other_game =
        ui::load_asset_cache(file, "C:\\Games\\TF2\\tf");
    UT_CHECK(!other_game.loaded);
    UT_CHECK(other_game.index.empty());

    const fs::path wrong_version = fresh_temp_dir() / "assets-v2.tsv";
    {
        std::ofstream stream(wrong_version, std::ios::binary);
        stream << "vmtstudio-assets\t2\n";
        stream << "game_dir\tC:\\Games\\GarrysMod\\garrysmod\n";
        stream << "count\t0\n";
    }
    const ui::AssetCacheLoad rejected =
        ui::load_asset_cache(wrong_version, "C:\\Games\\GarrysMod\\garrysmod");
    UT_CHECK(!rejected.loaded);
    UT_CHECK(rejected.index.empty());

    const ui::AssetCacheLoad missing =
        ui::load_asset_cache(fresh_temp_dir() / "does-not-exist.tsv",
                             "C:\\Games\\GarrysMod\\garrysmod");
    UT_CHECK(!missing.loaded);
    UT_CHECK(missing.index.empty());
}

UT_TEST_CASE("ui assets: cache serializes to and parses from memory") {
    ui::AssetIndex index;
    index.add_scanned_assets({ { "materials/brick/brick001.vmt", false },
                               { "models/player/heavy.mdl", true } },
                             ui::SourcePacked);

    ui::AssetCacheMeta meta;
    meta.game_dir = "C:\\Games\\TF2\\tf";
    meta.generated = 1761000123;
    meta.roots = { "materials", "models" };
    meta.extensions = { ".vmt", ".vtf", ".mdl" };

    const std::string text = ui::serialize_asset_cache(index, meta);
    UT_CHECK(text.rfind("vmtstudio-assets\t1\n", 0) == 0);
    UT_CHECK(text.find("game_dir\tC:\\Games\\TF2\\tf\n") != std::string::npos);
    UT_CHECK(text.find("asset\td\tmodels/player/heavy.mdl\n") != std::string::npos);

    const ui::AssetCacheLoad parsed =
        ui::parse_asset_cache(text, "c:/games/tf2/tf/");
    UT_CHECK(parsed.loaded);
    UT_CHECK_EQ(parsed.index.size(), index.size());
    UT_CHECK_EQ(parsed.meta.generated, meta.generated);
    UT_CHECK(parsed.meta.roots == meta.roots);
    UT_CHECK(parsed.meta.extensions == meta.extensions);

    UT_CHECK(!ui::parse_asset_cache(text, "C:\\Games\\HL2\\hl2").loaded);
    UT_CHECK(!ui::parse_asset_cache("not a cache", meta.game_dir).loaded);
    UT_CHECK(!ui::parse_asset_cache("", meta.game_dir).loaded);
}

namespace {

std::vector<unsigned char> rle_compress(const std::vector<unsigned char>& input) {
    std::vector<unsigned char> out;
    for (std::size_t i = 0; i < input.size();) {
        const unsigned char value = input[i];
        std::size_t run = 1;
        while (i + run < input.size() && input[i + run] == value && run < 255) {
            ++run;
        }
        out.push_back(static_cast<unsigned char>(run));
        out.push_back(value);
        i += run;
    }
    return out;
}

bool rle_decompress(const std::vector<unsigned char>& input,
                    std::vector<unsigned char>& out) {
    if ((input.size() % 2) != 0) {
        return false;
    }
    out.clear();
    for (std::size_t i = 0; i < input.size(); i += 2) {
        out.insert(out.end(), input[i], input[i + 1]);
    }
    return true;
}

} // namespace

UT_TEST_CASE("ui assets: pack round-trips, updates, compacts and trims a partial tail") {
    const fs::path file = fresh_temp_dir() / "cache.pack";
    const std::vector<unsigned char> index_bytes = { 'v', 'm', 't', 's', 't', 'u', 'd', 'i', 'o' };
    const std::vector<unsigned char> thumb_bytes = { 0x89, 'P', 'N', 'G', 1, 2, 3 };

    std::uint64_t good_size = 0;
    {
        ui::AssetPack pack;
        UT_CHECK(pack.open(file, true));
        UT_CHECK(pack.is_open());
        UT_CHECK(pack.live_count() == 0);
        UT_CHECK(pack.put(ui::kAssetPackTagMeta, "index", index_bytes, true));
        UT_CHECK(pack.put(ui::kAssetPackTagThumb, "abc", thumb_bytes, false));
        UT_CHECK(pack.live_count() == 2);

        std::vector<unsigned char> read;
        UT_CHECK(pack.get(ui::kAssetPackTagMeta, "index", read));
        UT_CHECK(read == index_bytes);
        UT_CHECK(pack.get(ui::kAssetPackTagThumb, "abc", read));
        UT_CHECK(read == thumb_bytes);

        // A later record with the same key wins; compact drops the old row.
        const std::vector<unsigned char> updated = { 9, 9, 9, 9 };
        UT_CHECK(pack.put(ui::kAssetPackTagThumb, "abc", updated, false));
        UT_CHECK(pack.get(ui::kAssetPackTagThumb, "abc", read));
        UT_CHECK(read == updated);
        const std::uint64_t before_compact = pack.file_size();
        UT_CHECK(pack.compact());
        UT_CHECK(pack.file_size() < before_compact);
        UT_CHECK(pack.get(ui::kAssetPackTagThumb, "abc", read));
        UT_CHECK(read == updated);

        // Remove a whole namespace without touching the index.
        UT_CHECK(pack.remove_tag(ui::kAssetPackTagThumb));
        UT_CHECK(!pack.contains(ui::kAssetPackTagThumb, "abc"));
        UT_CHECK(pack.contains(ui::kAssetPackTagMeta, "index"));
        UT_CHECK(pack.live_count() == 1);
        good_size = pack.file_size();
    }

    // Simulate a crash halfway through an append.
    {
        std::ofstream append(file, std::ios::binary | std::ios::app);
        append.write("\x01\x02\x03", 3);
    }
    {
        ui::AssetPack reopened;
        UT_CHECK(reopened.open(file, false));
        UT_CHECK_EQ(reopened.file_size(), good_size);
        std::vector<unsigned char> read;
        UT_CHECK(reopened.get(ui::kAssetPackTagMeta, "index", read));
        UT_CHECK(read == index_bytes);
    }
}

UT_TEST_CASE("ui assets: pack uses the injected compressor and rejects bad checksums") {
    const fs::path file = fresh_temp_dir() / "compressed.pack";
    ui::AssetPack pack(rle_compress, rle_decompress);
    UT_CHECK(pack.open(file, true));

    const std::vector<unsigned char> zeros(4000, 0);
    UT_CHECK(pack.put(ui::kAssetPackTagMeta, "index", zeros, true));
    UT_CHECK(pack.last_put_compressed());
    UT_CHECK(pack.file_size() < zeros.size());
    std::vector<unsigned char> read;
    UT_CHECK(pack.get(ui::kAssetPackTagMeta, "index", read));
    UT_CHECK(read == zeros);

    // A payload that does not shrink is stored raw instead of growing the file.
    const std::vector<unsigned char> noise = { 1, 2, 3, 4, 5, 6, 7, 8 };
    UT_CHECK(pack.put(ui::kAssetPackTagThumb, "noise", noise, true));
    UT_CHECK(!pack.last_put_compressed());
    UT_CHECK(pack.get(ui::kAssetPackTagThumb, "noise", read));
    UT_CHECK(read == noise);

    // Flip one byte of the raw payload and check that the FNV checksum notices.
    const fs::path raw_file = fresh_temp_dir() / "checksum.pack";
    const std::vector<unsigned char> checksum_bytes = { 4, 5, 6, 7, 8 };
    {
        ui::AssetPack raw;
        UT_CHECK(raw.open(raw_file, true));
        UT_CHECK(raw.put(ui::kAssetPackTagMeta, "index", checksum_bytes, false));
    }
    {
        std::fstream stream(raw_file, std::ios::binary | std::ios::in | std::ios::out);
        const std::streamoff payload_offset =
            static_cast<std::streamoff>(16 + 20 + std::string("meta\x1findex").size());
        stream.seekg(payload_offset);
        char byte = 0;
        stream.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5A);
        stream.seekp(payload_offset);
        stream.write(&byte, 1);
    }
    {
        ui::AssetPack raw;
        UT_CHECK(raw.open(raw_file, true));
        std::vector<unsigned char> corrupted;
        UT_CHECK(!raw.get(ui::kAssetPackTagMeta, "index", corrupted));
        UT_CHECK(corrupted.empty());
    }
}

UT_TEST_CASE("ui assets: pack keys and loose signatures are stable") {
    const std::string key = ui::asset_pack_key("materials/brick/brick001.vmt", 123456);
    UT_CHECK_EQ(key.size(), std::size_t{ 16 });
    for (const char c : key) {
        UT_CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    UT_CHECK_EQ(ui::asset_pack_key("materials/brick/brick001.vmt", 123456), key);
    UT_CHECK(ui::asset_pack_key("materials/brick/brick001.vmt", 123457) != key);

    UT_CHECK_EQ(ui::asset_signature(fs::path(), 777), std::uint64_t{ 777 });

    const fs::path file = fresh_temp_dir() / "loose.bin";
    std::ofstream(file, std::ios::binary) << "one";
    const std::uint64_t first = ui::asset_signature(file, 0);
    std::ofstream(file, std::ios::binary) << "two-two";
    const std::uint64_t second = ui::asset_signature(file, 0);
    UT_CHECK(first != 0);
    UT_CHECK(second != 0);
    UT_CHECK(first != second);
}


UT_TEST_CASE("ui assets: filter over 100k entries reports its runtime") {
    constexpr std::size_t kCount = 100000;
    std::vector<ui::ScannedAsset> scans;
    scans.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        const char* extension = ".mdl";
        if (i % 3 == 0) {
            extension = ".vtf";
        } else if (i % 3 == 1) {
            extension = ".vmt";
        }
        scans.push_back({ "materials/group" + std::to_string(i % 500) + "/asset" +
                              std::to_string(i) + extension,
                          false });
    }

    ui::AssetIndex index;
    index.add_scanned_assets(scans, ui::SourcePacked);
    UT_CHECK_EQ(index.size(), kCount);

    ui::AssetFilter filter;
    filter.kinds = (1u << static_cast<std::uint32_t>(ui::AssetKind::Material)) |
                   (1u << static_cast<std::uint32_t>(ui::AssetKind::Texture));
    filter.sources = ui::SourcePacked;
    filter.folder_prefix = "materials/group1";
    filter.tokens = { "asset", "1" };

    const auto start = std::chrono::steady_clock::now();
    const std::vector<std::size_t> hits = index.filter(filter);
    const auto finish = std::chrono::steady_clock::now();
    const double milliseconds =
        std::chrono::duration<double, std::milli>(finish - start).count();
    std::printf("  [perf] AssetIndex::filter over %zu assets: %.2f ms (%zu matches)\n",
                index.size(), milliseconds, hits.size());
    // The 30 ms budget from the plan is a Release-machine target, not a CI
    // invariant: the test reports the measurement and only checks correctness.
    UT_CHECK(!hits.empty());
}