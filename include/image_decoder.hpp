// DarkHouse — photo decoding for the live preview.
//
// Produces scene-linear RGBA float pixels (straight alpha, top row first),
// the working format of the document and the develop graph:
//
//   JPEG, PNG (8/16-bit), BMP, TGA, GIF, PSD   decoded with stb_image, sRGB -> linear
//   Radiance HDR                                already linear
//   TIFF-based RAW (NEF, CR2, ARW, DNG, ORF,    the largest embedded full-size JPEG
//   PEF, RW2, ...) and Fujifilm RAF             preview (demosaicing RAW sensor data
//                                               is a later milestone)
//   TIFF                                        uncompressed 8/16-bit RGB(A), or its
//                                               embedded JPEG
//
// EXIF orientation is applied, and images larger than maxDimension are
// downscaled by area averaging in linear light (a working preview keeps
// live editing interactive on multi-megapixel files).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace darkhouse {

struct DecodeOptions {
    // Longest edge of the result; larger images are area-downscaled. 0 = full size.
    std::uint32_t maxDimension = 0;
};

struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba;          // width * height * 4, linear light, straight alpha
    std::uint32_t sourceWidth = 0;    // before downscaling and orientation
    std::uint32_t sourceHeight = 0;
    int orientation = 1;              // EXIF orientation that was applied (1 = as stored)
    std::string format;               // e.g. "JPEG", "PNG 16-bit", "NEF embedded JPEG preview"
};

// Decodes `path`. Throws std::runtime_error with a message fit for the UI
// ("... is a HEIF file, which DarkHouse cannot decode yet") on failure.
// Thread-safe; meant to run on a worker thread.
[[nodiscard]] DecodedImage decodeImage(const std::filesystem::path& path, const DecodeOptions& options = {});

namespace decode_detail {

// Linear-light value of an 8-bit / 16-bit sRGB-encoded sample.
[[nodiscard]] float srgbToLinear8(std::uint8_t value) noexcept;
[[nodiscard]] float srgbToLinear16(std::uint16_t value) noexcept;

// Applies an EXIF orientation (1..8) to interleaved RGBA float pixels.
void applyOrientation(int orientation, std::uint32_t& width, std::uint32_t& height, std::vector<float>& rgba);

}  // namespace decode_detail
}  // namespace darkhouse
