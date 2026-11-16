#include "ut.h"

#include "bridge/engine_compatibility.h"

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
#include <initializer_list>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

class DllFixture {
public:
    DllFixture() {
        static unsigned counter = 0;
        root_ = fs::temp_directory_path() /
            ("vmtstudio_compatibility_tests_" + std::to_string(GetCurrentProcessId()) +
             "_" + std::to_string(++counter));
        fs::create_directories(root_);
    }

    ~DllFixture() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    DllFixture(const DllFixture&) = delete;
    DllFixture& operator=(const DllFixture&) = delete;

    const fs::path& root() const noexcept { return root_; }

    void write(const char* name, std::initializer_list<const char*> strings) const {
        std::ofstream output(root_ / name, std::ios::out | std::ios::binary);
        output.put('\0');
        for (const char* value : strings) {
            output << value;
            output.put('\0');
        }
    }

private:
    fs::path root_;
};

} // namespace

UT_TEST_CASE("engine compatibility: recognizes the verified L4D2 combination") {
    const DllFixture fixture;
    fixture.write("filesystem_stdio.dll", {"VFileSystem018", "VFileSystem018"});
    fixture.write("materialsystem.dll", {"VMaterialSystem080"});
    fixture.write("studiorender.dll", {"VStudioRender026"});

    const auto result = bridge::inspect_engine_compatibility(fixture.root());
    UT_CHECK(result.requires_l4d2_backend);
    UT_CHECK_EQ(result.filesystem_interfaces.size(), std::size_t(1));
    if (result.filesystem_interfaces.size() == 1u) {
        UT_CHECK(result.filesystem_interfaces[0] == "VFileSystem018");
    }
    UT_CHECK_EQ(result.material_system_interfaces.size(), std::size_t(1));
    UT_CHECK_EQ(result.studio_render_interfaces.size(), std::size_t(1));
    UT_CHECK(result.detail ==
        "Left 4 Dead 2 native 3D preview is not available in this development build; "
        "a dedicated engine backend is required.");
}

UT_TEST_CASE("engine compatibility: leaves SDK GMod and future combinations unclassified") {
    const DllFixture fixture;
    fixture.write("materialsystem.dll", {"VMaterialSystem080"});
    fixture.write("studiorender.dll", {"VStudioRender026"});
    for (const char* version : {"VFileSystem017", "VFileSystem022", "VFileSystem099"}) {
        fixture.write("filesystem_stdio.dll", {version});
        const auto result = bridge::inspect_engine_compatibility(fixture.root());
        UT_CHECK(!result.requires_l4d2_backend);
        UT_CHECK(result.detail.empty());
        UT_CHECK_EQ(result.filesystem_interfaces.size(), std::size_t(1));
    }
    fixture.write("filesystem_stdio.dll", {"VFileSystem018"});
    fixture.write("materialsystem.dll", {"VMaterialSystem081"});
    UT_CHECK(!bridge::inspect_engine_compatibility(fixture.root()).requires_l4d2_backend);
}

UT_TEST_CASE("engine compatibility: rejects substrings and incomplete interface names") {
    const DllFixture fixture;
    fixture.write("filesystem_stdio.dll",
                  {"prefixVFileSystem018", "VFileSystem018suffix", "VFileSystem0180",
                   "VFileSystem18", "VFileSystem0x8"});
    fixture.write("materialsystem.dll", {"VMaterialSystem080"});
    {
        std::ofstream output(fixture.root() / "studiorender.dll", std::ios::out | std::ios::binary);
        output << "VStudioRender026"; // A NUL terminator is required within the scanned bytes.
    }
    const auto result = bridge::inspect_engine_compatibility(fixture.root());
    UT_CHECK(result.filesystem_interfaces.empty());
    UT_CHECK(result.studio_render_interfaces.empty());
    UT_CHECK(!result.requires_l4d2_backend);
}

UT_TEST_CASE("engine compatibility: requires evidence from all three DLLs") {
    const DllFixture fixture;
    UT_CHECK(!bridge::inspect_engine_compatibility({}).requires_l4d2_backend);
    const auto empty = bridge::inspect_engine_compatibility(fixture.root() / "missing");
    UT_CHECK(empty.filesystem_interfaces.empty());
    UT_CHECK(empty.material_system_interfaces.empty());
    UT_CHECK(empty.studio_render_interfaces.empty());
    UT_CHECK(!empty.requires_l4d2_backend);
    fixture.write("filesystem_stdio.dll", {"VFileSystem018"});
    fixture.write("materialsystem.dll", {"VMaterialSystem080"});
    UT_CHECK(!bridge::inspect_engine_compatibility(fixture.root()).requires_l4d2_backend);
}

UT_TEST_CASE("engine compatibility: does not complete a name beyond the scan limit") {
    const DllFixture fixture;
    constexpr std::streamoff max_scan_bytes = 32 * 1024 * 1024;
    const std::string version = "VFileSystem018";
    {
        std::ofstream output(fixture.root() / "filesystem_stdio.dll", std::ios::out | std::ios::binary);
        output.seekp(max_scan_bytes - static_cast<std::streamoff>(version.size()));
        output << version;
        output.put('\0'); // The terminator is one byte outside the bounded prefix.
        output << version;
        output.put('\0'); // Complete names beyond the prefix must also be ignored.
    }
    fixture.write("materialsystem.dll", {"VMaterialSystem080"});
    fixture.write("studiorender.dll", {"VStudioRender026"});
    const auto result = bridge::inspect_engine_compatibility(fixture.root());
    UT_CHECK(result.filesystem_interfaces.empty());
    UT_CHECK(!result.requires_l4d2_backend);
}
