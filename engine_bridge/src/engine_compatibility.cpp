#include "bridge/engine_compatibility.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <string_view>

namespace bridge {
namespace {

constexpr std::size_t kMaxDllScanBytes = 32u * 1024u * 1024u;

bool is_printable_ascii(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return byte >= 0x20u && byte <= 0x7eu;
}

std::vector<std::string> discover_interfaces(const std::filesystem::path& dll,
                                           std::string_view prefix) {
    std::ifstream input(dll, std::ios::in | std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const std::streamoff file_size = input.tellg();
    if (file_size <= 0) {
        return {};
    }
    const auto scan_size = static_cast<std::size_t>(
        std::min(file_size, static_cast<std::streamoff>(kMaxDllScanBytes)));
    std::string bytes(scan_size, '\0');
    input.seekg(0);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));

    std::vector<std::string> interfaces;
    constexpr std::size_t version_digits = 3;
    const std::size_t name_size = prefix.size() + version_digits;
    std::size_t offset = 0;
    while ((offset = bytes.find(prefix, offset)) != std::string::npos) {
        if ((offset == 0 || !is_printable_ascii(bytes[offset - 1])) &&
            bytes.size() - offset > name_size && bytes[offset + name_size] == '\0') {
            const auto version = std::string_view(bytes).substr(offset + prefix.size(), version_digits);
            if (std::all_of(version.begin(), version.end(),
                            [](char value) { return value >= '0' && value <= '9'; })) {
                interfaces.push_back(bytes.substr(offset, name_size));
            }
        }
        offset += prefix.size();
    }
    std::sort(interfaces.begin(), interfaces.end());
    interfaces.erase(std::unique(interfaces.begin(), interfaces.end()), interfaces.end());
    return interfaces;
}

bool contains(const std::vector<std::string>& interfaces, const char* name) {
    return std::find(interfaces.begin(), interfaces.end(), name) != interfaces.end();
}

} // namespace

EngineCompatibility inspect_engine_compatibility(const std::filesystem::path& bin_dir) {
    EngineCompatibility result;
    if (bin_dir.empty()) {
        return result;
    }
    result.filesystem_interfaces = discover_interfaces(bin_dir / "filesystem_stdio.dll", "VFileSystem");
    result.material_system_interfaces = discover_interfaces(bin_dir / "materialsystem.dll", "VMaterialSystem");
    result.studio_render_interfaces = discover_interfaces(bin_dir / "studiorender.dll", "VStudioRender");
    result.requires_l4d2_backend =
        contains(result.filesystem_interfaces, "VFileSystem018") &&
        contains(result.material_system_interfaces, "VMaterialSystem080") &&
        contains(result.studio_render_interfaces, "VStudioRender026");
    if (result.requires_l4d2_backend) {
        result.detail = "Left 4 Dead 2 native 3D preview is not available in this development build; "
            "a dedicated engine backend is required.";
    }
    return result;
}

} // namespace bridge
