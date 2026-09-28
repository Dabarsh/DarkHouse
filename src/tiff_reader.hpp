// DarkHouse — minimal random-access TIFF/EXIF reader (internal).
//
// Walks IFD chains of TIFF-based files: plain TIFF, DNG and most camera RAW
// containers (CR2, NEF, ARW, ORF, RW2, PEF...), and the TIFF structure inside
// a JPEG's EXIF APP1 segment. Shared by the catalog importer (metadata) and
// the image decoder (orientation, embedded JPEG previews). Every read is
// bounds-checked against the file, so corrupt files fail softly.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace darkhouse::detail {

class RandomAccessFile {
public:
    explicit RandomAccessFile(const std::filesystem::path& path) : in_(path, std::ios::binary) {
        if (in_) {
            in_.seekg(0, std::ios::end);
            size_ = static_cast<std::uint64_t>(std::max<std::streamoff>(0, in_.tellg()));
        }
    }

    bool read(std::uint64_t offset, void* dst, std::size_t count) {
        if (!in_.is_open() || offset > size_ || count > size_ - offset) return false;
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(offset));
        in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(count));
        return in_.gcount() == static_cast<std::streamsize>(count);
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

private:
    std::ifstream in_;
    std::uint64_t size_ = 0;
};

inline std::uint16_t readBE16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
inline std::uint32_t readBE32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

class TiffReader {
public:
    struct Entry {
        std::uint16_t tag = 0;
        std::uint16_t type = 0;
        std::uint32_t count = 0;
        std::array<std::uint8_t, 4> field{};  // inline value, or offset to the value
    };

    TiffReader(RandomAccessFile& file, std::uint64_t base) : file_(file), base_(base) {}

    bool parseHeader() {
        std::uint8_t header[8];
        if (!file_.read(base_, header, sizeof header)) return false;
        if (header[0] == 'I' && header[1] == 'I') {
            littleEndian_ = true;
        } else if (header[0] == 'M' && header[1] == 'M') {
            littleEndian_ = false;
        } else {
            return false;
        }
        const std::uint16_t magic = u16(header + 2);
        // 42 = TIFF/DNG/CR2/NEF/ARW..., 0x4F52/0x5352 = Olympus ORF, 0x55 = Panasonic RW2.
        if (magic != 42 && magic != 0x4F52 && magic != 0x5352 && magic != 0x55) return false;
        firstIfd_ = u32(header + 4);
        return true;
    }

    [[nodiscard]] std::uint32_t firstIfdOffset() const noexcept { return firstIfd_; }

    bool readIfd(std::uint32_t offset, std::vector<Entry>& entries, std::uint32_t& nextIfd) {
        std::uint8_t countBytes[2];
        if (offset == 0 || !file_.read(base_ + offset, countBytes, 2)) return false;
        const std::uint16_t count = u16(countBytes);
        if (count == 0 || count > 1024) return false;  // malformed or not an IFD
        std::vector<std::uint8_t> raw(std::size_t{count} * 12 + 4);
        if (!file_.read(base_ + offset + 2, raw.data(), raw.size() - 4)) return false;
        std::uint8_t next[4] = {0, 0, 0, 0};
        file_.read(base_ + offset + 2 + std::uint64_t{count} * 12, next, 4);  // optional
        nextIfd = u32(next);

        entries.clear();
        entries.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* p = raw.data() + i * 12;
            Entry e;
            e.tag = u16(p);
            e.type = u16(p + 2);
            e.count = u32(p + 4);
            std::memcpy(e.field.data(), p + 8, 4);
            entries.push_back(e);
        }
        return true;
    }

    std::optional<std::uint32_t> readUInt(const Entry& e, std::uint32_t index = 0) {
        std::uint8_t buf[4];
        switch (e.type) {
        case 1:  // BYTE
        case 7:  // UNDEFINED
            if (!valueBytes(e, 1, index, buf)) return std::nullopt;
            return buf[0];
        case 3:  // SHORT
            if (!valueBytes(e, 2, index, buf)) return std::nullopt;
            return u16(buf);
        case 4:   // LONG
        case 13:  // IFD
            if (!valueBytes(e, 4, index, buf)) return std::nullopt;
            return u32(buf);
        default:
            return std::nullopt;
        }
    }

    std::optional<double> readRational(const Entry& e, std::uint32_t index = 0) {
        if (e.type != 5 && e.type != 10) return std::nullopt;  // RATIONAL / SRATIONAL
        std::uint8_t buf[8];
        if (!valueBytes(e, 8, index, buf)) return std::nullopt;
        const std::uint32_t num = u32(buf);
        const std::uint32_t den = u32(buf + 4);
        if (den == 0) return std::nullopt;
        if (e.type == 10) return static_cast<double>(static_cast<std::int32_t>(num)) / static_cast<std::int32_t>(den);
        return static_cast<double>(num) / den;
    }

    std::optional<std::string> readAscii(const Entry& e) {
        if (e.type != 2 || e.count == 0 || e.count > 512) return std::nullopt;
        std::string text(e.count, '\0');
        if (e.count <= 4) {
            std::memcpy(text.data(), e.field.data(), e.count);
        } else if (!file_.read(base_ + u32(e.field.data()), text.data(), e.count)) {
            return std::nullopt;
        }
        text.resize(std::strlen(text.c_str()));  // stop at the first NUL
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
        if (text.empty()) return std::nullopt;
        return text;
    }

private:
    bool valueBytes(const Entry& e, std::size_t elementSize, std::uint32_t index, std::uint8_t* dst) {
        if (index >= e.count) return false;
        const std::uint64_t total = std::uint64_t{e.count} * elementSize;
        const std::uint64_t at = std::uint64_t{index} * elementSize;
        if (total <= 4) {
            std::memcpy(dst, e.field.data() + at, elementSize);
            return true;
        }
        return file_.read(base_ + u32(e.field.data()) + at, dst, elementSize);
    }

    [[nodiscard]] std::uint16_t u16(const std::uint8_t* p) const {
        return littleEndian_ ? static_cast<std::uint16_t>(p[0] | (p[1] << 8)) : readBE16(p);
    }
    [[nodiscard]] std::uint32_t u32(const std::uint8_t* p) const {
        return littleEndian_ ? (static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24))
                             : readBE32(p);
    }

    RandomAccessFile& file_;
    std::uint64_t base_;
    bool littleEndian_ = true;
    std::uint32_t firstIfd_ = 0;
};

}  // namespace darkhouse::detail
