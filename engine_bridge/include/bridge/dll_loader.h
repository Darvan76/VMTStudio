#pragma once

#include "bridge/pe.h"
#include "vmt/result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace bridge {

// Result of attempting to locate, inspect and load one DLL.
struct DllReport {
    std::string name;
    bool present = false;             // file exists in bin_dir
    PeArch arch = PeArch::Unknown;    // PE header machine (Unknown if not a PE)
    bool loaded = false;              // LoadLibrary succeeded
    bool create_interface = false;    // exports "CreateInterface"
    bool command_line = false;        // exports "CommandLine" (GMod tier0)
    bool command_line_tier0 = false;  // exports "CommandLine_Tier0" (HL2/TF2 tier0)
    std::string error;                // exact error text (empty on success)
};

struct LoaderOptions {
    std::filesystem::path bin_dir;
    std::vector<std::string> dll_names; // load order
};

class DllLoader {
public:
    // Loads each DLL in order with explicit per-load search flags (the DLL's
    // own directory is searched for dependencies, e.g. steam_api.dll next to
    // the game binaries) and resolves its "CreateInterface" export. The
    // process-wide DLL search path is left untouched so this can run before an
    // in-process engine initialization. Never throws; failures are reported
    // per DLL.
    static vmt::Result<std::vector<DllReport>> load(const LoaderOptions& options);
};

} // namespace bridge
