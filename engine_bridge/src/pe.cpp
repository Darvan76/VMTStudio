#include "bridge/pe.h"

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace bridge {

PeArch read_pe_arch(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return PeArch::Unknown;

    unsigned char dos[64]{};
    in.read(reinterpret_cast<char*>(dos), sizeof(dos));
    if (!in || dos[0] != 'M' || dos[1] != 'Z') return PeArch::Unknown;

    const std::uint32_t pe_offset =
        static_cast<std::uint32_t>(dos[0x3C]) |
        (static_cast<std::uint32_t>(dos[0x3D]) << 8) |
        (static_cast<std::uint32_t>(dos[0x3E]) << 16) |
        (static_cast<std::uint32_t>(dos[0x3F]) << 24);

    in.seekg(pe_offset, std::ios::beg);
    unsigned char sig[4]{};
    in.read(reinterpret_cast<char*>(sig), 4);
    if (!in || sig[0] != 'P' || sig[1] != 'E' || sig[2] != 0 || sig[3] != 0) {
        return PeArch::Unknown;
    }

    unsigned char machine_bytes[2]{};
    in.read(reinterpret_cast<char*>(machine_bytes), 2);
    if (!in) return PeArch::Unknown;

    const std::uint16_t machine = static_cast<std::uint16_t>(
        machine_bytes[0] | (static_cast<std::uint16_t>(machine_bytes[1]) << 8));

    switch (machine) {
        case 0x14C:  return PeArch::X86; // IMAGE_FILE_MACHINE_I386
        case 0x8664: return PeArch::X64; // IMAGE_FILE_MACHINE_AMD64
        default:     return PeArch::Unknown;
    }
}

const char* to_string(PeArch arch) {
    switch (arch) {
        case PeArch::X86: return "x86";
        case PeArch::X64: return "x64";
        default:          return "unknown";
    }
}

} // namespace bridge
