#include "vtf/vtf_reader.h"

// VTFLib++ is used only in this translation unit: no VTFLib++ type, macro or
// function may leak through vtf/vtf_reader.h.
#include "VTFLib.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtf {
namespace {

// --- VTFLib++ library lifetime ---------------------------------------------
//
// VTFLib++ keeps a process-wide image singleton. Every entry point below is
// serialized through this mutex and the library is initialized exactly once,
// with one reusable image object bound.

std::mutex& vtflib_mutex() {
    static std::mutex mutex;
    return mutex;
}

bool ensure_vtflib_ready() {
    static const bool ready = [] {
        if (vlInitialize() == vlFalse) {
            return false;
        }
        vlUInt image = 0;
        if (vlCreateImage(&image) == vlFalse) {
            return false;
        }
        if (vlBindImage(image) == vlFalse) {
            return false;
        }
        return true;
    }();
    return ready;
}

// The bound image object is reusable, but its last pixel/header allocation must
// not outlive a query. These guards are always destroyed while the mutex is held.
struct ImageContents {
    ~ImageContents() { vlImageDestroy(); }
};

std::string vtflib_last_error() {
    const vlChar* text = vlGetLastError();
    return text != nullptr ? std::string(text) : std::string();
}

vmt::Error unexpected_error(const std::filesystem::path& path, std::string_view detail) {
    vmt::Error error = vmt::make_error(
        vmt::ErrorCode::Internal,
        "unexpected error while reading the VTF file: " + path.filename().u8string(),
        "Try again; if the problem persists, the file may be corrupt", path.u8string());
    error.detail = std::string(detail);
    return error;
}

// --- File input -------------------------------------------------------------

// VTFLib++ addresses image buffers with 32-bit sizes.
constexpr std::uint64_t kMaxVtfFileSize = 0xFFFFFFFFull;

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
                vmt::Error& error, bool header_only = false) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec || !exists) {
        error = vmt::make_error(vmt::ErrorCode::NotFound,
                                "the VTF file does not exist: " + path.u8string(),
                                "Check the material's texture path or pick another file",
                                path.u8string());
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = vmt::make_error(vmt::ErrorCode::PermissionDenied,
                                "the VTF file could not be opened: " + path.u8string(),
                                "Check that the file is readable and not locked by another program",
                                path.u8string());
        return false;
    }
    if (header_only && stream.rdbuf()->pubsetbuf(nullptr, 0) == nullptr) {
        error = vmt::make_error(vmt::ErrorCode::IoError,
                                "could not disable VTF header read-ahead", {}, path.u8string());
        return false;
    }

    stream.seekg(0, std::ios::end);
    const std::streamoff end = stream.tellg();
    if (end < 0) {
        error = vmt::make_error(vmt::ErrorCode::IoError,
                                "the VTF file size could not be determined: " + path.u8string(),
                                "Check that the file is readable",
                                path.u8string());
        return false;
    }

    const std::uint64_t size = static_cast<std::uint64_t>(end);
    if (size > kMaxVtfFileSize) {
        error = vmt::make_error(vmt::ErrorCode::OutOfMemory,
                                "the VTF file is larger than 4 GiB, which VTFLib++ cannot load",
                                "Re-export the texture at a lower resolution",
                                path.u8string());
        return false;
    }

    std::uint64_t read_size = size;
    if (header_only) {
        std::array<std::uint8_t, kVtfHeaderProbeSize> prefix{};
        const auto prefix_size = static_cast<std::streamsize>(
            std::min<std::uint64_t>(size, prefix.size()));
        stream.seekg(0, std::ios::beg);
        if (prefix_size != 0 &&
            !stream.read(reinterpret_cast<char*>(prefix.data()), prefix_size)) {
            error = vmt::make_error(vmt::ErrorCode::IoError,
                "failed to read the VTF header: " + path.u8string(), {}, path.u8string());
            return false;
        }
        const auto required = vtf_header_size(prefix.data(), static_cast<std::size_t>(prefix_size), path);
        if (!required) {
            error = required.error();
            return false;
        }
        if (size < required.value()) {
            error = vmt::make_error(vmt::ErrorCode::VtfTruncated,
                "the file is shorter than its declared VTF header", {}, path.u8string());
            return false;
        }
        read_size = required.value();
    }

    try {
        bytes.resize(static_cast<std::size_t>(read_size));
    } catch (const std::exception&) {
        error = vmt::make_error(vmt::ErrorCode::OutOfMemory,
                                "not enough memory to read the VTF file: " + path.u8string(),
                                "Close other applications or re-export the texture at a lower resolution",
                                path.u8string());
        return false;
    }

    stream.seekg(0, std::ios::beg);
    if (read_size > 0 && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(read_size))) {
        error = vmt::make_error(vmt::ErrorCode::IoError,
                                "failed to read the VTF file: " + path.u8string(),
                                "Check that the file is readable and not locked by another program",
                                path.u8string());
        return false;
    }
    return true;
}

// --- Header pre-check -------------------------------------------------------
//
// The VTF file header starts with: "VTF\0" (4 bytes), version[2] (u32 LE
// each) and header size (u32 LE). This pre-check exists only to classify
// errors deterministically; VTFLib++ still performs its own full validation.

// CVTFFile::Load accepts sizeof(SVTFHeader_76_A) (80) plus 32 eight-byte
// resource entries. Keep this bound tied to the pinned library version.
static_assert(VTF_MAJOR_VERSION == 7 && VTF_MINOR_VERSION == 6,
              "Recheck the header bound when updating VTFLib");
constexpr std::uint32_t kMaxVtfHeaderSize = 80u + VTF_RSRC_MAX_DICTIONARY_ENTRIES * 8u;

inline std::uint32_t read_u32_le(const std::uint8_t* data) noexcept {
    std::uint32_t val = 0;
    std::memcpy(&val, data, sizeof(val));
    return val;
}

vmt::Result<std::uint32_t> checked_header_size(const std::uint8_t* data, std::size_t size,
                                               const std::filesystem::path& path) {
    if (data == nullptr) {
        return vmt::Err<std::uint32_t>(vmt::make_error(
            vmt::ErrorCode::InvalidArgument, "no VTF header data was supplied", {}, path.u8string()));
    }
    if (size < kVtfHeaderProbeSize) {
        return vmt::Err<std::uint32_t>(vmt::make_error(
            vmt::ErrorCode::VtfTruncated,
            "the file is too small to hold a VTF header (" + std::to_string(size) + " bytes)",
            "Re-export or re-download the texture; the file is truncated",
            path.u8string()));
    }

    if (std::memcmp(data, "VTF\0", 4) != 0) {
        return vmt::Err<std::uint32_t>(vmt::make_error(
            vmt::ErrorCode::VtfInvalidHeader,
            "the file does not start with the 'VTF' signature",
            "Pick a .vtf texture file; other file types are not supported",
            path.u8string()));
    }

    const std::uint32_t major = read_u32_le(data + 4);
    const std::uint32_t minor = read_u32_le(data + 8);
    if (major != static_cast<std::uint32_t>(VTF_MAJOR_VERSION) ||
        minor > static_cast<std::uint32_t>(VTF_MINOR_VERSION)) {
        return vmt::Err<std::uint32_t>(vmt::make_error(
            vmt::ErrorCode::VtfUnsupportedVersion,
            "VTF version " + std::to_string(major) + "." + std::to_string(minor) +
                " is not supported (supported: " + std::to_string(VTF_MAJOR_VERSION) + ".0 to " +
                std::to_string(VTF_MAJOR_VERSION) + "." + std::to_string(VTF_MINOR_VERSION) + ")",
            "Re-export the texture with a supported VTF version",
            path.u8string()));
    }

    const std::uint32_t declared_header_size = read_u32_le(data + 12);
    if (declared_header_size < kVtfHeaderProbeSize) {
        return vmt::Err<std::uint32_t>(vmt::make_error(
            vmt::ErrorCode::VtfInvalidHeader,
            "the VTF header size " + std::to_string(declared_header_size) +
                " is smaller than the " + std::to_string(kVtfHeaderProbeSize) + "-byte minimum",
            "Re-export or re-download the texture; the file looks corrupt",
            path.u8string()));
    }
    if (declared_header_size > kMaxVtfHeaderSize) {
        // VTFLib rejects this as a corrupt load, which this module has always
        // classified as VtfTruncated. Reject before allocating an input buffer.
        return vmt::Err<std::uint32_t>(vmt::make_error(
            vmt::ErrorCode::VtfTruncated, "the declared VTF header exceeds the supported header size",
            "Re-export or re-download the texture; the file looks corrupt", path.u8string()));
    }
    return vmt::Ok(declared_header_size);
}

vmt::Result<void> probe_vtf_header(const std::uint8_t* data, std::size_t size,
                                   const std::filesystem::path& path) {
    const auto declared = checked_header_size(data, size, path);
    if (!declared) return vmt::Err<void>(declared.error());
    if (size < declared.value()) {
        return vmt::Err<void>(vmt::make_error(
            vmt::ErrorCode::VtfTruncated,
            "the file declares a " + std::to_string(declared.value()) + "-byte header but only " +
                std::to_string(size) + " bytes are present",
            "Re-export or re-download the texture; the file is truncated",
            path.u8string()));
    }

    return vmt::Ok();
}

// --- Format policy ----------------------------------------------------------

// Views an HDR format through the 8-bit preview pipeline would produce
// meaningless colors, so vtf_reader rejects them explicitly.
bool is_hdr_format(VTFImageFormat format) {
    switch (format) {
        case IMAGE_FORMAT_RGBA16161616F:
        case IMAGE_FORMAT_RGBA16161616:
        case IMAGE_FORMAT_R32F:
        case IMAGE_FORMAT_RGB323232F:
        case IMAGE_FORMAT_RGBA32323232F:
        case IMAGE_FORMAT_BC6H:
        case IMAGE_FORMAT_BC6H_UNSIGNED:
            return true;
        default:
            return false;
    }
}

bool is_depth_stencil_format(VTFImageFormat format) {
    switch (format) {
        case IMAGE_FORMAT_NV_DST16:
        case IMAGE_FORMAT_NV_DST24:
        case IMAGE_FORMAT_NV_INTZ:
        case IMAGE_FORMAT_NV_RAWZ:
        case IMAGE_FORMAT_ATI_DST16:
        case IMAGE_FORMAT_ATI_DST24:
        case IMAGE_FORMAT_NV_NULL:
            return true;
        default:
            return false;
    }
}

// Empty string when the format is previewable, otherwise the user-facing
// reason why it is not.
const char* preview_unsupported_reason(const SVTFImageFormatInfo& info, VTFImageFormat format) noexcept {
    if (is_hdr_format(format)) {
        return "HDR formats are not supported for preview";
    }
    if (is_depth_stencil_format(format)) {
        return "depth/stencil formats are not supported for preview";
    }
    if (info.bIsSupported == vlFalse) {
        return "the format is not supported by VTFLib++";
    }
    return "";
}

std::string format_name_of(const SVTFImageFormatInfo* info) {
    if (info != nullptr && info->lpName != nullptr) {
        return std::string(info->lpName);
    }
    return std::string("Unknown");
}

// --- Library queries --------------------------------------------------------

VtfInfo read_info() {
    const VTFImageFormat format = vlImageGetFormat();
    const SVTFImageFormatInfo* format_info = vlImageGetImageFormatInfo(format);

    VtfInfo info;
    info.width = static_cast<std::uint32_t>(vlImageGetWidth());
    info.height = static_cast<std::uint32_t>(vlImageGetHeight());
    info.depth = static_cast<std::uint32_t>(vlImageGetDepth());
    info.frame_count = static_cast<std::uint32_t>(vlImageGetFrameCount());
    info.face_count = static_cast<std::uint32_t>(vlImageGetFaceCount());
    info.mip_count = static_cast<std::uint32_t>(vlImageGetMipmapCount());
    info.version_major = static_cast<std::uint32_t>(vlImageGetMajorVersion());
    info.version_minor = static_cast<std::uint32_t>(vlImageGetMinorVersion());
    info.format_id = static_cast<std::uint32_t>(format);
    info.format_name = format_name_of(format_info);

    const bool alpha_in_format = format_info != nullptr && format_info->uiAlphaBitsPerPixel > 0;
    info.has_alpha = alpha_in_format ||
                     vlImageGetFlag(TEXTUREFLAGS_ONEBITALPHA) != 0 ||
                     vlImageGetFlag(TEXTUREFLAGS_EIGHTBITALPHA) != 0;
    info.has_mipmaps = info.mip_count > 1;
    info.is_cubemap = vlImageGetFlag(TEXTUREFLAGS_ENVMAP) != 0 || info.face_count > 1;
    info.is_normal_map = vlImageGetFlag(TEXTUREFLAGS_NORMAL) != 0;
    info.is_hdr = is_hdr_format(format);

    const bool format_previewable = format_info != nullptr &&
                                    preview_unsupported_reason(*format_info, format)[0] == '\0';
    info.preview_supported = format_previewable && info.depth == 1;
    return info;
}

vmt::Error load_failure_error(const std::filesystem::path& path) {
    const std::string library_error = vtflib_last_error();
    std::string message = "VTFLib++ could not load the VTF file: " + path.filename().u8string();
    if (!library_error.empty()) {
        message += " (" + library_error + ")";
    }
    vmt::Error error = vmt::make_error(
        vmt::ErrorCode::VtfTruncated, std::move(message),
        "Re-export or re-download the texture; the file looks truncated or corrupt",
        path.u8string());
    error.detail = library_error;
    return error;
}

vmt::Error vtflib_not_ready_error() {
    return vmt::make_error(vmt::ErrorCode::Internal,
                           "VTFLib++ could not be initialized",
                           "Rebuild the thirdparty/vtflib library (see vtf_reader/CMakeLists.txt)");
}

bool allocate_rgba(std::uint32_t width, std::uint32_t height,
                   std::vector<std::uint8_t>& out, vmt::Error& error) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        error = vmt::make_error(vmt::ErrorCode::VtfInvalidHeader,
                                "invalid image dimensions for RGBA allocation (" +
                                    std::to_string(width) + "x" + std::to_string(height) + ")",
                                "The image dimensions are invalid");
        return false;
    }
    const std::uint64_t byte_count = static_cast<std::uint64_t>(width) * height * 4u;
    if (byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        error = vmt::make_error(vmt::ErrorCode::OutOfMemory,
                                "the decoded image is too large to fit in memory (" +
                                    std::to_string(width) + "x" + std::to_string(height) + ")",
                                "Re-export the texture at a lower resolution");
        return false;
    }
    try {
        out.resize(static_cast<std::size_t>(byte_count));
    } catch (const std::exception&) {
        error = vmt::make_error(vmt::ErrorCode::OutOfMemory,
                                "not enough memory to decode the texture (" +
                                    std::to_string(width) + "x" + std::to_string(height) + ")",
                                "Close other applications or re-export the texture at a lower resolution");
        return false;
    }
    return true;
}

// The two helpers below take an already-loaded buffer and assume
// vtflib_mutex() is held by the caller; the public entry points own the lock so
// that the path-based and memory-based variants share one serialization point.

// Rejects an unusable buffer before VTFLib++ sees it, with the same error
// classification the file-based path produces.
vmt::Result<void> check_memory_input(const std::uint8_t* data, std::size_t size,
                                     const std::filesystem::path& origin) {
    if (data == nullptr) {
        return vmt::Err<void>(vmt::make_error(
            vmt::ErrorCode::InvalidArgument,
            "no VTF data was supplied for '" + origin.u8string() + "'",
            "The texture could not be read from the game files"));
    }
    if (size > kMaxVtfFileSize) {
        return vmt::Err<void>(vmt::make_error(
            vmt::ErrorCode::OutOfMemory,
            "the VTF data is larger than 4 GiB, which VTFLib++ cannot load",
            "Re-export the texture at a lower resolution",
            origin.u8string()));
    }
    return vmt::Ok();
}

vmt::Result<VtfInfo> open_vtf_info_bytes_locked(const std::uint8_t* data, std::size_t size,
                                                const std::filesystem::path& origin) {
    if (auto checked = check_memory_input(data, size, origin); !checked.ok()) {
        return vmt::Err<VtfInfo>(checked.error());
    }
    if (auto probe = probe_vtf_header(data, size, origin); !probe.ok()) {
        return vmt::Err<VtfInfo>(probe.error());
    }
    if (!ensure_vtflib_ready()) {
        return vmt::Err<VtfInfo>(vtflib_not_ready_error());
    }
    const ImageContents contents;
    if (vlImageLoadLump(data, static_cast<vlUInt>(size), vlTrue) == vlFalse) {
        return vmt::Err<VtfInfo>(load_failure_error(origin));
    }
    return vmt::Ok(read_info());
}

vmt::Result<RgbaImage> decode_vtf_preview_bytes_locked(const std::uint8_t* data, std::size_t size,
                                                       std::uint32_t frame, std::uint32_t max_size,
                                                       const std::filesystem::path& origin) {
    if (auto checked = check_memory_input(data, size, origin); !checked.ok()) {
        return vmt::Err<RgbaImage>(checked.error());
    }
    if (auto probe = probe_vtf_header(data, size, origin); !probe.ok()) {
        return vmt::Err<RgbaImage>(probe.error());
    }
    if (!ensure_vtflib_ready()) {
        return vmt::Err<RgbaImage>(vtflib_not_ready_error());
    }
    const ImageContents contents;
    if (vlImageLoadLump(data, static_cast<vlUInt>(size), vlFalse) == vlFalse) {
        return vmt::Err<RgbaImage>(load_failure_error(origin));
    }

    const VTFImageFormat format = vlImageGetFormat();
    const SVTFImageFormatInfo* format_info = vlImageGetImageFormatInfo(format);

    if (is_hdr_format(format)) {
        return vmt::Err<RgbaImage>(vmt::make_error(
            vmt::ErrorCode::VtfUnsupportedFormat,
            "the texture uses the HDR format '" + format_name_of(format_info) + "', which cannot be previewed as 8-bit RGBA",
            "Preview is unavailable for HDR textures",
            origin.u8string()));
    }
    if (is_depth_stencil_format(format)) {
        return vmt::Err<RgbaImage>(vmt::make_error(
            vmt::ErrorCode::VtfUnsupportedFormat,
            "the texture uses the depth/stencil format '" + format_name_of(format_info) + "', which cannot be previewed",
            "Preview is unavailable for depth/stencil textures",
            origin.u8string()));
    }
    if (format_info == nullptr || format_info->bIsSupported == vlFalse) {
        return vmt::Err<RgbaImage>(vmt::make_error(
            vmt::ErrorCode::VtfUnsupportedFormat,
            "the texture format '" + format_name_of(format_info) + "' is not supported by VTFLib++",
            "Re-export the texture with a standard format such as DXT1, DXT5 or RGBA8888",
            origin.u8string()));
    }

    const std::uint32_t frame_count = static_cast<std::uint32_t>(vlImageGetFrameCount());
    if (frame >= frame_count) {
        return vmt::Err<RgbaImage>(vmt::make_error(
            vmt::ErrorCode::InvalidArgument,
            "frame " + std::to_string(frame) + " is out of range (the texture has " +
                std::to_string(frame_count) + " frame(s))",
            "Choose a frame between 0 and " + std::to_string(frame_count - 1),
            origin.u8string()));
    }

    const std::uint32_t depth = static_cast<std::uint32_t>(vlImageGetDepth());
    if (depth > 1) {
        return vmt::Err<RgbaImage>(vmt::make_error(
            vmt::ErrorCode::VtfUnsupportedFormat,
            "volume textures cannot be previewed (depth " + std::to_string(depth) + ")",
            "Preview is available for 2D textures only",
            origin.u8string()));
    }

    const std::uint32_t width = static_cast<std::uint32_t>(vlImageGetWidth());
    const std::uint32_t height = static_cast<std::uint32_t>(vlImageGetHeight());
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        return vmt::Err<RgbaImage>(vmt::make_error(
            vmt::ErrorCode::VtfInvalidHeader,
            "invalid texture dimensions: " + std::to_string(width) + "x" + std::to_string(height),
            "The VTF file has corrupted image dimensions",
            origin.u8string()));
    }
    const bool needs_downscale = (max_size != 0 && (width > max_size || height > max_size));
    const std::uint32_t longest = std::max(width, height);
    // Derive the output from the original aspect ratio. The selected mip can
    // have a clamped 1-pixel edge and must not redefine the output geometry.
    const std::uint32_t target_width = needs_downscale
        ? std::max(1u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(width) * max_size / longest))
        : width;
    const std::uint32_t target_height = needs_downscale
        ? std::max(1u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(height) * max_size / longest))
        : height;

    vlUInt source_width = width;
    vlUInt source_height = height;
    vlUInt mip_level = 0;
    const vlUInt mip_count = vlImageGetMipmapCount();
    if (needs_downscale) {
        // Decode the smallest existing mip that still covers both target
        // edges. This bounds the temporary RGBA buffer near the thumbnail
        // size without upscaling an undersized mip or discarding image detail.
        while (mip_level + 1 < mip_count && (source_width > 1 || source_height > 1)) {
            vlUInt next_width = 0;
            vlUInt next_height = 0;
            vlUInt next_depth = 0;
            vlImageComputeMipmapDimensions(width, height, depth, mip_level + 1,
                                           &next_width, &next_height, &next_depth);
            if (next_width < target_width || next_height < target_height) break;
            source_width = next_width;
            source_height = next_height;
            ++mip_level;
        }
    }

    vlByte* source = vlImageGetData(frame, 0, 0, mip_level);
    if (source == nullptr) {
        return vmt::Err<RgbaImage>(vmt::make_error(
            vmt::ErrorCode::Internal,
            "VTFLib++ returned no image data for frame " + std::to_string(frame),
            "Re-export the texture; the file may be corrupt",
            origin.u8string()));
    }

    vmt::Error error;
    RgbaImage image;

    if (source_width == target_width && source_height == target_height) {
        if (!allocate_rgba(target_width, target_height, image.pixels, error)) {
            return vmt::Err<RgbaImage>(std::move(error));
        }
        if (vlImageConvertToRGBA8888(source, image.pixels.data(), source_width, source_height, format) == vlFalse) {
            return vmt::Err<RgbaImage>(vmt::make_error(
                vmt::ErrorCode::VtfUnsupportedFormat,
                "the texture format '" + format_name_of(format_info) + "' could not be converted to RGBA8 (" +
                    vtflib_last_error() + ")",
                "Preview is unavailable for this texture format",
                origin.u8string()));
        }
        image.width = target_width;
        image.height = target_height;
        return vmt::Ok(std::move(image));
    }

    if (!allocate_rgba(target_width, target_height, image.pixels, error)) {
        return vmt::Err<RgbaImage>(std::move(error));
    }

    if (format == IMAGE_FORMAT_RGBA8888) {
        // Fast path: source is already uncompressed RGBA8888; resize directly without intermediate allocation.
        if (vlImageResize(source, image.pixels.data(), source_width, source_height, target_width, target_height,
                          MIPMAP_FILTER_NICE, vlFalse) == vlFalse) {
            return vmt::Err<RgbaImage>(vmt::make_error(
                vmt::ErrorCode::Internal,
                "failed to downscale the preview to " + std::to_string(target_width) + "x" +
                    std::to_string(target_height) + " (" + vtflib_last_error() + ")",
                "Try a different preview size",
                origin.u8string()));
        }
    } else {
        std::vector<std::uint8_t> rgba;
        if (!allocate_rgba(source_width, source_height, rgba, error)) {
            return vmt::Err<RgbaImage>(std::move(error));
        }
        if (vlImageConvertToRGBA8888(source, rgba.data(), source_width, source_height, format) == vlFalse) {
            return vmt::Err<RgbaImage>(vmt::make_error(
                vmt::ErrorCode::VtfUnsupportedFormat,
                "the texture format '" + format_name_of(format_info) + "' could not be converted to RGBA8 (" +
                    vtflib_last_error() + ")",
                "Preview is unavailable for this texture format",
                origin.u8string()));
        }
        if (vlImageResize(rgba.data(), image.pixels.data(), source_width, source_height, target_width, target_height,
                          MIPMAP_FILTER_NICE, vlFalse) == vlFalse) {
            return vmt::Err<RgbaImage>(vmt::make_error(
                vmt::ErrorCode::Internal,
                "failed to downscale the preview to " + std::to_string(target_width) + "x" +
                    std::to_string(target_height) + " (" + vtflib_last_error() + ")",
                "Try a different preview size",
                origin.u8string()));
        }
        // Release the decoded mip buffer as soon as resizing is complete.
        std::vector<std::uint8_t>().swap(rgba);
    }

    image.width = target_width;
    image.height = target_height;
    return vmt::Ok(std::move(image));
}

} // namespace

vmt::Result<std::uint32_t> vtf_header_size(const std::uint8_t* data, std::size_t size,
                                         const std::filesystem::path& origin) {
    try {
        return checked_header_size(data, size, origin);
    } catch (const std::exception& exception) {
        return vmt::Err<std::uint32_t>(unexpected_error(origin, exception.what()));
    } catch (...) {
        return vmt::Err<std::uint32_t>(unexpected_error(origin, "unknown exception"));
    }
}

std::vector<FormatSupport> list_formats() {
    std::vector<FormatSupport> formats;
    try {
        std::scoped_lock lock(vtflib_mutex());
        if (!ensure_vtflib_ready()) {
            return formats;
        }

        formats.reserve(static_cast<std::size_t>(IMAGE_FORMAT_COUNT));
        for (int format_id = 0; format_id < static_cast<int>(IMAGE_FORMAT_COUNT); ++format_id) {
            const VTFImageFormat format = static_cast<VTFImageFormat>(format_id);
            const SVTFImageFormatInfo* info = vlImageGetImageFormatInfo(format);
            if (info == nullptr || info->lpName == nullptr) {
                continue;
            }

            const std::string_view name = info->lpName;
            // The library fills the gaps in its enum with "ReservedNN"
            // placeholders; they are internal, not actual texture formats.
            if (name.rfind("Reserved", 0) == 0) {
                continue;
            }

            FormatSupport entry;
            entry.format_id = static_cast<std::uint32_t>(format_id);
            entry.name = std::string(name);
            const char* reason = preview_unsupported_reason(*info, format);
            entry.reason = reason;
            entry.preview_supported = (reason[0] == '\0');
            formats.push_back(std::move(entry));
        }
    } catch (...) {
        formats.clear();
    }
    return formats;
}

vmt::Result<VtfInfo> open_vtf_info(const std::filesystem::path& path) {
    try {
        std::vector<std::uint8_t> bytes;
        vmt::Error error;
        if (!read_file(path, bytes, error, true)) {
            return vmt::Err<VtfInfo>(std::move(error));
        }
        std::scoped_lock lock(vtflib_mutex());
        return open_vtf_info_bytes_locked(bytes.data(), bytes.size(), path);
    } catch (const std::exception& exception) {
        return vmt::Err<VtfInfo>(unexpected_error(path, exception.what()));
    } catch (...) {
        return vmt::Err<VtfInfo>(unexpected_error(path, "unknown exception"));
    }
}

vmt::Result<RgbaImage> decode_vtf_preview(const std::filesystem::path& path,
                                          std::uint32_t frame,
                                          std::uint32_t max_size) {
    try {
        std::scoped_lock lock(vtflib_mutex());
        std::vector<std::uint8_t> bytes;
        vmt::Error error;
        if (!read_file(path, bytes, error)) {
            return vmt::Err<RgbaImage>(std::move(error));
        }
        return decode_vtf_preview_bytes_locked(bytes.data(), bytes.size(), frame, max_size, path);
    } catch (const std::exception& exception) {
        return vmt::Err<RgbaImage>(unexpected_error(path, exception.what()));
    } catch (...) {
        return vmt::Err<RgbaImage>(unexpected_error(path, "unknown exception"));
    }
}

vmt::Result<VtfInfo> open_vtf_info_from_memory(const std::uint8_t* data, std::size_t size,
                                               const std::filesystem::path& origin) {
    try {
        std::scoped_lock lock(vtflib_mutex());
        return open_vtf_info_bytes_locked(data, size, origin);
    } catch (const std::exception& exception) {
        return vmt::Err<VtfInfo>(unexpected_error(origin, exception.what()));
    } catch (...) {
        return vmt::Err<VtfInfo>(unexpected_error(origin, "unknown exception"));
    }
}

vmt::Result<RgbaImage> decode_vtf_preview_from_memory(const std::uint8_t* data, std::size_t size,
                                                      std::uint32_t frame, std::uint32_t max_size,
                                                      const std::filesystem::path& origin) {
    try {
        std::scoped_lock lock(vtflib_mutex());
        return decode_vtf_preview_bytes_locked(data, size, frame, max_size, origin);
    } catch (const std::exception& exception) {
        return vmt::Err<RgbaImage>(unexpected_error(origin, exception.what()));
    } catch (...) {
        return vmt::Err<RgbaImage>(unexpected_error(origin, "unknown exception"));
    }
}

} // namespace vtf
