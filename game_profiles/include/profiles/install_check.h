#pragma once

// install_check - validates a game profile candidate against the real files on
// disk so empty, corrupt or incompatible installs produce one clear, actionable
// reason instead of a generic "invalid" flag.
//
// The check is read-only, best-effort and never throws: whenever a file cannot
// be inspected (unreadable, unsupported PE layout) the corresponding sub-check
// is skipped rather than reported as a failure.

#include <string>
#include <vector>

namespace profiles {

struct GameProfile; // game_manager.h (checked via const reference only)

// Failure taxonomy, ordered from "structural" to "integrity":
//
//   EmptyFolder  the game folder does not exist or contains no files at all.
//   NoGameInfo   the folder exists but gameinfo.txt is missing, so it is not a
//                Source Engine game/mod folder.
//   NoBin        no Source /bin folder (tier0.dll + vstdlib.dll) could be
//                resolved in or near the game folder.
//   WrongArch    the engine DLLs exist but are not 32-bit x86 PE images (this
//                application always drives the 32-bit hlmv_backend), or the
//                files are corrupt / not valid PE images at all.
//   MissingDll   the /bin folder was resolved but required engine DLLs are
//                absent (see InstallReport::missing_dlls).
//   NoExports    the DLLs are present with the right architecture but the
//                expected exports (CreateInterface / CommandLine_Tier0) are
//                missing; the binaries likely belong to an unsupported engine
//                build.
enum class InstallProblem {
    None,
    EmptyFolder,
    NoGameInfo,
    NoBin,
    WrongArch,
    MissingDll,
    NoExports,
};

// Short human-readable English tag for a problem, e.g. "empty folder". Empty
// string for InstallProblem::None. Intended for UI labels such as
// "Counter-Strike Source (invalid: empty folder)".
const char* to_string(InstallProblem problem) noexcept;

struct InstallReport {
    bool usable = false;                       // true only when problem == None
    InstallProblem problem = InstallProblem::None;
    std::string detail;                        // human-readable, actionable
    std::vector<std::string> missing_dlls;     // filled for MissingDll
};

// Checks a game profile candidate: gameinfo.txt exists, the folder is
// non-empty, bin_dir is resolvable via GameManager::detect_bin_dir (falling
// back to plain "<game>/bin" folders that lack the marker DLLs so the report
// can name what is missing), and the required engine DLLs (tier0.dll,
// vstdlib.dll, filesystem_stdio.dll, materialsystem.dll, shaderapidx9.dll) are
// present with the right architecture and (best-effort) the expected exports.
InstallReport check_install(const GameProfile& profile) noexcept;

} // namespace profiles
