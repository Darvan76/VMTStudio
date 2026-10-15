#include "ut.h"
#include "pe_stub.h"

#include "profiles/game_manager.h"
#include "profiles/install_check.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path fresh_temp_dir(const char* tag) {
    static unsigned counter = 0;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) /
        ("vmtviewer_install_tests_" + std::to_string(GetCurrentProcessId()) +
         "_" + tag + "_" + std::to_string(++counter));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void write_text_file(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::out | std::ios::binary);
    stream << content;
}

// Creates the full marker set of a usable Source Engine game: gameinfo.txt
// plus a bin folder carrying valid 32-bit engine DLL stubs with the expected
// exports.
void make_valid_game(const fs::path& game_dir, const char* game_name) {
    write_text_file(game_dir / "gameinfo.txt",
                    std::string("\"GameInfo\"\n{\n\t\"game\"\t\t\"") +
                        game_name + "\"\n}\n");
    pe_stub::write_dll(game_dir / "bin" / "tier0.dll", pe_stub::kMachineI386, {"CommandLine_Tier0"});
    pe_stub::write_dll(game_dir / "bin" / "vstdlib.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(game_dir / "bin" / "filesystem_stdio.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(game_dir / "bin" / "materialsystem.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(game_dir / "bin" / "shaderapidx9.dll", pe_stub::kMachineI386, {"CreateInterface"});
}

profiles::GameProfile make_profile(const fs::path& game_dir) {
    profiles::GameProfile profile;
    profile.game_dir = game_dir;
    profile.id = game_dir.filename().string();
    return profile;
}

bool contains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

} // namespace

// --- to_string -----------------------------------------------------------------

UT_TEST_CASE("install: to_string returns a short tag per problem") {
    UT_CHECK_EQ(std::string(profiles::to_string(profiles::InstallProblem::None)), std::string(""));
    UT_CHECK_EQ(std::string(profiles::to_string(profiles::InstallProblem::EmptyFolder)),
                std::string("empty folder"));
    UT_CHECK_EQ(std::string(profiles::to_string(profiles::InstallProblem::NoGameInfo)),
                std::string("no gameinfo.txt"));
    UT_CHECK_EQ(std::string(profiles::to_string(profiles::InstallProblem::NoBin)),
                std::string("no bin folder"));
    UT_CHECK_EQ(std::string(profiles::to_string(profiles::InstallProblem::WrongArch)),
                std::string("wrong architecture"));
    UT_CHECK_EQ(std::string(profiles::to_string(profiles::InstallProblem::MissingDll)),
                std::string("missing DLLs"));
    UT_CHECK_EQ(std::string(profiles::to_string(profiles::InstallProblem::NoExports)),
                std::string("missing exports"));
}

// --- EmptyFolder ---------------------------------------------------------------

UT_TEST_CASE("install: empty and missing game folders report EmptyFolder") {
    // A folder that exists but holds no files at all (uninstalled / moved game).
    const fs::path empty = fresh_temp_dir("empty");
    const profiles::InstallReport report = profiles::check_install(make_profile(empty));
    UT_CHECK(!report.usable);
    UT_CHECK(report.problem == profiles::InstallProblem::EmptyFolder);
    UT_CHECK(!report.detail.empty());

    // A folder that does not exist on disk at all.
    const fs::path missing = fresh_temp_dir("gone") / "removed";
    const profiles::InstallReport missing_report = profiles::check_install(make_profile(missing));
    UT_CHECK(!missing_report.usable);
    UT_CHECK(missing_report.problem == profiles::InstallProblem::EmptyFolder);

    // No game folder set at all.
    profiles::GameProfile unset;
    const profiles::InstallReport unset_report = profiles::check_install(unset);
    UT_CHECK(!unset_report.usable);
    UT_CHECK(unset_report.problem == profiles::InstallProblem::EmptyFolder);
}

// --- NoGameInfo ----------------------------------------------------------------

UT_TEST_CASE("install: folder without gameinfo.txt reports NoGameInfo") {
    const fs::path root = fresh_temp_dir("nogameinfo");
    write_text_file(root / "readme.txt", "not a source game\n");

    const profiles::InstallReport report = profiles::check_install(make_profile(root));
    UT_CHECK(!report.usable);
    UT_CHECK(report.problem == profiles::InstallProblem::NoGameInfo);
    UT_CHECK(!report.detail.empty());
}

// --- NoBin ---------------------------------------------------------------------

UT_TEST_CASE("install: gameinfo without any bin folder reports NoBin") {
    const fs::path root = fresh_temp_dir("nobin");
    write_text_file(root / "gameinfo.txt", "\"GameInfo\"\n{\n\t\"game\"\t\t\"No Bin Game\"\n}\n");

    const profiles::InstallReport report = profiles::check_install(make_profile(root));
    UT_CHECK(!report.usable);
    UT_CHECK(report.problem == profiles::InstallProblem::NoBin);
    UT_CHECK(!report.detail.empty());
}

// --- MissingDll ----------------------------------------------------------------

UT_TEST_CASE("install: bin folder missing vstdlib reports MissingDll with names") {
    const fs::path root = fresh_temp_dir("missing");
    write_text_file(root / "gameinfo.txt", "\"GameInfo\"\n{\n\t\"game\"\t\t\"Broken Game\"\n}\n");
    pe_stub::write_dll(root / "bin" / "tier0.dll", pe_stub::kMachineI386, {"CommandLine_Tier0"});
    pe_stub::write_dll(root / "bin" / "filesystem_stdio.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(root / "bin" / "materialsystem.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(root / "bin" / "shaderapidx9.dll", pe_stub::kMachineI386, {"CreateInterface"});

    const profiles::InstallReport report = profiles::check_install(make_profile(root));
    UT_CHECK(!report.usable);
    UT_CHECK(report.problem == profiles::InstallProblem::MissingDll);
    UT_CHECK_EQ(report.missing_dlls.size(), std::size_t(1));
    if (report.missing_dlls.size() == 1u) {
        UT_CHECK(report.missing_dlls[0] == "vstdlib.dll");
    }
    UT_CHECK(!report.detail.empty());
}

// --- WrongArch -----------------------------------------------------------------

UT_TEST_CASE("install: 64-bit engine DLLs report WrongArch") {
    const fs::path root = fresh_temp_dir("x64");
    write_text_file(root / "gameinfo.txt", "\"GameInfo\"\n{\n\t\"game\"\t\t\"x64 Game\"\n}\n");
    pe_stub::write_dll(root / "bin" / "tier0.dll", pe_stub::kMachineAmd64, {"CommandLine_Tier0"});
    pe_stub::write_dll(root / "bin" / "vstdlib.dll", pe_stub::kMachineAmd64, {"CreateInterface"});
    pe_stub::write_dll(root / "bin" / "filesystem_stdio.dll", pe_stub::kMachineAmd64, {"CreateInterface"});
    pe_stub::write_dll(root / "bin" / "materialsystem.dll", pe_stub::kMachineAmd64, {"CreateInterface"});
    pe_stub::write_dll(root / "bin" / "shaderapidx9.dll", pe_stub::kMachineAmd64, {"CreateInterface"});

    const profiles::InstallReport report = profiles::check_install(make_profile(root));
    UT_CHECK(!report.usable);
    UT_CHECK(report.problem == profiles::InstallProblem::WrongArch);
    UT_CHECK(report.detail.find("64-bit") != std::string::npos);
}

UT_TEST_CASE("install: corrupt engine DLL reports WrongArch") {
    const fs::path root = fresh_temp_dir("corrupt");
    write_text_file(root / "gameinfo.txt", "\"GameInfo\"\n{\n\t\"game\"\t\t\"Corrupt Game\"\n}\n");
    write_text_file(root / "bin" / "tier0.dll", "this is not a PE image at all\n");
    pe_stub::write_dll(root / "bin" / "vstdlib.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(root / "bin" / "filesystem_stdio.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(root / "bin" / "materialsystem.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(root / "bin" / "shaderapidx9.dll", pe_stub::kMachineI386, {"CreateInterface"});

    const profiles::InstallReport report = profiles::check_install(make_profile(root));
    UT_CHECK(!report.usable);
    UT_CHECK(report.problem == profiles::InstallProblem::WrongArch);
    UT_CHECK(report.detail.find("tier0.dll") != std::string::npos);
}

// --- NoExports -----------------------------------------------------------------

UT_TEST_CASE("install: engine DLL without the expected export reports NoExports") {
    const fs::path root = fresh_temp_dir("exports");
    write_text_file(root / "gameinfo.txt", "\"GameInfo\"\n{\n\t\"game\"\t\t\"Foreign Engine\"\n}\n");
    // tier0.dll carries an export table, but not CommandLine_Tier0: the files
    // likely belong to an unsupported engine build. The other DLLs have no
    // export directory at all, which the best-effort check skips.
    pe_stub::write_dll(root / "bin" / "tier0.dll", pe_stub::kMachineI386, {"SomeOtherExport"});
    pe_stub::write_dll(root / "bin" / "vstdlib.dll", pe_stub::kMachineI386, {});
    pe_stub::write_dll(root / "bin" / "filesystem_stdio.dll", pe_stub::kMachineI386, {});
    pe_stub::write_dll(root / "bin" / "materialsystem.dll", pe_stub::kMachineI386, {});
    pe_stub::write_dll(root / "bin" / "shaderapidx9.dll", pe_stub::kMachineI386, {});

    const profiles::InstallReport report = profiles::check_install(make_profile(root));
    UT_CHECK(!report.usable);
    UT_CHECK(report.problem == profiles::InstallProblem::NoExports);
    UT_CHECK(report.detail.find("CommandLine_Tier0") != std::string::npos);
}

// --- usable install ------------------------------------------------------------

UT_TEST_CASE("install: valid temp install reports usable") {
    const fs::path root = fresh_temp_dir("valid");
    make_valid_game(root / "half_life_2", "Half-Life 2");

    const profiles::InstallReport report =
        profiles::check_install(make_profile(root / "half_life_2"));
    UT_CHECK(report.usable);
    UT_CHECK(report.problem == profiles::InstallProblem::None);
    UT_CHECK(report.missing_dlls.empty());
    UT_CHECK(!report.detail.empty());
}

// --- GameManager::scan wiring --------------------------------------------------

UT_TEST_CASE("install: GameManager::scan marks is_valid from the install report") {
    const fs::path root = fresh_temp_dir("scan");
    make_valid_game(root / "good_game", "Good Game");

    // Same layout but vstdlib.dll is absent: unusable, and the report must
    // say why.
    write_text_file(root / "broken_game" / "gameinfo.txt",
                    "\"GameInfo\"\n{\n\t\"game\"\t\t\"Broken Game\"\n}\n");
    pe_stub::write_dll(root / "broken_game" / "bin" / "tier0.dll", pe_stub::kMachineI386, {"CommandLine_Tier0"});
    pe_stub::write_dll(root / "broken_game" / "bin" / "filesystem_stdio.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(root / "broken_game" / "bin" / "materialsystem.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(root / "broken_game" / "bin" / "shaderapidx9.dll", pe_stub::kMachineI386, {"CreateInterface"});

    profiles::GameManager manager;
    manager.discover(root, 2);

    const profiles::GameProfile* good = manager.find("good_game");
    UT_CHECK(good != nullptr);
    if (good != nullptr) {
        UT_CHECK(good->is_valid);
        UT_CHECK(good->install.usable);
        UT_CHECK(good->install.problem == profiles::InstallProblem::None);
    }

    const profiles::GameProfile* broken = manager.find("broken_game");
    UT_CHECK(broken != nullptr);
    if (broken != nullptr) {
        UT_CHECK(!broken->is_valid);
        UT_CHECK(!broken->install.usable);
        UT_CHECK(broken->install.problem == profiles::InstallProblem::MissingDll);
        UT_CHECK(contains(broken->install.missing_dlls, "vstdlib.dll"));
        UT_CHECK(!broken->install.detail.empty());
    }
}
