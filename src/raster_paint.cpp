#include "raster_paint.hpp"

#include <algorithm>
#include <cmath>

namespace darkhouse {
namespace {

float smoothstep(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The pixel rectangle {x0, y0, x1, y1} (exclusive) a dab touches, clipped.
std::array<std::uint32_t, 4> dabRect(const SparseRasterLayer& layer, float x, float y, float radius) {
    const float r = radius + 1.0f;
    const auto lo = [](float v) { return static_cast<std::int64_t>(std::floor(v)); };
    const auto hi = [](float v) { return static_cast<std::int64_t>(std::ceil(v)); };
    const std::int64_t x0 = std::max<std::int64_t>(lo(x - r), 0), y0 = std::max<std::int64_t>(lo(y - r), 0);
    const std::int64_t x1 = std::min<std::int64_t>(hi(x + r), layer.width());
    const std::int64_t y1 = std::min<std::int64_t>(hi(y + r), layer.height());
    if (x0 >= x1 || y0 >= y1) return {0, 0, 0, 0};
    return {static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), static_cast<std::uint32_t>(x1),
            static_cast<std::uint32_t>(y1)};
}

// Calls fn(texel, px, py, coverage) for every pixel with coverage, tile by
// tile, and marks the rectangle dirty. Unallocated tiles are created only
// when `create` says so for that layer's default value.
template <class Fn>
void forEachDabTexel(SparseRasterLayer& layer, float x, float y, const BrushTip& tip, bool create, Fn&& fn) {
    const std::array<std::uint32_t, 4> rect = dabRect(layer, x, y, tip.radius);
    if (rect[0] == rect[2]) return;
    for (std::uint32_t ty = rect[1] / TILE_SIZE; ty <= (rect[3] - 1) / TILE_SIZE; ++ty) {
        for (std::uint32_t tx = rect[0] / TILE_SIZE; tx <= (rect[2] - 1) / TILE_SIZE; ++tx) {
            PixelTile* tile = layer.getTile({tx, ty});
            if (!tile) {
                if (!create) continue;
                tile = &layer.getOrCreateTile({tx, ty});
            }
            const std::uint32_t ox = tx * TILE_SIZE, oy = ty * TILE_SIZE;
            const std::uint32_t xa = std::max(rect[0], ox), xb = std::min(rect[2], ox + tile->width);
            const std::uint32_t ya = std::max(rect[1], oy), yb = std::min(rect[3], oy + tile->height);
            for (std::uint32_t py = ya; py < yb; ++py) {
                for (std::uint32_t px = xa; px < xb; ++px) {
                    const float d = std::hypot(static_cast<float>(px) + 0.5f - x, static_cast<float>(py) + 0.5f - y);
                    const float coverage = dabCoverage(tip, d) * std::clamp(tip.flow, 0.0f, 1.0f);
                    if (coverage <= 0.0f) continue;
                    fn(tile->data.data() + tile->index(px - ox, py - oy), px, py, coverage);
                }
            }
        }
    }
    layer.markDirtyRegion(rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]);
}

}  // namespace

float dabCoverage(const BrushTip& tip, float distance) noexcept {
    const float radius = std::max(tip.radius, 0.5f);
    const float hardness = std::clamp(tip.hardness, 0.0f, 1.0f);
    const float core = std::min(radius * hardness, radius - 1.0f);  // at least a pixel of anti-aliasing
    if (distance >= radius) return 0.0f;
    if (distance <= core) return 1.0f;
    return 1.0f - smoothstep(std::max(core, 0.0f), radius, distance);
}

std::vector<TileKey> dabTiles(const SparseRasterLayer& layer, float x, float y, float radius) {
    std::vector<TileKey> keys;
    const std::array<std::uint32_t, 4> rect = dabRect(layer, x, y, radius);
    if (rect[0] == rect[2]) return keys;
    for (std::uint32_t ty = rect[1] / TILE_SIZE; ty <= (rect[3] - 1) / TILE_SIZE; ++ty) {
        for (std::uint32_t tx = rect[0] / TILE_SIZE; tx <= (rect[2] - 1) / TILE_SIZE; ++tx) keys.push_back({tx, ty});
    }
    return keys;
}

void paintDab(SparseRasterLayer& layer, float x, float y, const BrushTip& tip, const std::array<float, 4>& color,
              DabMode mode) {
    if (layer.channels() == 1) {
        // Masks: towards the brush's grey level; erasing reveals again.
        const float target = mode == DabMode::ERASE
                                 ? 1.0f
                                 : std::clamp(0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2], 0.0f, 1.0f);
        const float strength = mode == DabMode::ERASE ? 1.0f : std::clamp(color[3], 0.0f, 1.0f);
        forEachDabTexel(layer, x, y, tip, layer.defaultValue() != target, [&](std::uint16_t* texel, std::uint32_t, std::uint32_t, float c) {
            const float v = halfToFloat(texel[0]);
            texel[0] = floatToHalf(v + (target - v) * c * strength);
        });
        return;
    }
    if (mode == DabMode::ERASE) {
        forEachDabTexel(layer, x, y, tip, layer.defaultValue() != 0.0f, [&](std::uint16_t* texel, std::uint32_t, std::uint32_t, float c) {
            if (layer.channels() >= 4) texel[3] = floatToHalf(halfToFloat(texel[3]) * (1.0f - c));
        });
        return;
    }
    const float alpha = std::clamp(color[3], 0.0f, 1.0f);
    forEachDabTexel(layer, x, y, tip, true, [&](std::uint16_t* texel, std::uint32_t, std::uint32_t, float c) {
        const float a = c * alpha;
        const float dstA = layer.channels() >= 4 ? std::clamp(halfToFloat(texel[3]), 0.0f, 1.0f) : 1.0f;
        const float outA = a + dstA * (1.0f - a);
        for (std::uint32_t k = 0; k < std::min<std::uint32_t>(layer.channels(), 3); ++k) {
            const float dst = halfToFloat(texel[k]);
            texel[k] = floatToHalf(outA > 0.0f ? (color[k] * a + dst * dstA * (1.0f - a)) / outA : 0.0f);
        }
        if (layer.channels() >= 4) texel[3] = floatToHalf(outA);
    });
}

void cloneDab(SparseRasterLayer& layer, const SparseRasterLayer& source, float x, float y, float dx, float dy,
              const BrushTip& tip) {
    const std::uint32_t channels = layer.channels();
    if (source.channels() != channels) return;
    // Read the whole source footprint first: the source may be this layer.
    const std::array<std::uint32_t, 4> rect = dabRect(layer, x, y, tip.radius);
    if (rect[0] == rect[2]) return;
    const std::uint32_t w = rect[2] - rect[0], h = rect[3] - rect[1];
    std::vector<float> copied(std::size_t{w} * h * channels);
    std::vector<float> texel(channels);
    const int ox = static_cast<int>(std::lround(dx)), oy = static_cast<int>(std::lround(dy));
    for (std::uint32_t py = 0; py < h; ++py) {
        for (std::uint32_t px = 0; px < w; ++px) {
            const std::int64_t sx = std::int64_t{rect[0] + px} + ox, sy = std::int64_t{rect[1] + py} + oy;
            const bool inside = sx >= 0 && sy >= 0 && sx < source.width() && sy < source.height();
            if (inside) {
                source.readPixel(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy), texel);
            } else {
                std::fill(texel.begin(), texel.end(), source.defaultValue());
            }
            std::copy(texel.begin(), texel.end(), copied.begin() + static_cast<std::ptrdiff_t>((std::size_t{py} * w + px) * channels));
        }
    }
    forEachDabTexel(layer, x, y, tip, true, [&](std::uint16_t* dst, std::uint32_t px, std::uint32_t py, float c) {
        const float* src = &copied[(std::size_t{py - rect[1]} * w + (px - rect[0])) * channels];
        if (channels < 4) {
            for (std::uint32_t k = 0; k < channels; ++k) {
                const float v = halfToFloat(dst[k]);
                dst[k] = floatToHalf(v + (src[k] - v) * c);
            }
            return;
        }
        // Colour: the copied pixel over the destination, faded by coverage.
        const float a = std::clamp(src[3], 0.0f, 1.0f) * c;
        const float dstA = std::clamp(halfToFloat(dst[3]), 0.0f, 1.0f);
        const float outA = a + dstA * (1.0f - a);
        for (std::uint32_t k = 0; k < 3; ++k) {
            const float d = halfToFloat(dst[k]);
            dst[k] = floatToHalf(outA > 0.0f ? (src[k] * a + d * dstA * (1.0f - a)) / outA : 0.0f);
        }
        dst[3] = floatToHalf(outA);
    });
}

void TileSnapshot::capture(std::span<const TileKey> keys) {
    for (const TileKey& key : keys) {
        if (saved_.count(key)) continue;
        const PixelTile* tile = layer_->getTile(key);
        saved_.emplace(key, tile ? std::optional<std::vector<std::uint16_t>>(tile->data) : std::nullopt);
    }
}

void TileSnapshot::restore() {
    for (const auto& [key, data] : saved_) {
        if (!data) {
            if (layer_->getTile(key)) layer_->releaseTile(key);
            continue;
        }
        PixelTile& tile = layer_->getOrCreateTile(key);
        tile.data = *data;
        layer_->markDirtyRegion(key.tx * TILE_SIZE, key.ty * TILE_SIZE, tile.width, tile.height);
    }
}

}  // namespace darkhouse
