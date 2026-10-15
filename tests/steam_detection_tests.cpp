#include "ut.h"
#include "pe_stub.h"

#include "profiles/game_manager.h"
#include "profiles/steam_library.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
        ("vmtviewer_steam_tests_" + std::to_string(GetCurrentProcessId()) +
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

// Doubles backslashes so a native Windows path can be embedded in a
// double-quoted libraryfolders.vdf value.
std::string vdf_escape(std::string text) {
    std::string out;
    for (const char ch : text) {
        if (ch == '\\') {
            out += "\\\\";
        } else {
            out += ch;
        }
    }
    return out;
}

void write_libraryfolders(const fs::path& steam_root, const std::string& body) {
    write_text_file(steam_root / "steamapps" / "libraryfolders.vdf", body);
}

// Creates the minimal marker set of a Source Engine game: gameinfo.txt plus a
// bin folder carrying valid 32-bit engine DLL stubs.
void make_source_game(const fs::path& game_dir, const char* game_name) {
    write_text_file(game_dir / "gameinfo.txt",
                    std::string("\"GameInfo\"\n{\n\t\"game\"\t\t\"") +
                        game_name + "\"\n}\n");
    pe_stub::write_dll(game_dir / "bin" / "tier0.dll", pe_stub::kMachineI386, {"CommandLine_Tier0"});
    pe_stub::write_dll(game_dir / "bin" / "vstdlib.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(game_dir / "bin" / "filesystem_stdio.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(game_dir / "bin" / "materialsystem.dll", pe_stub::kMachineI386, {"CreateInterface"});
    pe_stub::write_dll(game_dir / "bin" / "shaderapidx9.dll", pe_stub::kMachineI386, {"CreateInterface"});
}

std::string lower_ascii_copy(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

// Saves/restores STEAMPATH so the tests never leak environment changes.
class SteampathGuard {
public:
    SteampathGuard() {
        const DWORD required =
            ::GetEnvironmentVariableW(L"STEAMPATH", nullptr, 0);
        if (required != 0) {
            std::wstring buffer(required + 1, L'\0');
            const DWORD written = ::GetEnvironmentVariableW(
                L"STEAMPATH", buffer.data(), required + 1);
            if (written != 0) {
                buffer.resize(written);
                previous_ = std::move(buffer);
            }
        }
    }

    ~SteampathGuard() {
        ::SetEnvironmentVariableW(
            L"STEAMPATH", previous_.empty() ? nullptr : previous_.c_str());
    }

    SteampathGuard(const SteampathGuard&) = delete;
    SteampathGuard& operator=(const SteampathGuard&) = delete;

private:
    std::wstring previous_;
};

} // namespace

// --- parse_library_folders -----------------------------------------------------

UT_TEST_CASE("steam: parse_library_folders reads fixture libraryfolders.vdf") {
    const fs::path root = fresh_temp_dir("vdf");
    write_libraryfolders(root,
        "\"libraryfolders\"\n"
        "{\n"
        "\t\"0\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
        "\t\t\"label\"\t\t\"Steam Library\"\n"
        "\t\t\"contentid\"\t\t\"3820472647234423110\"\n"
        "\t\t\"buildid\"\t\t\"9297184\"\n"
        "\t}\n"
        "\t\"1\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"D:\\\\Games\\\\Steam Library\"\n"
        "\t}\n"
        "\t\"2\"\t\t\"E:\\\\OldFormat\"\n"
        "}\n");

    const std::vector<fs::path> libraries =
        profiles::SteamDetector::parse_library_folders(root);

    // Three declared libraries; no fallback entry because the file parsed.
    UT_CHECK_EQ(libraries.size(), std::size_t(3));
    if (libraries.size() != 3u) return;
    UT_CHECK(libraries[0] == fs::path("C:\\Program Files (x86)\\Steam"));
    // The escape sequences resolve to single backslashes, spaces included.
    UT_CHECK(libraries[1] == fs::path("D:\\Games\\Steam Library"));
    // Legacy flat numeric entries are still honoured.
    UT_CHECK(libraries[2] == fs::path("E:\\OldFormat"));
}

UT_TEST_CASE("steam: parse_library_folders tolerates crlf and comments") {
    const fs::path root = fresh_temp_dir("crlf");
    write_libraryfolders(root,
        "\"libraryfolders\"\r\n"
        "{\r\n"
        "\t// \"path\"\t\t\"C:\\\\IgnoredComment\"\r\n"
        "\t\"0\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"D:\\\\Real Library\"\r\n"
        "\t}\n"
        "}\r\n");

    const std::vector<fs::path> libraries =
        profiles::SteamDetector::parse_library_folders(root);

    UT_CHECK_EQ(libraries.size(), std::size_t(1));
    if (libraries.size() == 1u) {
        UT_CHECK(libraries[0] == fs::path("D:\\Real Library"));
    }
}

UT_TEST_CASE("steam: parse_library_folders falls back to the steam root") {
    // Missing steamapps/libraryfolders.vdf entirely.
    const fs::path missing = fresh_temp_dir("missing");
    std::vector<fs::path> libraries =
        profiles::SteamDetector::parse_library_folders(missing);
    UT_CHECK_EQ(libraries.size(), std::size_t(1));
    if (libraries.size() == 1u) {
        UT_CHECK(libraries[0] == missing);
    }

    // Present but unparseable: no usable key/value pairs.
    const fs::path garbage = fresh_temp_dir("garbage");
    write_libraryfolders(garbage,
        "\"LibraryFolders\"\r\n{\r\n\tthis is not keyvalues\r\n}\r\n");
    libraries = profiles::SteamDetector::parse_library_folders(garbage);
    UT_CHECK_EQ(libraries.size(), std::size_t(1));
    if (libraries.size() == 1u) {
        UT_CHECK(libraries[0] == garbage);
    }
}

// --- discover_all_in -----------------------------------------------------------

UT_TEST_CASE("steam: discover_all_in unions and de-duplicates libraries") {
    const fs::path root = fresh_temp_dir("discover");
    const fs::path nested = root / "library";

    // The game inside `nested` is reachable from both libraries (root scan at
    // depth 4 descends into `library` too), so the merged result must hold it
    // exactly once.
    make_source_game(nested / "my_game", "My Test Game");
    make_source_game(root / "root_only_game", "Root Only Game");

    write_libraryfolders(root,
        "\"libraryfolders\"\n"
        "{\n"
        "\t\"0\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"" + vdf_escape(root.string()) + "\"\n"
        "\t}\n"
        "\t\"1\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"" + vdf_escape(nested.string()) + "\"\n"
        "\t}\n"
        "}\n");

    const std::vector<profiles::GameProfile> profiles =
        profiles::SteamDetector::discover_all_in(root, 4);

    UT_CHECK_EQ(profiles.size(), std::size_t(2));
    if (profiles.size() != 2u) return;

    const profiles::GameProfile* mine = nullptr;
    const profiles::GameProfile* only_root = nullptr;
    for (const profiles::GameProfile& profile : profiles) {
        if (profile.id == "my_game") mine = &profile;
        if (profile.id == "root_only_game") only_root = &profile;
    }
    UT_CHECK(mine != nullptr);
    UT_CHECK(only_root != nullptr);
    if (mine != nullptr) {
        UT_CHECK(mine->game_dir == nested / "my_game");
        UT_CHECK(mine->name == "My Test Game");
        UT_CHECK(mine->is_valid);
    }
    if (only_root != nullptr) {
        UT_CHECK(only_root->game_dir == root / "root_only_game");
        UT_CHECK(only_root->is_valid);
    }
}

UT_TEST_CASE("steam: discover_all_in de-duplicates case-insensitively") {
    const fs::path root = fresh_temp_dir("case");
    make_source_game(root / "my_game", "My Test Game");

    // The same library declared twice with different path casing: the game
    // must surface exactly once.
    write_libraryfolders(root,
        "\"libraryfolders\"\n"
        "{\n"
        "\t\"0\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"" + vdf_escape(root.string()) + "\"\n"
        "\t}\n"
        "\t\"1\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"" + vdf_escape(lower_ascii_copy(root.string())) + "\"\n"
        "\t}\n"
        "}\n");

    const std::vector<profiles::GameProfile> profiles =
        profiles::SteamDetector::discover_all_in(root, 4);

    UT_CHECK_EQ(profiles.size(), std::size_t(1));
    if (profiles.size() == 1u) {
        UT_CHECK(profiles[0].id == "my_game");
        UT_CHECK(profiles[0].is_valid);
    }
}

UT_TEST_CASE("steam: discover_all_in returns empty for a missing steam root") {
    const fs::path missing = fresh_temp_dir("gone");
    std::error_code ec;
    fs::remove_all(missing, ec);

    const std::vector<profiles::GameProfile> profiles =
        profiles::SteamDetector::discover_all_in(missing, 4);
    UT_CHECK(profiles.empty());
}

// --- find_steam_root / discover_all --------------------------------------------

UT_TEST_CASE("steam: find_steam_root honors STEAMPATH and falls back when unset") {
    const SteampathGuard guard;

    // A fixture directory wins as soon as STEAMPATH points at it.
    const fs::path fixture = fresh_temp_dir("env");
    {
        std::wstring value = fixture.wstring();
        ::SetEnvironmentVariableW(L"STEAMPATH", value.c_str());
        const std::optional<fs::path> found =
            profiles::SteamDetector::find_steam_root();
        UT_CHECK(found.has_value());
        if (found.has_value()) {
            UT_CHECK(*found == fixture);
        }
    }

    // A stale STEAMPATH is ignored: detection falls through to the registry
    // and the well-known locations. Whatever comes back must be a real
    // directory (or nothing at all on a machine without Steam).
    {
        std::wstring stale = (fixture / "does_not_exist").wstring();
        ::SetEnvironmentVariableW(L"STEAMPATH", stale.c_str());
        const std::optional<fs::path> found =
            profiles::SteamDetector::find_steam_root();
        if (found.has_value()) {
            std::error_code ec;
            UT_CHECK(fs::is_directory(*found, ec) && !ec);
            UT_CHECK(*found != fixture);
        }
    }

    // Unset STEAMPATH: same invariant - nullopt or the machine's real Steam.
    {
        ::SetEnvironmentVariableW(L"STEAMPATH", nullptr);
        const std::optional<fs::path> found =
            profiles::SteamDetector::find_steam_root();
        if (found.has_value()) {
            std::error_code ec;
            UT_CHECK(fs::is_directory(*found, ec) && !ec);
        }
    }
}

UT_TEST_CASE("steam: discover_all scans the STEAMPATH fixture end to end") {
    const SteampathGuard guard;

    const fs::path root = fresh_temp_dir("all");
    make_source_game(root / "half_life_2", "Half-Life 2");
    write_libraryfolders(root,
        "\"libraryfolders\"\n"
        "{\n"
        "\t\"0\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"" + vdf_escape(root.string()) + "\"\n"
        "\t}\n"
        "}\n");

    std::wstring value = root.wstring();
    ::SetEnvironmentVariableW(L"STEAMPATH", value.c_str());

    const std::vector<profiles::GameProfile> profiles =
        profiles::SteamDetector::discover_all();

    UT_CHECK_EQ(profiles.size(), std::size_t(1));
    if (profiles.size() == 1u) {
        UT_CHECK(profiles[0].id == "half_life_2");
        UT_CHECK(profiles[0].game_dir == root / "half_life_2");
        UT_CHECK(profiles[0].name == "Half-Life 2");
        UT_CHECK(profiles[0].is_valid);
    }
}
