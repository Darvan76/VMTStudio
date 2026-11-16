#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bridge {

// Evidence from bounded, read-only DLL scans. These are embedded interface
// names, not interfaces obtained from CreateInterface. Absence of a recognized
// fingerprint does not establish compatibility with the SDK 2013 backend.
struct EngineCompatibility {
    std::vector<std::string> filesystem_interfaces;
    std::vector<std::string> material_system_interfaces;
    std::vector<std::string> studio_render_interfaces;
    bool requires_l4d2_backend = false;
    std::string detail;
};

// Reads at most 32 MiB of each DLL without loading executable code. Missing or
// unreadable DLLs contribute no evidence. Only the verified L4D2 interface
// combination is recognized; unknown and future versions remain unclassified.
EngineCompatibility inspect_engine_compatibility(const std::filesystem::path& bin_dir);

} // namespace bridge
