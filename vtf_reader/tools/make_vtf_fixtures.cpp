// make_vtf_fixtures - regenerates the committed VTF fixture set under
// tests/data/vtf using the pinned VTFLib++ build.
//
// This tool is not part of the normal build. Build and run it on demand:
//
//   cmake --build build --target make_vtf_fixtures --config Release
//   build\bin\Release\make_vtf_fixtures.exe tests\data\vtf
//
// The fixtures are deterministic (fixed pixel pattern): re-running the tool
// must reproduce byte-identical files, so the committed binaries stay in sync
// with the generator. vtf_tests.cpp documents the expected.json schema.
//
// Produced files:
//   dxt1.vtf      64x64 DXT1,  mipmaps, no alpha
//   rgba8888.vtf  64x64 RGBA8888, mipmaps, gradient alpha
//   multiframe.vtf 32x32 DXT1, 4 frames, mipmaps, no alpha

#include "VTFLib.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

bool fail(const std::string& what) {
    std::fprintf(stderr, "make_vtf_fixtures: %s: %s\n", what.c_str(), vlGetLastError());
    return false;
}

// Deterministic RGBA8888 pattern: red ramps along x, green along y, blue per
// frame and an optional alpha ramp along x.
std::vector<vlByte> make_pixels(vlUInt width, vlUInt height, vlUInt frame, bool alpha) {
    std::vector<vlByte> pixels(static_cast<std::size_t>(width) * height * 4);
    for (vlUInt y = 0; y < height; ++y) {
        for (vlUInt x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[i + 0] = static_cast<vlByte>((x * 255) / (width - 1));
            pixels[i + 1] = static_cast<vlByte>((y * 255) / (height - 1));
            pixels[i + 2] = static_cast<vlByte>(40 + frame * 60);
            pixels[i + 3] = alpha ? static_cast<vlByte>(255 - (x * 255) / (width - 1)) : 255;
        }
    }
    return pixels;
}

SVTFCreateOptions create_options(VTFImageFormat format) {
    SVTFCreateOptions options;
    vlImageCreateDefaultCreateStructure(&options);
    options.ImageFormat = format;
    options.bMipmaps = vlTrue;
    options.bThumbnail = vlTrue;
    return options;
}

bool save_single(const std::filesystem::path& out, vlUInt size, VTFImageFormat format, bool alpha) {
    std::vector<vlByte> pixels = make_pixels(size, size, 0, alpha);
    SVTFCreateOptions options = create_options(format);
    if (vlImageCreateSingle(size, size, pixels.data(), &options) == vlFalse) {
        return fail("create " + out.filename().string());
    }
    if (vlImageSave(out.string().c_str()) == vlFalse) {
        return fail("save " + out.filename().string());
    }
    std::printf("wrote %s (%ux%u, %s)\n", out.filename().string().c_str(), size, size,
                vlImageGetImageFormatInfo(vlImageGetFormat())->lpName);
    return true;
}

bool save_multiframe(const std::filesystem::path& out, vlUInt size, vlUInt frames,
                     VTFImageFormat format) {
    std::vector<std::vector<vlByte>> frame_data;
    std::vector<vlByte*> pointers;
    frame_data.reserve(frames);
    pointers.reserve(frames);
    for (vlUInt frame = 0; frame < frames; ++frame) {
        frame_data.push_back(make_pixels(size, size, frame, false));
    }
    for (std::vector<vlByte>& data : frame_data) {
        pointers.push_back(data.data());
    }

    SVTFCreateOptions options = create_options(format);
    if (vlImageCreateMultiple(size, size, frames, 1, 1, pointers.data(), &options) == vlFalse) {
        return fail("create " + out.filename().string());
    }
    if (vlImageSave(out.string().c_str()) == vlFalse) {
        return fail("save " + out.filename().string());
    }
    std::printf("wrote %s (%ux%u, %s, %u frames)\n", out.filename().string().c_str(), size, size,
                vlImageGetImageFormatInfo(vlImageGetFormat())->lpName, frames);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path out_dir = argc > 1 ? argv[1] : ".";
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        std::fprintf(stderr, "make_vtf_fixtures: cannot create %s\n", out_dir.string().c_str());
        return 1;
    }

    if (vlInitialize() == vlFalse) {
        std::fprintf(stderr, "make_vtf_fixtures: VTFLib++ initialization failed\n");
        return 1;
    }

    vlUInt image = 0;
    if (vlCreateImage(&image) == vlFalse || vlBindImage(image) == vlFalse) {
        std::fprintf(stderr, "make_vtf_fixtures: VTFLib++ image setup failed: %s\n",
                     vlGetLastError());
        vlShutdown();
        return 1;
    }

    bool ok = true;
    ok = save_single(out_dir / "dxt1.vtf", 64, IMAGE_FORMAT_DXT1, false) && ok;
    ok = save_single(out_dir / "rgba8888.vtf", 64, IMAGE_FORMAT_RGBA8888, true) && ok;
    ok = save_multiframe(out_dir / "multiframe.vtf", 32, 4, IMAGE_FORMAT_DXT1) && ok;

    vlDeleteImage(image);
    vlShutdown();
    return ok ? 0 : 1;
}
