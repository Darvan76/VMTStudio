#pragma once

// vtf_reader - VTF texture metadata reading and RGBA8 preview decoding.
//
// This module wraps VTFLib++ (LGPL-2.1, pinned at thirdparty/vtflib) and is
// the only place in VMTViewer allowed to include its headers. The interface
// below never exposes VTFLib++ types: consumers only need this header.
//
// Design rules:
//  - Every entry point returns vmt::Result<T>; no exception crosses this
//    boundary and no VTFLib++ state is retained between calls.
//  - Formats that cannot be turned into an 8-bit RGBA preview (HDR,
//    depth/stencil, reserved/unknown) fail with ErrorCode::VtfUnsupportedFormat
//    instead of returning garbage pixels.
//  - Volume textures are reported by open_vtf_info but cannot be previewed.
//  - decode_vtf_preview always decodes face 0; cubemap faces beyond the first
//    are not exposed by this API.

#include "vmt/error.h"
#include "vmt/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vtf {

// Metadata read from a VTF file. All values are copied out of the library;
// no library handle is kept.
struct VtfInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;       // > 1 for volume textures
    std::uint32_t frame_count = 1;
    std::uint32_t face_count = 1;  // 6 or 7 for cubemaps
    std::uint32_t mip_count = 1;
    std::uint32_t version_major = 0;
    std::uint32_t version_minor = 0;
    std::uint32_t format_id = 0;   // numeric VTF image format id as stored in the file
    std::string   format_name;     // human-readable, e.g. "DXT5", "RGBA8888"

    bool has_alpha = false;
    bool has_mipmaps = false;
    bool is_cubemap = false;
    bool is_normal_map = false;
    bool is_hdr = false;

    // True when decode_vtf_preview can produce an image for this file
    // (format supported by policy and not a volume texture).
    bool preview_supported = false;
};

// A decoded 8-bit RGBA image, row-major, top row first.
// Invariant: pixels.size() == width * height * 4.
struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

// Support status for one VTF image format.
struct FormatSupport {
    std::uint32_t format_id = 0;
    std::string   name;
    bool          preview_supported = false;
    std::string   reason;  // empty when preview_supported is true
};

// Every image format known to the pinned VTFLib++ except reserved
// placeholders. preview_supported mirrors VTFLib++ support minus the formats
// vtf_reader rejects by policy (HDR, depth/stencil).
std::vector<FormatSupport> list_formats();

// Reads VTF metadata. The file is parsed header-only; no pixel decoding
// happens here.
//
// Errors:
//  - NotFound / PermissionDenied / IoError: file access.
//  - VtfTruncated: file smaller than a VTF header or than its declared header.
//  - VtfInvalidHeader: missing "VTF" signature or a corrupt header size.
//  - VtfUnsupportedVersion: version outside the range of the pinned VTFLib++
//    (7.0 to 7.6).
//  - OutOfMemory: file larger than 4 GiB (VTFLib++ limit) or allocation
//    failure.
//  - Internal: VTFLib++ could not be initialized or an unexpected error
//    occurred.
vmt::Result<VtfInfo> open_vtf_info(const std::filesystem::path& path);

// Decodes one frame of a VTF file to RGBA8.
//  - frame: 0-based frame index, must be < VtfInfo::frame_count.
//  - max_size: longest edge of the returned image in pixels; 0 keeps the
//    original size. The aspect ratio is preserved.
//
// Errors: the same as open_vtf_info, plus
//  - InvalidArgument: frame is out of range.
//  - VtfUnsupportedFormat: HDR, depth/stencil or otherwise unconvertible
//    formats, volume textures, or a failed format conversion.
vmt::Result<RgbaImage> decode_vtf_preview(const std::filesystem::path& path,
                                          std::uint32_t frame,
                                          std::uint32_t max_size);

// Module identity; also gives the static library a guaranteed symbol.
const char* vtf_reader_version_string() noexcept;

} // namespace vtf
