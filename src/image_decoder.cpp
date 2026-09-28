#include "image_decoder.hpp"

#include "tiff_reader.hpp"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace darkhouse {
namespace {

namespace fs = std::filesystem;
using detail::RandomAccessFile;
using detail::readBE16;
using detail::readBE32;
using detail::TiffReader;

float srgbDecode(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

const std::array<float, 256>& srgbTable8() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) t[i] = srgbDecode(static_cast<float>(i) / 255.0f);
        return t;
    }();
    return table;
}

const std::vector<float>& srgbTable16() {
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (std::size_t i = 0; i < t.size(); ++i) t[i] = srgbDecode(static_cast<float>(i) / 65535.0f);
        return t;
    }();
    return table;
}

std::string upperExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return ext.empty() ? std::string("image") : ext;
}

[[noreturn]] void fail(const fs::path& path, const std::string& why) {
    throw std::runtime_error(path.filename().string() + ": " + why);
}

// Pixels as they come out of a decoder, before linearisation and resampling.
struct SourcePixels {
    enum class Kind { U8, U16, F32 };
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Kind kind = Kind::U8;
    const void* data = nullptr;  // interleaved RGBA of `kind`

    void linear(std::uint32_t x, std::uint32_t y, float* out) const noexcept {
        const std::size_t i = (std::size_t{y} * width + x) * 4;
        switch (kind) {
        case Kind::U8: {
            const auto* p = static_cast<const std::uint8_t*>(data) + i;
            const auto& lut = srgbTable8();
            out[0] = lut[p[0]];
            out[1] = lut[p[1]];
            out[2] = lut[p[2]];
            out[3] = static_cast<float>(p[3]) / 255.0f;
            break;
        }
        case Kind::U16: {
            const auto* p = static_cast<const std::uint16_t*>(data) + i;
            const auto& lut = srgbTable16();
            out[0] = lut[p[0]];
            out[1] = lut[p[1]];
            out[2] = lut[p[2]];
            out[3] = static_cast<float>(p[3]) / 65535.0f;
            break;
        }
        case Kind::F32: {
            const auto* p = static_cast<const float*>(data) + i;
            out[0] = std::max(p[0], 0.0f);
            out[1] = std::max(p[1], 0.0f);
            out[2] = std::max(p[2], 0.0f);
            out[3] = p[3];
            break;
        }
        }
    }
};

// Runs fn(begin, end) over [0, count) split across hardware threads.
template <class F>
void parallelRows(std::uint32_t count, F fn) {
    const std::uint32_t threads =
        std::clamp<std::uint32_t>(std::thread::hardware_concurrency(), 1, 8);
    if (threads == 1 || count < 64) {
        fn(0u, count);
        return;
    }
    std::vector<std::thread> pool;
    const std::uint32_t chunk = (count + threads - 1) / threads;
    for (std::uint32_t begin = 0; begin < count; begin += chunk) {
        pool.emplace_back(fn, begin, std::min(count, begin + chunk));
    }
    for (std::thread& t : pool) t.join();
}

// Area-averaging resample to outWidth x outHeight in linear light (each
// output pixel is the exact mean of the source area it covers). With equal
// sizes it is a plain conversion.
std::vector<float> toLinearResampled(const SourcePixels& src, std::uint32_t outWidth, std::uint32_t outHeight) {
    std::vector<float> out(std::size_t{outWidth} * outHeight * 4);
    if (outWidth == src.width && outHeight == src.height) {
        parallelRows(outHeight, [&](std::uint32_t begin, std::uint32_t end) {
            for (std::uint32_t y = begin; y < end; ++y) {
                for (std::uint32_t x = 0; x < outWidth; ++x) src.linear(x, y, &out[(std::size_t{y} * outWidth + x) * 4]);
            }
        });
        return out;
    }

    // Column footprints: (first source column, weights...) per output column.
    const double sx = static_cast<double>(src.width) / outWidth;
    const double sy = static_cast<double>(src.height) / outHeight;
    struct Span {
        std::uint32_t first = 0;
        std::vector<float> weights;
    };
    std::vector<Span> columns(outWidth);
    for (std::uint32_t ox = 0; ox < outWidth; ++ox) {
        const double x0 = ox * sx;
        const double x1 = std::min<double>((ox + 1) * sx, src.width);
        Span& span = columns[ox];
        span.first = static_cast<std::uint32_t>(x0);
        for (auto x = span.first; x < src.width && x < x1; ++x) {
            span.weights.push_back(static_cast<float>(std::min<double>(x1, x + 1.0) - std::max<double>(x0, x)));
        }
    }
    const float norm = static_cast<float>(1.0 / (sx * sy));

    parallelRows(outHeight, [&](std::uint32_t begin, std::uint32_t end) {
        std::vector<float> row(std::size_t{outWidth} * 4);
        float pixel[4];
        for (std::uint32_t oy = begin; oy < end; ++oy) {
            std::fill(row.begin(), row.end(), 0.0f);
            const double y0 = oy * sy;
            const double y1 = std::min<double>((oy + 1) * sy, src.height);
            for (auto y = static_cast<std::uint32_t>(y0); y < src.height && y < y1; ++y) {
                const float wy = static_cast<float>(std::min<double>(y1, y + 1.0) - std::max<double>(y0, y));
                for (std::uint32_t ox = 0; ox < outWidth; ++ox) {
                    const Span& span = columns[ox];
                    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    for (std::size_t k = 0; k < span.weights.size(); ++k) {
                        src.linear(span.first + static_cast<std::uint32_t>(k), y, pixel);
                        const float w = span.weights[k];
                        acc[0] += w * pixel[0];
                        acc[1] += w * pixel[1];
                        acc[2] += w * pixel[2];
                        acc[3] += w * pixel[3];
                    }
                    float* r = &row[std::size_t{ox} * 4];
                    r[0] += wy * acc[0];
                    r[1] += wy * acc[1];
                    r[2] += wy * acc[2];
                    r[3] += wy * acc[3];
                }
            }
            float* dst = &out[std::size_t{oy} * outWidth * 4];
            for (std::size_t i = 0; i < row.size(); ++i) dst[i] = row[i] * norm;
        }
    });
    return out;
}

std::vector<std::uint8_t> readRange(RandomAccessFile& file, std::uint64_t offset, std::uint64_t length) {
    if (length == 0 || length > (std::uint64_t{1} << 31)) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (!file.read(offset, bytes.data(), bytes.size())) return {};
    return bytes;
}

// --- JPEG inspection -------------------------------------------------------------

struct JpegInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<std::uint64_t> exifTiffOffset;  // start of the TIFF header inside APP1
};

// Walks the markers of the JPEG at `offset`. Only Huffman-coded 8-bit
// baseline/extended/progressive frames are accepted: RAW files also hold
// lossless-JPEG sensor data (SOF3) that is not a picture.
std::optional<JpegInfo> inspectJpeg(RandomAccessFile& file, std::uint64_t offset, std::uint64_t length) {
    std::uint8_t soi[2];
    if (!file.read(offset, soi, 2) || soi[0] != 0xFF || soi[1] != 0xD8) return std::nullopt;
    JpegInfo info;
    std::uint64_t pos = offset + 2;
    const std::uint64_t end = std::min(file.size(), offset + length);
    while (pos + 4 <= end) {
        std::uint8_t header[4];
        if (!file.read(pos, header, sizeof header) || header[0] != 0xFF) return std::nullopt;
        const std::uint8_t marker = header[1];
        if (marker == 0xFF) {
            ++pos;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            pos += 2;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) return std::nullopt;  // no frame header before the scan
        const std::uint16_t size = readBE16(header + 2);
        if (size < 2) return std::nullopt;
        if (marker == 0xE1 && !info.exifTiffOffset && size >= 8) {
            std::uint8_t id[6];
            if (file.read(pos + 4, id, sizeof id) && std::memcmp(id, "Exif\0\0", 6) == 0) info.exifTiffOffset = pos + 10;
        }
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            std::uint8_t sof[5];
            if (!file.read(pos + 4, sof, sizeof sof) || sof[0] != 8) return std::nullopt;
            info.height = readBE16(sof + 1);
            info.width = readBE16(sof + 3);
            return info.width > 0 && info.height > 0 ? std::optional<JpegInfo>(info) : std::nullopt;
        }
        if (marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            return std::nullopt;  // lossless / arithmetic / hierarchical: not decodable here
        }
        pos += 2 + std::uint64_t{size};
    }
    return std::nullopt;
}

int orientationFromTiff(RandomAccessFile& file, std::uint64_t base) {
    TiffReader tiff(file, base);
    std::vector<TiffReader::Entry> entries;
    std::uint32_t next = 0;
    if (!tiff.parseHeader() || !tiff.readIfd(tiff.firstIfdOffset(), entries, next)) return 1;
    for (const auto& e : entries) {
        if (e.tag == 0x0112) {
            const std::uint32_t value = tiff.readUInt(e).value_or(1);
            return value >= 1 && value <= 8 ? static_cast<int>(value) : 1;
        }
    }
    return 1;
}

// --- Decoders ----------------------------------------------------------------------

struct StbDeleter {
    void operator()(void* p) const noexcept { stbi_image_free(p); }
};
using StbPixels = std::unique_ptr<void, StbDeleter>;

// Decodes an in-memory image with stb_image into 4-channel source pixels.
StbPixels decodeWithStb(const fs::path& path, const std::vector<std::uint8_t>& bytes, SourcePixels& pixels,
                        std::string& format) {
    const auto* data = bytes.data();
    const int size = static_cast<int>(bytes.size());
    int w = 0, h = 0, channels = 0;
    void* decoded = nullptr;
    if (stbi_is_hdr_from_memory(data, size)) {
        decoded = stbi_loadf_from_memory(data, size, &w, &h, &channels, 4);
        pixels.kind = SourcePixels::Kind::F32;
        format = "Radiance HDR";
    } else if (stbi_is_16_bit_from_memory(data, size)) {
        decoded = stbi_load_16_from_memory(data, size, &w, &h, &channels, 4);
        pixels.kind = SourcePixels::Kind::U16;
        format += " 16-bit";
    } else {
        decoded = stbi_load_from_memory(data, size, &w, &h, &channels, 4);
        pixels.kind = SourcePixels::Kind::U8;
    }
    if (!decoded) fail(path, std::string("cannot decode (") + stbi_failure_reason() + ")");
    pixels.width = static_cast<std::uint32_t>(w);
    pixels.height = static_cast<std::uint32_t>(h);
    pixels.data = decoded;
    return StbPixels(decoded);
}

struct TiffImage {
    std::optional<std::pair<std::uint64_t, std::uint64_t>> bestJpeg;  // offset, length
    std::uint64_t bestJpegArea = 0;
    // Uncompressed chunky RGB(A) main image, if any.
    std::uint32_t width = 0, height = 0, samples = 0, bits = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> strips;  // offset, byte count
    int orientation = 1;
};

TiffImage inspectTiff(RandomAccessFile& file) {
    TiffImage image;
    TiffReader tiff(file, 0);
    if (!tiff.parseHeader()) return image;

    std::vector<std::uint32_t> pending{tiff.firstIfdOffset()};
    std::vector<std::uint32_t> visited;
    std::vector<TiffReader::Entry> entries;
    bool first = true;
    while (!pending.empty() && visited.size() < 32) {
        const std::uint32_t offset = pending.back();
        pending.pop_back();
        if (offset == 0 || std::find(visited.begin(), visited.end(), offset) != visited.end()) continue;
        visited.push_back(offset);
        std::uint32_t next = 0;
        if (!tiff.readIfd(offset, entries, next)) continue;
        if (next) pending.push_back(next);

        std::uint32_t w = 0, h = 0, compression = 1, photometric = 0, samples = 1, bits = 8, planar = 1;
        std::optional<std::uint32_t> jpegOffset, jpegLength;
        std::vector<std::uint32_t> stripOffsets, stripCounts;
        for (const auto& e : entries) {
            switch (e.tag) {
            case 0x0100: w = tiff.readUInt(e).value_or(0); break;
            case 0x0101: h = tiff.readUInt(e).value_or(0); break;
            case 0x0102: bits = tiff.readUInt(e).value_or(8); break;
            case 0x0103: compression = tiff.readUInt(e).value_or(1); break;
            case 0x0106: photometric = tiff.readUInt(e).value_or(0); break;
            case 0x0112:
                if (first) image.orientation = static_cast<int>(std::clamp<std::uint32_t>(tiff.readUInt(e).value_or(1), 1, 8));
                break;
            case 0x0115: samples = tiff.readUInt(e).value_or(1); break;
            case 0x011C: planar = tiff.readUInt(e).value_or(1); break;
            case 0x0111:
                for (std::uint32_t i = 0; i < std::min<std::uint32_t>(e.count, 65536); ++i) stripOffsets.push_back(tiff.readUInt(e, i).value_or(0));
                break;
            case 0x0117:
                for (std::uint32_t i = 0; i < std::min<std::uint32_t>(e.count, 65536); ++i) stripCounts.push_back(tiff.readUInt(e, i).value_or(0));
                break;
            case 0x0201: jpegOffset = tiff.readUInt(e); break;
            case 0x0202: jpegLength = tiff.readUInt(e); break;
            case 0x014A:
                for (std::uint32_t i = 0; i < std::min<std::uint32_t>(e.count, 8); ++i) {
                    if (auto sub = tiff.readUInt(e, i)) pending.push_back(*sub);
                }
                break;
            default: break;
            }
        }
        first = false;

        auto considerJpeg = [&](std::uint64_t at, std::uint64_t length) {
            if (auto info = inspectJpeg(file, at, length)) {
                const std::uint64_t area = std::uint64_t{info->width} * info->height;
                if (area > image.bestJpegArea) {
                    image.bestJpegArea = area;
                    image.bestJpeg = std::make_pair(at, length);
                }
            }
        };
        if (jpegOffset && jpegLength) considerJpeg(*jpegOffset, *jpegLength);
        if ((compression == 6 || compression == 7) && stripOffsets.size() == 1 && stripCounts.size() == 1) {
            considerJpeg(stripOffsets[0], stripCounts[0]);
        }
        const bool plainRgb = compression == 1 && photometric == 2 && planar == 1 && (samples == 3 || samples == 4) &&
                              (bits == 8 || bits == 16) && !stripOffsets.empty() &&
                              stripOffsets.size() == stripCounts.size();
        if (plainRgb && std::uint64_t{w} * h > std::uint64_t{image.width} * image.height) {
            image.width = w;
            image.height = h;
            image.samples = samples;
            image.bits = bits;
            image.strips.clear();
            for (std::size_t i = 0; i < stripOffsets.size(); ++i) image.strips.emplace_back(stripOffsets[i], stripCounts[i]);
        }
    }
    return image;
}

// Reads an uncompressed chunky RGB(A) TIFF image into 4-channel buffers.
bool readUncompressedTiff(RandomAccessFile& file, const TiffImage& info, std::vector<std::uint8_t>& rgba8,
                          std::vector<std::uint16_t>& rgba16) {
    std::uint8_t header[2];
    if (!file.read(0, header, 2)) return false;
    const bool littleEndian = header[0] == 'I';
    const std::size_t bytesPerSample = info.bits / 8;
    const std::size_t pixelBytes = bytesPerSample * info.samples;
    const std::size_t pixels = std::size_t{info.width} * info.height;
    std::vector<std::uint8_t> raw;
    raw.reserve(pixels * pixelBytes);
    for (const auto& [offset, count] : info.strips) {
        const std::vector<std::uint8_t> strip = readRange(file, offset, count);
        if (strip.empty()) return false;
        raw.insert(raw.end(), strip.begin(), strip.end());
    }
    if (raw.size() < pixels * pixelBytes) return false;
    if (info.bits == 8) {
        rgba8.resize(pixels * 4);
        for (std::size_t i = 0; i < pixels; ++i) {
            for (std::size_t c = 0; c < 3; ++c) rgba8[i * 4 + c] = raw[i * pixelBytes + c];
            rgba8[i * 4 + 3] = info.samples == 4 ? raw[i * pixelBytes + 3] : 255;
        }
    } else {
        rgba16.resize(pixels * 4);
        auto sample = [&](std::size_t at) {
            const std::uint8_t* p = &raw[at];
            return littleEndian ? static_cast<std::uint16_t>(p[0] | (p[1] << 8)) : readBE16(p);
        };
        for (std::size_t i = 0; i < pixels; ++i) {
            for (std::size_t c = 0; c < 3; ++c) rgba16[i * 4 + c] = sample(i * pixelBytes + c * 2);
            rgba16[i * 4 + 3] = info.samples == 4 ? sample(i * pixelBytes + 6) : 65535;
        }
    }
    return true;
}

std::optional<std::string> unsupportedKind(const std::uint8_t* magic, std::size_t size) {
    if (size >= 12 && std::memcmp(magic + 4, "ftyp", 4) == 0) {
        const std::string brand(reinterpret_cast<const char*>(magic + 8), 4);
        if (brand == "crx ") return "a Canon CR3 file";
        if (brand == "avif" || brand == "avis") return "an AVIF image";
        return "a HEIF image";
    }
    if (size >= 12 && std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "WEBP", 4) == 0) return "a WebP image";
    if (size >= 4 && magic[0] == 0x76 && magic[1] == 0x2F && magic[2] == 0x31 && magic[3] == 0x01) return "an OpenEXR image";
    return std::nullopt;
}

}  // namespace

namespace decode_detail {

float srgbToLinear8(std::uint8_t value) noexcept { return srgbTable8()[value]; }
float srgbToLinear16(std::uint16_t value) noexcept { return srgbTable16()[value]; }

void applyOrientation(int orientation, std::uint32_t& width, std::uint32_t& height, std::vector<float>& rgba) {
    if (orientation <= 1 || orientation > 8) return;
    const std::uint32_t w = width;
    const std::uint32_t h = height;
    const bool swap = orientation >= 5;
    const std::uint32_t outW = swap ? h : w;
    const std::uint32_t outH = swap ? w : h;
    std::vector<float> out(rgba.size());
    for (std::uint32_t y = 0; y < outH; ++y) {
        for (std::uint32_t x = 0; x < outW; ++x) {
            std::uint32_t sx = x, sy = y;
            switch (orientation) {
            case 2: sx = w - 1 - x; sy = y; break;          // mirror horizontal
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;  // rotate 180
            case 4: sx = x; sy = h - 1 - y; break;          // mirror vertical
            case 5: sx = y; sy = x; break;                  // transpose
            case 6: sx = y; sy = h - 1 - x; break;          // rotate 90 clockwise
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;  // transverse
            case 8: sx = w - 1 - y; sy = x; break;          // rotate 90 counter-clockwise
            default: break;
            }
            std::memcpy(&out[(std::size_t{y} * outW + x) * 4], &rgba[(std::size_t{sy} * w + sx) * 4], 4 * sizeof(float));
        }
    }
    rgba = std::move(out);
    width = outW;
    height = outH;
}

}  // namespace decode_detail

DecodedImage decodeImage(const fs::path& path, const DecodeOptions& options) {
    RandomAccessFile file(path);
    if (file.size() == 0) fail(path, "cannot read the file (missing, empty or unreadable)");
    std::uint8_t magic[32] = {};
    const std::size_t magicSize = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof magic, file.size()));
    file.read(0, magic, magicSize);
    if (auto kind = unsupportedKind(magic, magicSize)) fail(path, "is " + *kind + ", which DarkHouse cannot decode yet");

    DecodedImage result;
    SourcePixels pixels;
    StbPixels stbPixels;
    std::vector<std::uint8_t> tiff8;
    std::vector<std::uint16_t> tiff16;
    const std::string extension = upperExtension(path);

    const bool isTiff = (magic[0] == 'I' && magic[1] == 'I') || (magic[0] == 'M' && magic[1] == 'M');
    const bool isRaf = magicSize >= 16 && std::memcmp(magic, "FUJIFILMCCD-RAW", 15) == 0;
    if (isTiff || isRaf) {
        std::optional<std::pair<std::uint64_t, std::uint64_t>> jpeg;
        TiffImage tiffInfo;
        if (isRaf) {
            std::uint8_t header[8];
            if (file.read(84, header, sizeof header)) jpeg = std::make_pair(readBE32(header), readBE32(header + 4));
        } else {
            tiffInfo = inspectTiff(file);
            result.orientation = tiffInfo.orientation;
            jpeg = tiffInfo.bestJpeg;
        }
        const std::uint64_t plainArea = std::uint64_t{tiffInfo.width} * tiffInfo.height;
        if (plainArea > 0 && plainArea >= tiffInfo.bestJpegArea) {
            if (!readUncompressedTiff(file, tiffInfo, tiff8, tiff16)) fail(path, "the TIFF image data is truncated");
            pixels.width = tiffInfo.width;
            pixels.height = tiffInfo.height;
            pixels.kind = tiff16.empty() ? SourcePixels::Kind::U8 : SourcePixels::Kind::U16;
            pixels.data = tiff16.empty() ? static_cast<const void*>(tiff8.data()) : tiff16.data();
            result.format = tiff16.empty() ? "TIFF" : "TIFF 16-bit";
        } else if (jpeg) {
            const std::vector<std::uint8_t> bytes = readRange(file, jpeg->first, jpeg->second);
            if (bytes.empty()) fail(path, "the embedded preview is truncated");
            if (isRaf) {  // RAF has no TIFF IFD0: the preview's own EXIF carries the orientation
                if (auto info = inspectJpeg(file, jpeg->first, jpeg->second); info && info->exifTiffOffset) {
                    result.orientation = orientationFromTiff(file, *info->exifTiffOffset);
                }
            }
            std::string ignored;
            stbPixels = decodeWithStb(path, bytes, pixels, ignored);
            result.format = extension + " embedded JPEG preview";
        } else {
            fail(path, "contains no preview DarkHouse can decode yet (RAW demosaicing is not implemented)");
        }
    } else {
        const std::vector<std::uint8_t> bytes = readRange(file, 0, file.size());
        if (bytes.empty()) fail(path, "cannot read the file");
        if (magic[0] == 0xFF && magic[1] == 0xD8) {
            result.format = "JPEG";
            if (auto info = inspectJpeg(file, 0, file.size()); info && info->exifTiffOffset) {
                result.orientation = orientationFromTiff(file, *info->exifTiffOffset);
            }
        } else if (magic[0] == 0x89 && magic[1] == 'P') {
            result.format = "PNG";
        } else {
            result.format = extension;
        }
        stbPixels = decodeWithStb(path, bytes, pixels, result.format);
    }

    result.sourceWidth = pixels.width;
    result.sourceHeight = pixels.height;
    std::uint32_t width = pixels.width;
    std::uint32_t height = pixels.height;
    const std::uint32_t longest = std::max(width, height);
    if (options.maxDimension > 0 && longest > options.maxDimension) {
        const double scale = static_cast<double>(options.maxDimension) / longest;
        width = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(width * scale)), 1, options.maxDimension);
        height = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(height * scale)), 1, options.maxDimension);
    }
    result.rgba = toLinearResampled(pixels, width, height);
    decode_detail::applyOrientation(result.orientation, width, height, result.rgba);
    result.width = width;
    result.height = height;
    return result;
}

}  // namespace darkhouse
