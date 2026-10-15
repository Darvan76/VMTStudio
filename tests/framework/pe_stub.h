#pragma once

// pe_stub.h - synthesizes minimal Windows PE images so tests can build game
// install fixtures without real engine DLLs. The generated files carry valid
// DOS / PE / optional headers (the machine field is selectable), a single
// ".rdata" section and an optional export directory listing the given names.
//
// Layout (all little-endian):
//   0x000  DOS header (64 bytes, e_lfanew = 0x40)
//   0x040  "PE\0\0"
//   0x044  COFF header (20 bytes)
//   0x058  optional header (0xE0 for PE32, 0xF0 for PE32+)
//   0x058 + SOH  one section header (40 bytes): ".rdata"
//   0x200  section raw data: optional IMAGE_EXPORT_DIRECTORY + name strings
//
// The section maps RVA 0x1000 to file offset 0x200, so an export blob at file
// offset 0x200 + o lives at RVA 0x1000 + o.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pe_stub {

constexpr std::uint16_t kMachineI386 = 0x14C;  // IMAGE_FILE_MACHINE_I386
constexpr std::uint16_t kMachineAmd64 = 0x8664; // IMAGE_FILE_MACHINE_AMD64

inline void set_u8(std::string& out, std::size_t offset, std::uint8_t value) {
    out[offset] = static_cast<char>(value);
}

inline void set_u16(std::string& out, std::size_t offset, std::uint16_t value) {
    set_u8(out, offset, static_cast<std::uint8_t>(value & 0xFFu));
    set_u8(out, offset + 1, static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

inline void set_u32(std::string& out, std::size_t offset, std::uint32_t value) {
    set_u8(out, offset, static_cast<std::uint8_t>(value & 0xFFu));
    set_u8(out, offset + 1, static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    set_u8(out, offset + 2, static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    set_u8(out, offset + 3, static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

// Writes a minimal DLL image claiming `machine`. When `exports` is non-empty
// the image carries an export directory listing exactly those names; when it
// is empty there is no export directory at all. Returns true on success.
inline bool write_dll(const std::filesystem::path& path,
                      std::uint16_t machine,
                      const std::vector<const char*>& exports) {
    const bool plus = (machine == kMachineAmd64);
    const std::uint16_t size_of_optional = plus ? 0xF0u : 0xE0u;
    const std::size_t optional = 0x58;
    const std::size_t section_header = optional + size_of_optional;
    const std::size_t raw_base = 0x200;
    const std::uint32_t rva_base = 0x1000;

    // --- export blob (built first so the data directory can reference it) ---
    std::string blob;
    std::uint32_t export_rva = 0;
    std::uint32_t export_size = 0;
    if (!exports.empty()) {
        const std::size_t count = exports.size();
        const char* dll_name = "game.dll";
        const std::size_t functions_offset = 40;
        const std::size_t names_offset = functions_offset + 4 * count;
        const std::size_t ordinals_offset = names_offset + 4 * count;
        std::size_t strings_offset = ordinals_offset + 2 * count;
        const std::size_t dll_name_offset = strings_offset;
        strings_offset += std::strlen(dll_name) + 1;
        std::vector<std::size_t> name_offsets(count);
        for (std::size_t i = 0; i < count; ++i) {
            name_offsets[i] = strings_offset;
            strings_offset += std::strlen(exports[i]) + 1;
        }

        blob.assign(strings_offset, '\0');
        // IMAGE_EXPORT_DIRECTORY (only the fields that matter here).
        set_u32(blob, 0x0C, rva_base + static_cast<std::uint32_t>(dll_name_offset)); // Name
        set_u32(blob, 0x14, static_cast<std::uint32_t>(count)); // NumberOfFunctions
        set_u32(blob, 0x18, static_cast<std::uint32_t>(count)); // NumberOfNames
        set_u32(blob, 0x1C, rva_base + static_cast<std::uint32_t>(functions_offset));
        set_u32(blob, 0x20, rva_base + static_cast<std::uint32_t>(names_offset));
        set_u32(blob, 0x24, rva_base + static_cast<std::uint32_t>(ordinals_offset));
        for (std::size_t i = 0; i < count; ++i) {
            // Function bodies point at the section start; the values are never
            // dereferenced by the header-level checks under test.
            set_u32(blob, functions_offset + 4 * i, rva_base);
            set_u32(blob, names_offset + 4 * i,
                    rva_base + static_cast<std::uint32_t>(name_offsets[i]));
            set_u16(blob, ordinals_offset + 2 * i, static_cast<std::uint16_t>(i));
            std::memcpy(&blob[name_offsets[i]], exports[i], std::strlen(exports[i]));
        }
        std::memcpy(&blob[dll_name_offset], dll_name, std::strlen(dll_name));

        export_rva = rva_base;
        export_size = 40;
    }

    // Pad the blob to the file alignment so SizeOfRawData stays realistic.
    while (blob.size() % 0x200u != 0) {
        blob.push_back('\0');
    }

    // --- headers ---
    std::string image(raw_base, '\0');
    set_u8(image, 0x00, 0x4D); // 'M'
    set_u8(image, 0x01, 0x5A); // 'Z'
    set_u32(image, 0x3C, 0x40); // e_lfanew
    image[0x40] = 'P';
    image[0x41] = 'E';
    // 0x42 / 0x43 stay zero.
    set_u16(image, 0x44, machine);
    set_u16(image, 0x46, 1);        // NumberOfSections
    set_u32(image, 0x48, 0);        // TimeDateStamp
    set_u32(image, 0x4C, 0);        // PointerToSymbolTable
    set_u32(image, 0x50, 0);        // NumberOfSymbols
    set_u16(image, 0x54, size_of_optional);
    set_u16(image, 0x56, 0x2100);   // Characteristics: IMAGE_FILE_DLL

    set_u16(image, optional + 0x00, plus ? 0x20Bu : 0x10Bu); // Magic
    set_u8(image, optional + 0x02, 14);                      // MajorLinkerVersion
    set_u32(image, optional + 0x04, 0);                      // SizeOfCode
    set_u32(image, optional + 0x08, static_cast<std::uint32_t>(blob.size()));
    set_u32(image, optional + 0x0C, 0);                      // SizeOfUninitializedData
    set_u32(image, optional + 0x10, 0);                      // AddressOfEntryPoint
    set_u32(image, optional + 0x14, rva_base);               // BaseOfCode
    if (!plus) {
        set_u32(image, optional + 0x18, rva_base);           // BaseOfData (PE32 only)
    }
    const std::size_t image_base = plus ? optional + 0x18 : optional + 0x1C;
    set_u32(image, image_base, 0x00400000);                  // ImageBase (low half)
    if (plus) {
        set_u32(image, image_base + 4, 0);                   // ImageBase (high half)
    }
    set_u32(image, optional + 0x20, 0x1000);                 // SectionAlignment
    set_u32(image, optional + 0x24, 0x200);                  // FileAlignment
    set_u16(image, optional + 0x28, 6);                      // MajorOperatingSystemVersion
    set_u16(image, optional + 0x2A, 0);                      // MinorOperatingSystemVersion
    set_u16(image, optional + 0x2C, 6);                      // MajorImageVersion
    set_u16(image, optional + 0x2E, 0);                      // MinorImageVersion
    set_u16(image, optional + 0x30, 6);                      // MajorSubsystemVersion
    set_u16(image, optional + 0x32, 0);                      // MinorSubsystemVersion
    set_u32(image, optional + 0x34, 0);                      // Win32VersionValue
    set_u32(image, optional + 0x38, 0x2000);                 // SizeOfImage
    set_u32(image, optional + 0x3C, 0x200);                  // SizeOfHeaders
    set_u32(image, optional + 0x40, 0);                      // CheckSum
    set_u16(image, optional + 0x44, 2);                      // Subsystem: GUI
    set_u16(image, optional + 0x46, 0);                      // DllCharacteristics
    if (plus) {
        set_u32(image, optional + 0x48, 0x100000);           // SizeOfStackReserve
        set_u32(image, optional + 0x4C, 0x1000);             // SizeOfStackCommit
        set_u32(image, optional + 0x50, 0x100000);           // SizeOfHeapReserve
        set_u32(image, optional + 0x54, 0x1000);             // SizeOfHeapCommit
    } else {
        set_u32(image, optional + 0x48, 0x100000);           // SizeOfStackReserve
        set_u32(image, optional + 0x4C, 0x1000);             // SizeOfStackCommit
        set_u32(image, optional + 0x50, 0x100000);           // SizeOfHeapReserve
        set_u32(image, optional + 0x54, 0x1000);             // SizeOfHeapCommit
    }
    set_u32(image, optional + 0x58, 0);                      // LoaderFlags
    set_u32(image, optional + (plus ? 0x6C : 0x5C), 16);     // NumberOfRvaAndSizes
    const std::size_t data_dirs = optional + (plus ? 0x70 : 0x60);
    set_u32(image, data_dirs + 0, export_rva);               // DataDirectory[0]: export
    set_u32(image, data_dirs + 4, export_size);

    // Section header: ".rdata"
    image[section_header + 0] = '.';
    image[section_header + 1] = 'r';
    image[section_header + 2] = 'd';
    image[section_header + 3] = 'a';
    image[section_header + 4] = 't';
    image[section_header + 5] = 'a';
    set_u32(image, section_header + 8, static_cast<std::uint32_t>(blob.size())); // VirtualSize
    set_u32(image, section_header + 12, rva_base);           // VirtualAddress
    set_u32(image, section_header + 16, static_cast<std::uint32_t>(blob.size()));
    set_u32(image, section_header + 20, static_cast<std::uint32_t>(raw_base));
    set_u32(image, section_header + 36, 0x40000040);         // Characteristics: readable data

    image += blob;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::out | std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(image.data(), static_cast<std::streamsize>(image.size()));
    return static_cast<bool>(out);
}

} // namespace pe_stub
