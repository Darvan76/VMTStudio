#pragma once

#include <cstdint>
#include <filesystem>

namespace bridge {

// Machine architecture of a Windows PE image (a .dll / .exe on disk).
enum class PeArch {
    Unknown,
    X86, // 32-bit (IMAGE_FILE_MACHINE_I386)
    X64, // 64-bit (IMAGE_FILE_MACHINE_AMD64)
};

// Reads the DOS + PE headers of a file and reports its architecture.
// Returns PeArch::Unknown if the file cannot be read or is not a valid PE.
PeArch read_pe_arch(const std::filesystem::path& path);

const char* to_string(PeArch arch);

} // namespace bridge
