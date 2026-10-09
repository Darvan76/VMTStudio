#include "ut.h"

#include "ui/connection_model.h"
#include "ui/material_browser.h"
#include "ui/panel_model.h"
#include "ui/preview_options.h"
#include "ui/preview_sync.h"
#include "ui/session.h"
#include "ui/settings.h"
#include "ui/text_encoding.h"
#include "ui/validate.h"
#include "vmt/schema.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "vmtviewer_ui_tests";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
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
    // "café ✓" as explicit UTF-8 bytes (source escapes are charset-dependent).
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
    UT_CHECK(!ui::parse_shape("dodecahedron", shape));
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

    UT_CHECK(settings.save(file));
    const ui::UISettings loaded = ui::UISettings::load(file);
    UT_CHECK_EQ(loaded.preview_shape, std::string("cube"));
    UT_CHECK_EQ(loaded.preview_environment, std::string("flat"));
    UT_CHECK_EQ(loaded.preview_rig, settings.preview_rig);
    UT_CHECK_EQ(loaded.browser_root, settings.browser_root);
    UT_CHECK(loaded.browser_scan_game);
    UT_CHECK(!loaded.thumbnail_flip);

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
