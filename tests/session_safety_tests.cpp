#include "ut.h"

#include "ui/atomic_file.h"
#include "ui/preview_sync.h"
#include "ui/session.h"
#include "ui/settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

class TestDirectory {
public:
    TestDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const fs::path candidate = fs::temp_directory_path() /
                (L"vmtstudio_session_safety_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                 std::to_wstring(GetTickCount64()) + L"_" +
                 std::to_wstring(sequence.fetch_add(1)));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
        }
        UT_CHECK(false);
    }
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;
    ~TestDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }
    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::string read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t entry_count(const fs::path& directory) {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator(directory),
                                                  fs::directory_iterator()));
}

const std::string kValidMaterial = "\"UnlitGeneric\"\n{\n\t\"$alpha\" \"1\"\n}\n";
const std::string kIncompleteMaterial = "\"UnlitGeneric\"\n{\n\t\"$alpha\" \"0.5\"\n";

} // namespace

UT_TEST_CASE("atomic file: replacement preserves unrelated temporary files and exact bytes") {
    TestDirectory directory;
    if (directory.path().empty()) return;
    const fs::path file = directory.path() / "material.vmt";
    const fs::path unrelated = directory.path() / "material.vmt.tmp";
    UT_CHECK(ui::atomic_write_file(unrelated, "unrelated work").ok());
    UT_CHECK(ui::atomic_write_file(file, "original").ok());
    const std::string binary("new\0bytes\r\n", 11);
    UT_CHECK(ui::atomic_write_file(file, binary).ok());
    UT_CHECK_EQ(read_bytes(file), binary);
    UT_CHECK_EQ(read_bytes(unrelated), std::string("unrelated work"));
    UT_CHECK_EQ(entry_count(directory.path()), std::size_t{2});
    UT_CHECK(ui::atomic_write_file(file, "").ok());
    UT_CHECK(read_bytes(file).empty());
    UT_CHECK_EQ(entry_count(directory.path()), std::size_t{2});
}

UT_TEST_CASE("atomic file: a locked destination survives replacement failure without artifacts") {
    TestDirectory directory;
    if (directory.path().empty()) return;
    const fs::path file = directory.path() / "locked.vmt";
    UT_CHECK(ui::atomic_write_file(file, "keep the original").ok());
    const HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    UT_CHECK(lock != INVALID_HANDLE_VALUE);
    if (lock == INVALID_HANDLE_VALUE) return;
    const vmt::Result<void> result = ui::atomic_write_file(file, "replacement");
    CloseHandle(lock);
    UT_CHECK(!result.ok());
    UT_CHECK_EQ(read_bytes(file), std::string("keep the original"));
    UT_CHECK_EQ(entry_count(directory.path()), std::size_t{1});
}

UT_TEST_CASE("atomic file: a directory destination is never removed") {
    TestDirectory directory;
    if (directory.path().empty()) return;
    const fs::path destination = directory.path() / "destination.vmt";
    UT_CHECK(fs::create_directory(destination));
    UT_CHECK(ui::atomic_write_file(destination / "important.txt", "keep").ok());
    UT_CHECK(!ui::atomic_write_file(destination, "replacement").ok());
    UT_CHECK_EQ(read_bytes(destination / "important.txt"), std::string("keep"));
    UT_CHECK_EQ(entry_count(directory.path()), std::size_t{1});
    const fs::path empty = directory.path() / "empty.vmt";
    UT_CHECK(fs::create_directory(empty));
    UT_CHECK(!ui::atomic_write_file(empty, "replacement").ok());
    UT_CHECK(fs::is_directory(empty));
    UT_CHECK(fs::is_empty(empty));
}

UT_TEST_CASE("session safety: recoverable syntax diagnostics forbid saving over the original") {
    TestDirectory directory;
    if (directory.path().empty()) return;
    const fs::path file = directory.path() / "material.vmt";
    UT_CHECK(ui::atomic_write_file(file, kValidMaterial).ok());
    ui::EditorSession session;
    UT_CHECK(session.open_file(file).ok());
    UT_CHECK(session.set_text(kIncompleteMaterial).ok());
    UT_CHECK(!session.text_is_valid());
    UT_CHECK(session.document() != nullptr);
    if (session.document()) UT_CHECK(!session.document()->diagnostics().empty());
    UT_CHECK(session.last_error().code != vmt::ErrorCode::Ok);
    UT_CHECK(session.last_error().line != 0);
    UT_CHECK(!session.save().ok());
    UT_CHECK(!session.save_as(directory.path() / "other.vmt").ok());
    UT_CHECK_EQ(read_bytes(file), kValidMaterial);
    UT_CHECK_EQ(session.text(), kIncompleteMaterial);
    UT_CHECK(session.dirty());
    UT_CHECK_EQ(entry_count(directory.path()), std::size_t{1});
    UT_CHECK(!session.set_parameter("$alpha", "0.2").ok());
    UT_CHECK(!session.set_shader("VertexLitGeneric").ok());
    UT_CHECK_EQ(session.text(), kIncompleteMaterial);
}

UT_TEST_CASE("session safety: invalid typing states undo redo and recover without losing text") {
    ui::EditorSession session;
    UT_CHECK(session.open_text(kValidMaterial, "material.vmt").ok());
    UT_CHECK(session.set_text(kIncompleteMaterial).ok());
    UT_CHECK(!session.text_is_valid());
    UT_CHECK(session.undo().ok());
    UT_CHECK_EQ(session.text(), kValidMaterial);
    UT_CHECK(session.text_is_valid());
    UT_CHECK(!session.dirty());
    UT_CHECK(session.redo().ok());
    UT_CHECK_EQ(session.text(), kIncompleteMaterial);
    UT_CHECK(!session.text_is_valid());
    UT_CHECK(session.set_text(kValidMaterial).ok());
    UT_CHECK(session.text_is_valid());
    UT_CHECK(session.last_error().code == vmt::ErrorCode::Ok);
    UT_CHECK(session.set_text("").ok());
    UT_CHECK(!session.text_is_valid());
    UT_CHECK(session.text().empty());
}

UT_TEST_CASE("session safety: malformed files open clean for repair with current diagnostics") {
    ui::EditorSession session;
    UT_CHECK(session.open_text(kIncompleteMaterial, "incomplete.vmt").ok());
    UT_CHECK(!session.text_is_valid());
    UT_CHECK(!session.dirty());
    UT_CHECK_EQ(session.text(), kIncompleteMaterial);
    UT_CHECK(session.document() != nullptr);
    UT_CHECK(session.set_text(kValidMaterial).ok());
    UT_CHECK(session.text_is_valid());
    UT_CHECK(session.dirty());
}

UT_TEST_CASE("session safety: failed saves retain the unsaved buffer and saved revision") {
    TestDirectory directory;
    if (directory.path().empty()) return;
    const fs::path file = directory.path() / "locked.vmt";
    UT_CHECK(ui::atomic_write_file(file, kValidMaterial).ok());
    ui::EditorSession session;
    UT_CHECK(session.open_file(file).ok());
    UT_CHECK(session.set_parameter("$alpha", "0.2").ok());
    const std::string edited = session.text();
    const HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    UT_CHECK(lock != INVALID_HANDLE_VALUE);
    if (lock == INVALID_HANDLE_VALUE) return;
    UT_CHECK(!session.save().ok());
    CloseHandle(lock);
    UT_CHECK_EQ(read_bytes(file), kValidMaterial);
    UT_CHECK_EQ(session.text(), edited);
    UT_CHECK(session.dirty());
    UT_CHECK_EQ(session.path(), file);
    UT_CHECK_EQ(entry_count(directory.path()), std::size_t{1});
    UT_CHECK(session.save().ok());
    UT_CHECK_EQ(read_bytes(file), edited);
    UT_CHECK(!session.dirty());
}

UT_TEST_CASE("preview safety: a failed replacement preserves the last material on disk") {
    TestDirectory directory;
    if (directory.path().empty()) return;
    UT_CHECK(ui::write_preview_material_at(directory.path(), "nested/test", kValidMaterial).ok());
    const fs::path file = directory.path() / "materials/nested/test.vmt";
    const HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    UT_CHECK(lock != INVALID_HANDLE_VALUE);
    if (lock == INVALID_HANDLE_VALUE) return;
    UT_CHECK(!ui::write_preview_material_at(directory.path(), "nested/test", "new text").ok());
    CloseHandle(lock);
    UT_CHECK_EQ(read_bytes(file), kValidMaterial);
    UT_CHECK_EQ(entry_count(file.parent_path()), std::size_t{1});
    UT_CHECK(!ui::write_preview_material_at(directory.path(), "/absolute", "text").ok());
    UT_CHECK(!ui::write_preview_material_at(directory.path(), "\\absolute", "text").ok());
    UT_CHECK(!ui::write_preview_material_at(directory.path(), std::string("bad\0name", 8), "text").ok());
}

UT_TEST_CASE("settings safety: replacement failure preserves the prior configuration") {
    TestDirectory directory;
    if (directory.path().empty()) return;
    const fs::path file = directory.path() / "settings.json";
    ui::UISettings settings;
    settings.last_game_dir = "original-game";
    UT_CHECK(settings.save(file));
    const std::string original = read_bytes(file);
    settings.last_game_dir = "new-game";
    const HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    UT_CHECK(lock != INVALID_HANDLE_VALUE);
    if (lock == INVALID_HANDLE_VALUE) return;
    UT_CHECK(!settings.save(file));
    CloseHandle(lock);
    UT_CHECK_EQ(read_bytes(file), original);
    UT_CHECK_EQ(entry_count(directory.path()), std::size_t{1});
}
