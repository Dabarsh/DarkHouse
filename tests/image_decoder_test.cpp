// Image decoder tests: sRGB linearisation, EXIF orientation, stb formats
// (PNG 8/16-bit, JPEG, HDR), TIFF (uncompressed and embedded previews),
// RAW-style containers (TIFF-based and Fujifilm RAF), area downscaling and
// error reporting. Test files are generated in memory.
//
// Usage: darkhouse_image_decoder_test <work directory>

#include "image_decoder.hpp"

#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace darkhouse;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::cout << "FAIL line " << __LINE__ << ": " #condition << '\n';              \
        }                                                                                  \
    } while (false)

fs::path g_dir;

fs::path writeFile(const std::string& name, const Bytes& bytes) {
    const fs::path path = g_dir / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<Bytes*>(context);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

Bytes encodePng(int w, int h, int comp, const std::vector<std::uint8_t>& pixels) {
    Bytes out;
    stbi_write_png_to_func(appendBytes, &out, w, h, comp, pixels.data(), w * comp);
    return out;
}

Bytes encodeJpeg(int w, int h, const std::vector<std::uint8_t>& rgb) {
    Bytes out;
    stbi_write_jpg_to_func(appendBytes, &out, w, h, 3, rgb.data(), 100);
    return out;
}

void put16BE(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put32BE(Bytes& b, std::uint32_t v) {
    put16BE(b, v >> 16);
    put16BE(b, v & 0xFFFF);
}

// 16-bit RGBA PNG (stb_image_write only writes 8-bit): stored deflate blocks.
Bytes encodePng16(std::uint32_t w, std::uint32_t h, const std::vector<std::uint16_t>& rgba) {
    Bytes raw;
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        for (std::uint32_t i = 0; i < w * 4; ++i) put16BE(raw, rgba[y * w * 4 + i]);
    }
    Bytes zlib{0x78, 0x01};
    for (std::size_t at = 0; at < raw.size(); at += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        zlib.push_back(at + n == raw.size() ? 1 : 0);
        zlib.push_back(static_cast<std::uint8_t>(n));
        zlib.push_back(static_cast<std::uint8_t>(n >> 8));
        zlib.push_back(static_cast<std::uint8_t>(~n));
        zlib.push_back(static_cast<std::uint8_t>(~n >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                    raw.begin() + static_cast<std::ptrdiff_t>(at + n));
    }
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put32BE(zlib, (b << 16) | a);

    auto crc32 = [](const Bytes& data, std::size_t from) {
        std::uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = from; i < data.size(); ++i) {
            crc ^= data[i];
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    };
    Bytes png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const Bytes& data) {
        put32BE(png, static_cast<std::uint32_t>(data.size()));
        const std::size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        put32BE(png, crc32(png, start));
    };
    Bytes ihdr;
    put32BE(ihdr, w);
    put32BE(ihdr, h);
    ihdr.insert(ihdr.end(), {16, 6, 0, 0, 0});  // 16-bit RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", zlib);
    chunk("IEND", {});
    return png;
}

// Minimal TIFF writer: blobs and IFDs are appended in any order; offsets are
// relative to the start of the TIFF (which is what EXIF APP1 expects too).
class TiffWriter {
public:
    struct Entry {
        std::uint16_t tag;
        std::uint16_t type;  // 3 = SHORT, 4 = LONG
        std::vector<std::uint32_t> values;
    };

    explicit TiffWriter(bool littleEndian) : little_(littleEndian) {
        bytes_ = {static_cast<std::uint8_t>(little_ ? 'I' : 'M'), static_cast<std::uint8_t>(little_ ? 'I' : 'M')};
        put16(42);
        put32(0);  // first IFD, set by setFirstIfd()
    }

    std::uint32_t append(const Bytes& blob) {
        align();
        const auto at = static_cast<std::uint32_t>(bytes_.size());
        bytes_.insert(bytes_.end(), blob.begin(), blob.end());
        return at;
    }

    std::uint32_t appendIfd(std::vector<Entry> entries, std::uint32_t next = 0) {
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
        align();
        const auto at = static_cast<std::uint32_t>(bytes_.size());
        put16(static_cast<std::uint32_t>(entries.size()));
        std::vector<std::pair<std::size_t, const Entry*>> external;
        for (const Entry& e : entries) {
            put16(e.tag);
            put16(e.type);
            put32(static_cast<std::uint32_t>(e.values.size()));
            const std::size_t field = bytes_.size();
            put32(0);
            const std::size_t unit = e.type == 3 ? 2 : 4;
            if (e.values.size() * unit <= 4) {
                bytes_.resize(field);
                for (std::uint32_t v : e.values) unit == 2 ? put16(v) : put32(v);
                bytes_.resize(field + 4);
            } else {
                external.emplace_back(field, &e);
            }
        }
        put32(next);
        for (const auto& [field, e] : external) {
            align();
            patch32(field, static_cast<std::uint32_t>(bytes_.size()));
            for (std::uint32_t v : e->values) e->type == 3 ? put16(v) : put32(v);
        }
        return at;
    }

    void setFirstIfd(std::uint32_t offset) { patch32(4, offset); }
    [[nodiscard]] const Bytes& bytes() const { return bytes_; }

    void put16(std::uint32_t v) {
        const auto lo = static_cast<std::uint8_t>(v), hi = static_cast<std::uint8_t>(v >> 8);
        little_ ? bytes_.insert(bytes_.end(), {lo, hi}) : bytes_.insert(bytes_.end(), {hi, lo});
    }

private:
    void put32(std::uint32_t v) {
        if (little_) {
            put16(v & 0xFFFF);
            put16(v >> 16);
        } else {
            put16(v >> 16);
            put16(v & 0xFFFF);
        }
    }
    void patch32(std::size_t at, std::uint32_t v) {
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t shift = little_ ? 8 * i : 8 * (3 - i);
            bytes_[at + i] = static_cast<std::uint8_t>(v >> shift);
        }
    }
    void align() {
        if (bytes_.size() % 2) bytes_.push_back(0);
    }

    bool little_;
    Bytes bytes_;
};

// A JPEG with an EXIF APP1 (orientation) spliced in after SOI.
Bytes withExifOrientation(const Bytes& jpeg, int orientation, bool littleEndian) {
    TiffWriter exif(littleEndian);
    exif.setFirstIfd(exif.appendIfd({{0x0112, 3, {static_cast<std::uint32_t>(orientation)}}}));
    Bytes app1{0xFF, 0xE1};
    put16BE(app1, static_cast<std::uint32_t>(2 + 6 + exif.bytes().size()));
    app1.insert(app1.end(), {'E', 'x', 'i', 'f', 0, 0});
    app1.insert(app1.end(), exif.bytes().begin(), exif.bytes().end());
    Bytes out(jpeg.begin(), jpeg.begin() + 2);
    out.insert(out.end(), app1.begin(), app1.end());
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    return out;
}

// w x h RGB image: left half `left`, right half `right`.
std::vector<std::uint8_t> halves(int w, int h, std::array<std::uint8_t, 3> left, std::array<std::uint8_t, 3> right) {
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w * h * 3));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto& c = x < w / 2 ? left : right;
            std::memcpy(&rgb[static_cast<std::size_t>((y * w + x) * 3)], c.data(), 3);
        }
    }
    return rgb;
}

const float* pixelAt(const DecodedImage& image, std::uint32_t x, std::uint32_t y) {
    return &image.rgba[(std::size_t{y} * image.width + x) * 4];
}

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// Colour check for lossy (JPEG) content, in linear light.
bool looksLike(const float* p, std::array<std::uint8_t, 3> srgb, float tolerance = 0.02f) {
    for (int c = 0; c < 3; ++c) {
        if (!near(p[c], decode_detail::srgbToLinear8(srgb[static_cast<std::size_t>(c)]), tolerance)) return false;
    }
    return near(p[3], 1.0f, 1e-6f);
}

std::string decodeError(const fs::path& path) {
    try {
        (void)decodeImage(path);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

constexpr std::array<std::uint8_t, 3> kRed{220, 30, 20};
constexpr std::array<std::uint8_t, 3> kBlue{20, 40, 230};

void testTransferFunction() {
    CHECK(decode_detail::srgbToLinear8(0) == 0.0f);
    CHECK(near(decode_detail::srgbToLinear8(255), 1.0f, 1e-6f));
    CHECK(near(decode_detail::srgbToLinear8(128), 0.2158605f, 1e-5f));
    CHECK(near(decode_detail::srgbToLinear8(10), 10.0f / 255.0f / 12.92f, 1e-7f));  // linear toe
    CHECK(near(decode_detail::srgbToLinear16(65535), 1.0f, 1e-6f));
    CHECK(near(decode_detail::srgbToLinear16(128 * 257), 0.2158605f, 1e-5f));
    for (int v = 1; v < 65536; ++v) {  // monotonic
        if (decode_detail::srgbToLinear16(static_cast<std::uint16_t>(v)) <=
            decode_detail::srgbToLinear16(static_cast<std::uint16_t>(v - 1))) {
            CHECK(!"srgbToLinear16 is not strictly increasing");
            break;
        }
    }
}

void testOrientation() {
    // Stored 3x2:  0 1 2 / 3 4 5. Expected display per the EXIF definition.
    const std::vector<std::vector<int>> expected = {
        {},
        {0, 1, 2, 3, 4, 5},  // 1: as stored (3x2)
        {2, 1, 0, 5, 4, 3},  // 2: mirror horizontal
        {5, 4, 3, 2, 1, 0},  // 3: rotate 180
        {3, 4, 5, 0, 1, 2},  // 4: mirror vertical
        {0, 3, 1, 4, 2, 5},  // 5: transpose (2x3 from here)
        {3, 0, 4, 1, 5, 2},  // 6: rotate 90 clockwise
        {5, 2, 4, 1, 3, 0},  // 7: transverse
        {2, 5, 1, 4, 0, 3},  // 8: rotate 90 counter-clockwise
    };
    for (int orientation = 1; orientation <= 8; ++orientation) {
        std::uint32_t w = 3, h = 2;
        std::vector<float> rgba(24);
        for (int i = 0; i < 6; ++i) rgba[static_cast<std::size_t>(i * 4)] = static_cast<float>(i);
        decode_detail::applyOrientation(orientation, w, h, rgba);
        CHECK((orientation >= 5 ? (w == 2 && h == 3) : (w == 3 && h == 2)));
        bool match = true;
        for (int i = 0; i < 6; ++i) {
            match = match && rgba[static_cast<std::size_t>(i * 4)] == static_cast<float>(expected[static_cast<std::size_t>(orientation)][static_cast<std::size_t>(i)]);
        }
        if (!match) std::cout << "orientation " << orientation << " mismatch\n";
        CHECK(match);
    }
}

void testPng8() {
    // 5x3 RGBA with every sample distinct, alpha included.
    std::vector<std::uint8_t> pixels(5 * 3 * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<std::uint8_t>((i * 37 + 11) % 256);
    const DecodedImage image = decodeImage(writeFile("rgba8.png", encodePng(5, 3, 4, pixels)));
    CHECK(image.width == 5 && image.height == 3);
    CHECK(image.sourceWidth == 5 && image.sourceHeight == 3);
    CHECK(image.format == "PNG");
    CHECK(image.orientation == 1);
    CHECK(image.rgba.size() == 5 * 3 * 4);
    bool exact = true;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const float expected = i % 4 == 3 ? pixels[i] / 255.0f : decode_detail::srgbToLinear8(pixels[i]);
        exact = exact && image.rgba[i] == expected;
    }
    CHECK(exact);

    // Grey (1 channel) expands to opaque RGB.
    const DecodedImage grey = decodeImage(writeFile("grey.png", encodePng(2, 1, 1, {0, 255})));
    CHECK(grey.width == 2 && grey.height == 1);
    CHECK(grey.rgba[0] == 0.0f && grey.rgba[3] == 1.0f);
    CHECK(near(grey.rgba[4], 1.0f, 1e-6f) && near(grey.rgba[6], 1.0f, 1e-6f) && grey.rgba[7] == 1.0f);
}

void testPng16() {
    std::vector<std::uint16_t> pixels(4 * 2 * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<std::uint16_t>((i * 4099 + 17) % 65536);
    const DecodedImage image = decodeImage(writeFile("rgba16.png", encodePng16(4, 2, pixels)));
    CHECK(image.width == 4 && image.height == 2);
    CHECK(image.format == "PNG 16-bit");
    bool exact = true;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const float expected = i % 4 == 3 ? pixels[i] / 65535.0f : decode_detail::srgbToLinear16(pixels[i]);
        exact = exact && image.rgba[i] == expected;
    }
    CHECK(exact);
}

void testJpegAndExif() {
    const Bytes jpeg = encodeJpeg(32, 16, halves(32, 16, kRed, kBlue));
    const DecodedImage plain = decodeImage(writeFile("plain.jpg", jpeg));
    CHECK(plain.width == 32 && plain.height == 16);
    CHECK(plain.format == "JPEG");
    CHECK(plain.orientation == 1);
    CHECK(looksLike(pixelAt(plain, 4, 8), kRed));
    CHECK(looksLike(pixelAt(plain, 27, 8), kBlue));

    // Orientation 6 (rotate 90 CW): the stored left half becomes the top.
    for (bool little : {true, false}) {
        const DecodedImage rotated = decodeImage(writeFile(little ? "exif6_ii.jpg" : "exif6_mm.jpg",
                                                           withExifOrientation(jpeg, 6, little)));
        CHECK(rotated.orientation == 6);
        CHECK(rotated.width == 16 && rotated.height == 32);
        CHECK(rotated.sourceWidth == 32 && rotated.sourceHeight == 16);
        CHECK(looksLike(pixelAt(rotated, 8, 4), kRed));
        CHECK(looksLike(pixelAt(rotated, 8, 27), kBlue));
    }
    // Orientation 3 (rotate 180): the left half ends up on the right.
    const DecodedImage flipped = decodeImage(writeFile("exif3.jpg", withExifOrientation(jpeg, 3, true)));
    CHECK(flipped.orientation == 3 && flipped.width == 32 && flipped.height == 16);
    CHECK(looksLike(pixelAt(flipped, 4, 8), kBlue));
    CHECK(looksLike(pixelAt(flipped, 27, 8), kRed));
    // Out-of-range orientation values are ignored.
    const DecodedImage bogus = decodeImage(writeFile("exif9.jpg", withExifOrientation(jpeg, 9, true)));
    CHECK(bogus.orientation == 1 && bogus.width == 32);
}

void testUncompressedTiff() {
    // 16-bit big-endian RGBA, one strip.
    {
        const std::uint32_t w = 5, h = 3;
        Bytes data;
        std::vector<std::uint16_t> values;
        for (std::uint32_t i = 0; i < w * h * 4; ++i) {
            values.push_back(static_cast<std::uint16_t>((i * 5003 + 101) % 65536));
            put16BE(data, values.back());
        }
        TiffWriter tiff(false);
        const std::uint32_t strip = tiff.append(data);
        tiff.setFirstIfd(tiff.appendIfd({
            {0x0100, 4, {w}}, {0x0101, 4, {h}}, {0x0102, 3, {16, 16, 16, 16}}, {0x0103, 3, {1}},
            {0x0106, 3, {2}}, {0x0111, 4, {strip}}, {0x0115, 3, {4}}, {0x0116, 4, {h}},
            {0x0117, 4, {static_cast<std::uint32_t>(data.size())}}, {0x011C, 3, {1}}, {0x0152, 3, {2}},
        }));
        const DecodedImage image = decodeImage(writeFile("rgba16_mm.tif", tiff.bytes()));
        CHECK(image.width == w && image.height == h);
        CHECK(image.format == "TIFF 16-bit");
        bool exact = image.rgba.size() == values.size();
        for (std::size_t i = 0; exact && i < values.size(); ++i) {
            const float expected = i % 4 == 3 ? values[i] / 65535.0f : decode_detail::srgbToLinear16(values[i]);
            exact = image.rgba[i] == expected;
        }
        CHECK(exact);
    }
    // 8-bit little-endian RGB, one strip per row, orientation 8.
    {
        const std::uint32_t w = 7, h = 2;
        TiffWriter tiff(true);
        std::vector<std::uint32_t> offsets, counts;
        std::vector<std::uint8_t> values;
        for (std::uint32_t y = 0; y < h; ++y) {
            Bytes row;
            for (std::uint32_t i = 0; i < w * 3; ++i) row.push_back(static_cast<std::uint8_t>((y * 100 + i * 9) % 256));
            values.insert(values.end(), row.begin(), row.end());
            offsets.push_back(tiff.append(row));
            counts.push_back(static_cast<std::uint32_t>(row.size()));
        }
        tiff.setFirstIfd(tiff.appendIfd({
            {0x0100, 3, {w}}, {0x0101, 3, {h}}, {0x0102, 3, {8, 8, 8}}, {0x0103, 3, {1}}, {0x0106, 3, {2}},
            {0x0111, 4, offsets}, {0x0112, 3, {8}}, {0x0115, 3, {3}}, {0x0116, 3, {1}}, {0x0117, 4, counts},
        }));
        const DecodedImage image = decodeImage(writeFile("rgb8_ii.tif", tiff.bytes()));
        CHECK(image.format == "TIFF");
        CHECK(image.orientation == 8);
        CHECK(image.width == h && image.height == w);  // rotated
        // Orientation 8 puts stored (x, y) at display (y, w - 1 - x).
        bool exact = true;
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const float* p = pixelAt(image, y, w - 1 - x);
                for (std::uint32_t c = 0; c < 3; ++c) {
                    exact = exact && p[c] == decode_detail::srgbToLinear8(values[(y * w + x) * 3 + c]);
                }
                exact = exact && p[3] == 1.0f;
            }
        }
        CHECK(exact);
    }
}

// A TIFF-based RAW: IFD0 = small uncompressed thumbnail with the camera
// orientation, SubIFDs = an embedded JPEG preview and lossless-JPEG sensor
// data (larger, but SOF3, so it must be skipped), IFD1 = a small JPEG
// thumbnail in swapped colours (visited last; the largest preview must win).
Bytes makeTiffRaw(const Bytes& preview, int orientation) {
    TiffWriter tiff(true);
    Bytes thumb(8 * 4 * 3, 90);
    const std::uint32_t thumbAt = tiff.append(thumb);
    const std::uint32_t previewAt = tiff.append(preview);
    const Bytes thumbJpeg = encodeJpeg(16, 8, halves(16, 8, kBlue, kRed));
    const std::uint32_t thumbJpegAt = tiff.append(thumbJpeg);
    const std::uint32_t ifd1 = tiff.appendIfd({
        {0x0103, 3, {6}}, {0x0201, 4, {thumbJpegAt}}, {0x0202, 4, {static_cast<std::uint32_t>(thumbJpeg.size())}},
    });
    Bytes lossless{0xFF, 0xD8, 0xFF, 0xC3, 0x00, 0x0B, 14};  // SOF3, 14-bit
    put16BE(lossless, 3000);
    put16BE(lossless, 4000);
    lossless.insert(lossless.end(), {1, 1, 0x11, 0});
    lossless.resize(lossless.size() + 64, 0);
    const std::uint32_t losslessAt = tiff.append(lossless);
    const std::uint32_t previewIfd = tiff.appendIfd({
        {0x00FE, 4, {1}}, {0x0103, 3, {6}}, {0x0201, 4, {previewAt}},
        {0x0202, 4, {static_cast<std::uint32_t>(preview.size())}},
    });
    const std::uint32_t rawIfd = tiff.appendIfd({
        {0x00FE, 4, {0}}, {0x0100, 4, {4000}}, {0x0101, 4, {3000}}, {0x0103, 3, {7}}, {0x0106, 3, {32803}},
        {0x0111, 4, {losslessAt}}, {0x0117, 4, {static_cast<std::uint32_t>(lossless.size())}},
    });
    tiff.setFirstIfd(tiff.appendIfd({
        {0x00FE, 4, {1}}, {0x0100, 4, {8}}, {0x0101, 4, {4}}, {0x0102, 3, {8, 8, 8}}, {0x0103, 3, {1}},
        {0x0106, 3, {2}}, {0x0111, 4, {thumbAt}}, {0x0112, 3, {static_cast<std::uint32_t>(orientation)}},
        {0x0115, 3, {3}}, {0x0117, 4, {static_cast<std::uint32_t>(thumb.size())}}, {0x014A, 4, {previewIfd, rawIfd}},
    }, ifd1));
    return tiff.bytes();
}

void testRawPreviews() {
    const Bytes preview = encodeJpeg(64, 32, halves(64, 32, kRed, kBlue));
    const DecodedImage nef = decodeImage(writeFile("camera.NEF", makeTiffRaw(preview, 8)));
    CHECK(nef.format == "NEF embedded JPEG preview");
    CHECK(nef.sourceWidth == 64 && nef.sourceHeight == 32);
    CHECK(nef.orientation == 8);
    CHECK(nef.width == 32 && nef.height == 64);
    // Orientation 8 (rotate 90 CCW): the stored left half becomes the bottom.
    CHECK(looksLike(pixelAt(nef, 16, 8), kBlue));
    CHECK(looksLike(pixelAt(nef, 16, 56), kRed));

    // RAW without any decodable preview: only the SOF3 sensor data.
    Bytes lossless{0xFF, 0xD8, 0xFF, 0xC3, 0x00, 0x0B, 14, 0x0B, 0xB8, 0x0F, 0xA0, 1, 1, 0x11, 0};
    TiffWriter bare(true);
    const std::uint32_t at = bare.append(lossless);
    bare.setFirstIfd(bare.appendIfd({{0x0100, 4, {4000}}, {0x0101, 4, {3000}}, {0x0103, 3, {7}},
                                     {0x0111, 4, {at}}, {0x0117, 4, {static_cast<std::uint32_t>(lossless.size())}}}));
    CHECK(decodeError(writeFile("bare.dng", bare.bytes())).find("contains no preview") != std::string::npos);

    // Fujifilm RAF: big-endian JPEG offset/length at byte 84; the orientation
    // comes from the preview's own EXIF.
    Bytes raf(160, 0);
    const char* magic = "FUJIFILMCCD-RAW 0201FF383501";
    std::memcpy(raf.data(), magic, std::strlen(magic));
    const Bytes rafPreview = withExifOrientation(preview, 3, false);
    Bytes field;
    put32BE(field, static_cast<std::uint32_t>(raf.size()));
    put32BE(field, static_cast<std::uint32_t>(rafPreview.size()));
    std::copy(field.begin(), field.end(), raf.begin() + 84);
    raf.insert(raf.end(), rafPreview.begin(), rafPreview.end());
    raf.resize(raf.size() + 256, 0);  // sensor data would follow
    const DecodedImage fuji = decodeImage(writeFile("camera.raf", raf));
    CHECK(fuji.format == "RAF embedded JPEG preview");
    CHECK(fuji.orientation == 3);
    CHECK(fuji.width == 64 && fuji.height == 32);
    CHECK(looksLike(pixelAt(fuji, 8, 16), kBlue));
    CHECK(looksLike(pixelAt(fuji, 56, 16), kRed));
}

void testHdr() {
    const std::vector<float> rgb = {0.0f, 0.5f, 1.0f, 2.0f, 8.0f, 0.125f};
    Bytes hdr;
    stbi_write_hdr_to_func(appendBytes, &hdr, 2, 1, 3, rgb.data());
    const DecodedImage image = decodeImage(writeFile("scene.hdr", hdr));
    CHECK(image.format == "Radiance HDR");
    CHECK(image.width == 2 && image.height == 1);
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t c = 0; c < 3; ++c) CHECK(near(image.rgba[i * 4 + c], rgb[i * 3 + c], rgb[i * 3 + c] * 0.01f + 1e-6f));
        CHECK(image.rgba[i * 4 + 3] == 1.0f);
    }
}

void testDownscale() {
    // Non-integer ratio: 3x1 -> 2x1, exact area weights (1, 1/2) and (1/2, 1).
    const std::vector<std::uint8_t> row = {0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255};
    const DecodedImage small = decodeImage(writeFile("row.png", encodePng(3, 1, 4, row)), {2});
    CHECK(small.width == 2 && small.height == 1);
    CHECK(small.sourceWidth == 3);
    const float mid = decode_detail::srgbToLinear8(128);
    CHECK(near(small.rgba[0], (0.0f + 0.5f * mid) / 1.5f, 1e-6f));
    CHECK(near(small.rgba[4], (0.5f * mid + 1.0f) / 1.5f, 1e-6f));
    CHECK(near(small.rgba[3], 1.0f, 1e-6f));

    // Checkerboard averages in linear light (0.5), not in sRGB (0.214).
    const int w = 1000, h = 600;
    std::vector<std::uint8_t> board(static_cast<std::size_t>(w * h * 3));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t v = (x + y) % 2 ? 255 : 0;
            std::memset(&board[static_cast<std::size_t>((y * w + x) * 3)], v, 3);
        }
    }
    const DecodedImage down = decodeImage(writeFile("board.png", encodePng(w, h, 3, board)), {100});
    CHECK(down.width == 100 && down.height == 60);
    float worst = 0.0f;
    for (std::size_t i = 0; i < down.rgba.size(); ++i) {
        worst = std::max(worst, std::fabs(down.rgba[i] - (i % 4 == 3 ? 1.0f : 0.5f)));
    }
    CHECK(worst < 1e-5f);

    // Images within the limit are untouched; the limit applies to the long edge.
    CHECK(decodeImage(writeFile("row.png", encodePng(3, 1, 4, row)), {3}).width == 3);
    const DecodedImage tall = decodeImage(writeFile("tall.png", encodePng(3, 9, 3, std::vector<std::uint8_t>(81, 77))), {4});
    CHECK(tall.width == 1 && tall.height == 4);
    CHECK(near(tall.rgba[0], decode_detail::srgbToLinear8(77), 1e-6f));

    // Downscaling happens before orientation: a rotated JPEG keeps its aspect.
    const Bytes jpeg = withExifOrientation(encodeJpeg(64, 32, halves(64, 32, kRed, kBlue)), 6, true);
    const DecodedImage rotated = decodeImage(writeFile("rotated_big.jpg", jpeg), {16});
    CHECK(rotated.width == 8 && rotated.height == 16);
}

void testErrors() {
    CHECK(decodeError(g_dir / "missing.jpg").find("missing.jpg: cannot read") == 0);
    CHECK(decodeError(writeFile("empty.png", {})).find("cannot read") != std::string::npos);
    Bytes heif{0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c', 0, 0, 0, 0};
    CHECK(decodeError(writeFile("photo.heic", heif)).find("is a HEIF image") != std::string::npos);
    Bytes cr3{0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'c', 'r', 'x', ' ', 0, 0, 0, 0};
    CHECK(decodeError(writeFile("photo.cr3", cr3)).find("Canon CR3") != std::string::npos);
    Bytes webp{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 0, 0};
    CHECK(decodeError(writeFile("photo.webp", webp)).find("WebP") != std::string::npos);
    const std::string garbage = decodeError(writeFile("garbage.jpg", {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'g'}));
    CHECK(garbage.find("garbage.jpg: cannot decode") == 0);
    // Truncated JPEG: fails cleanly or decodes, but never crashes.
    Bytes jpeg = encodeJpeg(32, 16, halves(32, 16, kRed, kBlue));
    jpeg.resize(jpeg.size() / 3);
    (void)decodeError(writeFile("truncated.jpg", jpeg));
}

void testConcurrentDecodes() {
    const fs::path path = writeFile("shared.jpg", encodeJpeg(128, 64, halves(128, 64, kRed, kBlue)));
    const DecodedImage reference = decodeImage(path, {50});
    std::vector<std::thread> threads;
    std::array<bool, 4> same{};
    for (std::size_t i = 0; i < same.size(); ++i) {
        threads.emplace_back([&, i] { same[i] = decodeImage(path, {50}).rgba == reference.rgba; });
    }
    for (auto& t : threads) t.join();
    CHECK(std::all_of(same.begin(), same.end(), [](bool b) { return b; }));
}

}  // namespace

int main(int argc, char** argv) {
    g_dir = fs::path(argc > 1 ? argv[1] : "image_decoder_test_work") / "image_decoder";
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir);

    try {
        testTransferFunction();
        testOrientation();
        testPng8();
        testPng16();
        testJpegAndExif();
        testUncompressedTiff();
        testRawPreviews();
        testHdr();
        testDownscale();
        testErrors();
        testConcurrentDecodes();
    } catch (const std::exception& e) {
        std::cout << "FAIL unexpected exception: " << e.what() << '\n';
        return 1;
    }
    if (g_failures) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "image decoder: all checks passed\n";
    return 0;
}
