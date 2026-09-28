#include "layer_stack.hpp"

#include "adjustment_ops.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define DARKHOUSE_X86 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define DARKHOUSE_TARGET_F16C
#else
#define DARKHOUSE_TARGET_F16C __attribute__((target("avx,f16c")))
#endif
#endif

namespace darkhouse {
namespace {

#if DARKHOUSE_X86
// F16C converts 8 values per instruction, rounding to nearest even like
// floatToHalf. Compiled for F16C here and only called after the CPU check.
DARKHOUSE_TARGET_F16C void floatsToHalvesF16C(const float* in, std::uint16_t* out, std::size_t count) noexcept {
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m128i halves = _mm256_cvtps_ph(_mm256_loadu_ps(in + i), _MM_FROUND_TO_NEAREST_INT);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), halves);
    }
    for (; i < count; ++i) out[i] = floatToHalf(in[i]);
}

DARKHOUSE_TARGET_F16C void halvesToFloatsF16C(const std::uint16_t* in, float* out, std::size_t count) noexcept {
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        _mm256_storeu_ps(out + i, _mm256_cvtph_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i))));
    }
    for (; i < count; ++i) out[i] = halfToFloat(in[i]);
}

bool cpuHasF16C() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    int info[4];
    __cpuid(info, 1);
    const bool avx = (info[2] & (1 << 28)) != 0, osxsave = (info[2] & (1 << 27)) != 0, f16c = (info[2] & (1 << 29)) != 0;
    return avx && osxsave && f16c && (_xgetbv(0) & 6) == 6;  // the OS saves the YMM registers
#else
    return __builtin_cpu_supports("avx") && __builtin_cpu_supports("f16c");
#endif
}

const bool kHasF16C = cpuHasF16C();
#endif

float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

// Clips [origin, origin + extent) against [0, limit). The math is 64-bit so
// huge extents cannot overflow. Returns the exclusive end.
std::uint32_t clippedEnd(std::uint32_t origin, std::uint32_t extent, std::uint32_t limit) noexcept {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(std::uint64_t{origin} + extent, limit));
}

bool lessRowMajor(const TileKey& a, const TileKey& b) noexcept {
    return a.ty != b.ty ? a.ty < b.ty : a.tx < b.tx;
}

// Source-over with a blend mode (W3C Compositing Level 1 section 5.2) on
// straight-alpha RGBA. `sourceAlpha` already includes opacity and mask.
void blendPixel(float* dst, const float* src, float sourceAlpha, BlendMode mode) noexcept {
    if (!(sourceAlpha > 0.0f)) return;
    const float backdropAlpha = dst[3];
    const float outAlpha = sourceAlpha + backdropAlpha * (1.0f - sourceAlpha);
    const std::array<float, 3> blended =
        mode == BlendMode::NORMAL ? std::array<float, 3>{src[0], src[1], src[2]}
                                  : blendColor(mode, {dst[0], dst[1], dst[2]}, {src[0], src[1], src[2]});
    for (int c = 0; c < 3; ++c) {
        const float cb = dst[c];
        const float cs = src[c];
        const float mixed = (1.0f - backdropAlpha) * cs + backdropAlpha * blended[static_cast<std::size_t>(c)];
        const float premultiplied = sourceAlpha * mixed + backdropAlpha * cb * (1.0f - sourceAlpha);
        dst[c] = outAlpha > 0.0f ? premultiplied / outAlpha : 0.0f;
    }
    dst[3] = outAlpha;
}

// --- Transformed content ---------------------------------------------------------------

// The canvas rectangle {x0, y0, x1, y1} covered by layer rectangle `r` under `m`.
std::array<float, 4> mappedBounds(const Affine& m, const std::array<float, 4>& r) noexcept {
    std::array<float, 4> out{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const auto& [x, y] : {std::pair{r[0], r[1]}, std::pair{r[2], r[1]}, std::pair{r[0], r[3]}, std::pair{r[2], r[3]}}) {
        const std::array<float, 2> p = applyAffine(m, x, y);
        out[0] = std::min(out[0], p[0]);
        out[1] = std::min(out[1], p[1]);
        out[2] = std::max(out[2], p[0]);
        out[3] = std::max(out[3], p[1]);
    }
    return out;
}

bool overlapsTile(const std::array<float, 4>& bounds, TileKey key, std::uint32_t tw, std::uint32_t th) noexcept {
    const float x0 = static_cast<float>(key.tx * TILE_SIZE), y0 = static_cast<float>(key.ty * TILE_SIZE);
    return bounds[2] > x0 && bounds[0] < x0 + static_cast<float>(tw) && bounds[3] > y0 && bounds[1] < y0 + static_cast<float>(th);
}

// One texel of a colour layer (straight alpha), transparent outside it.
struct TexelReader {
    const SparseRasterLayer& layer;
    TileKey cachedKey{~0u, ~0u};
    const PixelTile* cachedTile = nullptr;

    void read(int x, int y, float* rgba) {
        if (x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= layer.width() || static_cast<std::uint32_t>(y) >= layer.height()) {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
            return;
        }
        const TileKey key = SparseRasterLayer::tileKeyFor(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        if (!(key == cachedKey)) {
            cachedKey = key;
            cachedTile = layer.getTile(key);
        }
        if (!cachedTile) {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = layer.defaultValue();
            return;
        }
        const std::uint16_t* texel = cachedTile->data.data() +
                                     cachedTile->index(static_cast<std::uint32_t>(x) % TILE_SIZE, static_cast<std::uint32_t>(y) % TILE_SIZE);
        for (std::uint32_t c = 0; c < 4; ++c) {
            const float v = c < cachedTile->channels ? halfToFloat(texel[c]) : 1.0f;
            rgba[c] = std::isfinite(v) ? v : 0.0f;
        }
    }
};

// Resamples a colour layer placed with `m` into the tile: bilinear on
// premultiplied colour, so transparent neighbours do not darken edges.
// Returns false when the layer does not reach the tile.
bool sampleTransformed(const SparseRasterLayer& layer, const Affine& m, TileKey key, std::uint32_t tw, std::uint32_t th,
                       std::vector<float>& out) {
    const std::optional<Affine> inverse = invertAffine(m);
    if (!inverse) return false;
    const std::array<float, 4> bounds =
        mappedBounds(m, {0.0f, 0.0f, static_cast<float>(layer.width()), static_cast<float>(layer.height())});
    if (!overlapsTile({bounds[0] - 1.0f, bounds[1] - 1.0f, bounds[2] + 1.0f, bounds[3] + 1.0f}, key, tw, th)) return false;
    out.assign(std::size_t{tw} * th * 4, 0.0f);
    TexelReader reader{layer};
    const float originX = static_cast<float>(key.tx * TILE_SIZE), originY = static_cast<float>(key.ty * TILE_SIZE);
    for (std::uint32_t y = 0; y < th; ++y) {
        for (std::uint32_t x = 0; x < tw; ++x) {
            const std::array<float, 2> l =
                applyAffine(*inverse, originX + static_cast<float>(x) + 0.5f, originY + static_cast<float>(y) + 0.5f);
            const float fx = l[0] - 0.5f, fy = l[1] - 0.5f;
            if (fx < -1.0f || fy < -1.0f || fx > static_cast<float>(layer.width()) || fy > static_cast<float>(layer.height())) continue;
            const float bx = std::floor(fx), by = std::floor(fy);
            const float tx = fx - bx, ty = fy - by;
            const int ix = static_cast<int>(bx), iy = static_cast<int>(by);
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const float weights[4] = {(1.0f - tx) * (1.0f - ty), tx * (1.0f - ty), (1.0f - tx) * ty, tx * ty};
            const int offsets[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
            for (int i = 0; i < 4; ++i) {
                float texel[4];
                reader.read(ix + offsets[i][0], iy + offsets[i][1], texel);
                const float a = clamp01(texel[3]) * weights[i];
                sum[0] += texel[0] * a;
                sum[1] += texel[1] * a;
                sum[2] += texel[2] * a;
                sum[3] += a;
            }
            float* pixel = &out[(std::size_t{y} * tw + x) * 4];
            if (sum[3] > 0.0f) {
                pixel[0] = sum[0] / sum[3];
                pixel[1] = sum[1] / sum[3];
                pixel[2] = sum[2] / sum[3];
                pixel[3] = std::min(sum[3], 1.0f);
            }
        }
    }
    return true;
}

// --- Vector shapes -----------------------------------------------------------------------

using Polygon = std::vector<std::array<float, 2>>;

float signedArea(const Polygon& polygon) noexcept {
    float area = 0.0f;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        area += polygon[j][0] * polygon[i][1] - polygon[i][0] * polygon[j][1];
    }
    return 0.5f * area;
}

// Anti-aliased coverage of `polygons` (nonzero winding) over a tile whose
// top-left canvas pixel is (originX, originY): four sample rows per pixel,
// exact span coverage along each row.
void rasterizeCoverage(const std::vector<Polygon>& polygons, float originX, float originY, std::uint32_t tw,
                       std::uint32_t th, std::vector<float>& coverage) {
    struct Edge {
        float x0, y0, x1, y1;
        int direction;
    };
    std::vector<Edge> edges;
    const float width = static_cast<float>(tw), height = static_cast<float>(th);
    for (const Polygon& polygon : polygons) {
        for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
            Edge e{polygon[j][0] - originX, polygon[j][1] - originY, polygon[i][0] - originX, polygon[i][1] - originY, 1};
            if (e.y0 == e.y1) continue;
            if (e.y0 > e.y1) {
                std::swap(e.x0, e.x1);
                std::swap(e.y0, e.y1);
                e.direction = -1;
            }
            if (e.y1 <= 0.0f || e.y0 >= height || std::min(e.x0, e.x1) >= width) continue;
            edges.push_back(e);
        }
    }
    coverage.assign(std::size_t{tw} * th, 0.0f);
    if (edges.empty()) return;
    constexpr int kRows = 4;
    std::vector<std::pair<float, int>> crossings;
    for (std::uint32_t py = 0; py < th; ++py) {
        for (int sub = 0; sub < kRows; ++sub) {
            const float y = static_cast<float>(py) + (static_cast<float>(sub) + 0.5f) / kRows;
            crossings.clear();
            for (const Edge& e : edges) {
                if (y < e.y0 || y >= e.y1) continue;
                crossings.emplace_back(e.x0 + (y - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.direction);
            }
            if (crossings.size() < 2) continue;
            std::sort(crossings.begin(), crossings.end());
            float* row = &coverage[std::size_t{py} * tw];
            int winding = 0;
            float spanStart = 0.0f;
            for (const auto& [x, direction] : crossings) {
                const int before = winding;
                winding += direction;
                if (before == 0 && winding != 0) spanStart = x;
                if (before != 0 && winding == 0) {
                    const float a = std::max(spanStart, 0.0f), b = std::min(x, width);
                    if (b <= a) continue;
                    const auto first = static_cast<std::uint32_t>(a);
                    const auto last = std::min(static_cast<std::uint32_t>(std::ceil(b)), tw);
                    for (std::uint32_t px = first; px < last; ++px) {
                        const float overlap = std::min(b, static_cast<float>(px + 1)) - std::max(a, static_cast<float>(px));
                        if (overlap > 0.0f) row[px] += overlap / kRows;
                    }
                }
            }
        }
    }
    for (float& c : coverage) c = std::min(c, 1.0f);
}

// A round-capped, round-joined stroke of the polylines as a union of
// same-orientation polygons (nonzero winding adds them up).
std::vector<Polygon> strokePolygons(const std::vector<Polyline>& lines, float halfWidth) {
    std::vector<Polygon> out;
    constexpr int kSegments = 16;
    auto orient = [&](Polygon polygon) {
        if (signedArea(polygon) < 0.0f) std::reverse(polygon.begin(), polygon.end());
        out.push_back(std::move(polygon));
    };
    for (const Polyline& line : lines) {
        const std::size_t n = line.points.size();
        for (std::size_t i = 0; i < n; ++i) {
            const auto& p = line.points[i];
            Polygon disc;
            for (int k = 0; k < kSegments; ++k) {
                const float angle = static_cast<float>(k) / kSegments * 6.2831853f;
                disc.push_back({p[0] + halfWidth * std::cos(angle), p[1] + halfWidth * std::sin(angle)});
            }
            orient(std::move(disc));
            if (i + 1 == n && !line.closed) break;
            const auto& q = line.points[(i + 1) % n];
            const float dx = q[0] - p[0], dy = q[1] - p[1];
            const float length = std::hypot(dx, dy);
            if (length < 1e-6f) continue;
            const float nx = -dy / length * halfWidth, ny = dx / length * halfWidth;
            orient({{p[0] + nx, p[1] + ny}, {q[0] + nx, q[1] + ny}, {q[0] - nx, q[1] - ny}, {p[0] - nx, p[1] - ny}});
        }
    }
    return out;
}

// Fills and strokes a vector shape placed with `m` into the tile. Returns
// false when it covers none of the tile.
bool rasterizeVector(const VectorContent& shape, const Affine& m, TileKey key, std::uint32_t tw, std::uint32_t th,
                     std::vector<float>& out) {
    const bool fill = shape.fillColor[3] > 0.0f;
    const float scale = std::sqrt(std::fabs(m[0] * m[3] - m[1] * m[2]));
    const float halfStroke = 0.5f * shape.strokeWidth * scale;
    const bool stroke = shape.strokeColor[3] > 0.0f && halfStroke > 0.0f;
    if (!fill && !stroke) return false;
    const std::vector<Polyline> lines = flattenPath(shape, m);
    std::array<float, 4> bounds{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const Polyline& line : lines) {
        for (const auto& p : line.points) {
            bounds = {std::min(bounds[0], p[0]), std::min(bounds[1], p[1]), std::max(bounds[2], p[0]), std::max(bounds[3], p[1])};
        }
    }
    const float grow = stroke ? halfStroke + 1.0f : 1.0f;
    if (!overlapsTile({bounds[0] - grow, bounds[1] - grow, bounds[2] + grow, bounds[3] + grow}, key, tw, th)) return false;

    const float originX = static_cast<float>(key.tx * TILE_SIZE), originY = static_cast<float>(key.ty * TILE_SIZE);
    std::vector<float> fillCoverage, strokeCoverage;
    if (fill) {
        std::vector<Polygon> polygons;
        for (const Polyline& line : lines) {
            if (line.points.size() >= 3) polygons.push_back(line.points);  // filling closes every subpath
        }
        rasterizeCoverage(polygons, originX, originY, tw, th, fillCoverage);
    }
    if (stroke) rasterizeCoverage(strokePolygons(lines, halfStroke), originX, originY, tw, th, strokeCoverage);

    out.assign(std::size_t{tw} * th * 4, 0.0f);
    bool any = false;
    for (std::size_t i = 0; i < std::size_t{tw} * th; ++i) {
        float* pixel = &out[i * 4];
        if (fill && fillCoverage[i] > 0.0f) {
            pixel[0] = shape.fillColor[0];
            pixel[1] = shape.fillColor[1];
            pixel[2] = shape.fillColor[2];
            pixel[3] = clamp01(shape.fillColor[3]) * fillCoverage[i];
        }
        if (stroke && strokeCoverage[i] > 0.0f) {
            blendPixel(pixel, shape.strokeColor.data(), clamp01(shape.strokeColor[3]) * strokeCoverage[i], BlendMode::NORMAL);
        }
        any = any || pixel[3] > 0.0f;
    }
    return any;
}

// Reads `layer`'s tile `key` into a tw x th RGBA float buffer. Returns false if
// the tile is unallocated and transparent, so the caller can skip it.
bool sampleRasterTile(const SparseRasterLayer& layer, TileKey key, std::uint32_t tw, std::uint32_t th,
                      std::vector<float>& out) {
    const PixelTile* tile = layer.getTile(key);
    if (!tile && layer.defaultValue() == 0.0f) return false;
    out.assign(std::size_t{tw} * th * 4, layer.defaultValue());
    if (!tile) return true;
    const std::uint32_t channels = tile->channels;
    if (channels == 4) {  // colour layers: whole rows at once
        const std::uint32_t columns = std::min(tw, tile->width);
        for (std::uint32_t y = 0; y < std::min(th, tile->height); ++y) {
            halvesToFloats(tile->data.data() + tile->index(0, y), out.data() + std::size_t{y} * tw * 4,
                           std::size_t{columns} * 4);
        }
        return true;
    }
    for (std::uint32_t y = 0; y < std::min(th, tile->height); ++y) {
        for (std::uint32_t x = 0; x < std::min(tw, tile->width); ++x) {
            const std::uint16_t* texel = tile->data.data() + tile->index(x, y);
            float* pixel = out.data() + (std::size_t{y} * tw + x) * 4;
            for (std::uint32_t c = 0; c < std::min<std::uint32_t>(channels, 4); ++c) pixel[c] = halfToFloat(texel[c]);
            if (channels < 4) pixel[3] = 1.0f;  // no alpha channel means opaque
        }
    }
    return true;
}

float sampleMask(const SparseRasterLayer& mask, const PixelTile* tile, std::uint32_t x, std::uint32_t y) {
    if (!tile || x >= tile->width || y >= tile->height) return mask.defaultValue();
    return clamp01(halfToFloat(tile->data[tile->index(x, y)]));
}

// An adjustment layer: its operator applied to what lies below it in the
// group (`dst`), mixed in by blend mode, opacity and mask. Alpha is kept.
void applyAdjustmentLayer(const LayerNode& node, const AdjustmentContent& content, TileKey key, std::uint32_t tw,
                          std::uint32_t th, std::vector<float>& dst) {
    const std::optional<PointAdjustment> op = PointAdjustment::create(content.nodeType, content.serializedParams);
    if (!op || op->identity()) return;
    const SparseRasterLayer* mask = node.maskEnabled() ? node.mask() : nullptr;
    const PixelTile* maskTile = mask ? mask->getTile(key) : nullptr;
    for (std::uint32_t y = 0; y < th; ++y) {
        for (std::uint32_t x = 0; x < tw; ++x) {
            float* pixel = &dst[(std::size_t{y} * tw + x) * 4];
            if (!(pixel[3] > 0.0f)) continue;
            float amount = node.opacity();
            if (mask) amount *= sampleMask(*mask, maskTile, x, y);
            if (!(amount > 0.0f)) continue;
            const Rgb adjusted = op->apply({pixel[0], pixel[1], pixel[2]});
            const std::array<float, 3> blended =
                node.blendMode() == BlendMode::NORMAL ? adjusted : blendColor(node.blendMode(), {pixel[0], pixel[1], pixel[2]}, adjusted);
            for (int c = 0; c < 3; ++c) pixel[c] += (blended[static_cast<std::size_t>(c)] - pixel[c]) * amount;
        }
    }
}

void compositeInto(const LayerNode& node, TileKey key, std::uint32_t tw, std::uint32_t th, std::vector<float>& dst) {
    if (!node.visible() || node.opacity() <= 0.0f) return;
    if (const AdjustmentContent* adjustment = node.adjustment()) {
        applyAdjustmentLayer(node, *adjustment, key, tw, th, dst);
        return;
    }

    std::vector<float> source;
    const bool moved = !node.transform().isIdentity();
    if (node.isGroup()) {
        // Isolated group: children are composited onto transparency first.
        source.assign(dst.size(), 0.0f);
        for (std::size_t i = 0; i < node.childCount(); ++i) compositeInto(node.child(i), key, tw, th, source);
    } else if (const SparseRasterLayer* raster = node.raster()) {
        const bool drawn = moved ? sampleTransformed(*raster, node.transform().matrix(), key, tw, th, source)
                                 : sampleRasterTile(*raster, key, tw, th, source);
        if (!drawn) return;
    } else if (const VectorContent* shape = node.vectorShape()) {
        if (!rasterizeVector(*shape, node.transform().matrix(), key, tw, th, source)) return;
    } else if (const SmartObjectContent* object = node.smartObject()) {
        if (!object->pixels || !sampleTransformed(*object->pixels, node.transform().matrix(), key, tw, th, source)) return;
    } else {
        return;
    }

    const SparseRasterLayer* mask = node.maskEnabled() ? node.mask() : nullptr;
    const PixelTile* maskTile = mask ? mask->getTile(key) : nullptr;
    for (std::uint32_t y = 0; y < th; ++y) {
        for (std::uint32_t x = 0; x < tw; ++x) {
            const std::size_t i = (std::size_t{y} * tw + x) * 4;
            float alpha = clamp01(source[i + 3]) * node.opacity();
            if (mask) alpha *= sampleMask(*mask, maskTile, x, y);
            blendPixel(&dst[i], &source[i], alpha, node.blendMode());
        }
    }
}

// The raster layer `root` composites to unchanged, if there is one (see
// compositeTileHalf): every other child is hidden, fully transparent, an
// identity adjustment or a smart object that has not loaded.
const SparseRasterLayer* passThroughRaster(const LayerNode& root, std::uint32_t width, std::uint32_t height) {
    if (!root.isGroup() || !root.visible() || root.opacity() < 1.0f || root.maskEnabled()) return nullptr;
    const SparseRasterLayer* only = nullptr;
    for (std::size_t i = 0; i < root.childCount(); ++i) {
        const LayerNode& child = root.child(i);
        if (!child.visible() || child.opacity() <= 0.0f) continue;
        if (child.isGroup() || child.vectorShape()) return nullptr;
        if (const AdjustmentContent* adjustment = child.adjustment()) {
            const std::optional<PointAdjustment> op = PointAdjustment::create(adjustment->nodeType, adjustment->serializedParams);
            if (op && !op->identity()) return nullptr;
            continue;
        }
        if (const SmartObjectContent* object = child.smartObject()) {
            if (object->pixels) return nullptr;
            continue;
        }
        const SparseRasterLayer* raster = child.raster();
        if (!raster) continue;
        if (only || child.opacity() < 1.0f || child.maskEnabled() || raster->channels() != 4 ||
            raster->width() != width || raster->height() != height || !child.transform().isIdentity()) {
            return nullptr;
        }
        only = raster;
    }
    return only;
}

// Source-over of one straight-alpha pixel onto transparency, in binary16:
// alpha clamps to [0, 1], a pixel without positive alpha (NaN included)
// becomes transparent black, -0 becomes +0 and NaN is canonical, exactly as
// blendPixel's float arithmetic does it for finite values.
void copyOverTransparent(const std::uint16_t* in, std::uint16_t* out, std::size_t pixels) noexcept {
    for (std::size_t p = 0; p < pixels; ++p, in += 4, out += 4) {
        const std::uint16_t alpha = in[3];
        if (alpha == 0 || (alpha & 0x8000u) != 0 || alpha > 0x7C00u) {  // +0, negative (and -0), NaN
            out[0] = out[1] = out[2] = out[3] = 0;
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            const std::uint16_t v = in[c];
            out[c] = v == 0x8000u ? std::uint16_t{0}
                     : (v & 0x7FFFu) > 0x7C00u ? static_cast<std::uint16_t>((v & 0x8000u) | 0x7E00u)
                                               : v;
        }
        out[3] = std::min<std::uint16_t>(alpha, 0x3C00u);  // positive halves order like integers; 0x3C00 = 1.0
    }
}

}  // namespace

void floatsToHalves(const float* in, std::uint16_t* out, std::size_t count) noexcept {
#if DARKHOUSE_X86
    if (kHasF16C) {
        floatsToHalvesF16C(in, out, count);
        return;
    }
#endif
    for (std::size_t i = 0; i < count; ++i) out[i] = floatToHalf(in[i]);
}

void halvesToFloats(const std::uint16_t* in, float* out, std::size_t count) noexcept {
#if DARKHOUSE_X86
    if (kHasF16C) {
        halvesToFloatsF16C(in, out, count);
        return;
    }
#endif
    for (std::size_t i = 0; i < count; ++i) out[i] = halfToFloat(in[i]);
}

// -----------------------------------------------------------------------------
// SparseRasterLayer
// -----------------------------------------------------------------------------

SparseRasterLayer::SparseRasterLayer(std::uint32_t width, std::uint32_t height, std::uint32_t channels,
                                     float defaultValue)
    : width_(width),
      height_(height),
      channels_(channels),
      defaultValue_(defaultValue),
      defaultHalf_(floatToHalf(defaultValue)) {
    if (width == 0 || height == 0) throw std::invalid_argument("SparseRasterLayer: zero-sized layer");
    if (channels == 0 || channels > 4) throw std::invalid_argument("SparseRasterLayer: channels must be 1..4");
}

const PixelTile* SparseRasterLayer::getTile(TileKey key) const noexcept {
    const auto it = tiles_.find(key);
    return it == tiles_.end() ? nullptr : &it->second;
}

PixelTile* SparseRasterLayer::getTile(TileKey key) noexcept {
    const auto it = tiles_.find(key);
    return it == tiles_.end() ? nullptr : &it->second;
}

PixelTile& SparseRasterLayer::getOrCreateTile(TileKey key) {
    if (key.tx >= tilesX() || key.ty >= tilesY()) throw std::out_of_range("SparseRasterLayer: tile outside layer");
    if (PixelTile* existing = getTile(key)) return *existing;
    const std::uint32_t tileWidth = std::min(TILE_SIZE, width_ - key.tx * TILE_SIZE);
    const std::uint32_t tileHeight = std::min(TILE_SIZE, height_ - key.ty * TILE_SIZE);
    return tiles_.emplace(key, PixelTile(tileWidth, tileHeight, channels_, defaultHalf_)).first->second;
}

void SparseRasterLayer::releaseTile(TileKey key) {
    if (tiles_.erase(key) > 0) dirty_.insert(key);
}

void SparseRasterLayer::markTileDirty(TileKey key, PixelTile& tile) {
    tile.dirty = true;
    dirty_.insert(key);
}

bool SparseRasterLayer::writePixel(std::uint32_t x, std::uint32_t y, std::span<const float> values) {
    if (values.size() != channels_) throw std::invalid_argument("writePixel: value count must equal channels()");
    if (x >= width_ || y >= height_) return false;
    const TileKey key = tileKeyFor(x, y);
    PixelTile& tile = getOrCreateTile(key);
    std::uint16_t* texel = tile.data.data() + tile.index(x % TILE_SIZE, y % TILE_SIZE);
    for (std::uint32_t c = 0; c < channels_; ++c) texel[c] = floatToHalf(values[c]);
    markTileDirty(key, tile);
    return true;
}

void SparseRasterLayer::readPixel(std::uint32_t x, std::uint32_t y, std::span<float> out) const {
    if (out.size() != channels_) throw std::invalid_argument("readPixel: output size must equal channels()");
    const PixelTile* tile = (x < width_ && y < height_) ? getTile(tileKeyFor(x, y)) : nullptr;
    if (!tile) {
        std::fill(out.begin(), out.end(), defaultValue_);
        return;
    }
    const std::uint16_t* texel = tile->data.data() + tile->index(x % TILE_SIZE, y % TILE_SIZE);
    for (std::uint32_t c = 0; c < channels_; ++c) out[c] = halfToFloat(texel[c]);
}

template <class T, class CopyRow>
void SparseRasterLayer::writeRegionRows(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                                        std::span<const T> source, std::size_t srcRowStride, CopyRow copyRow) {
    if (w == 0 || h == 0) return;
    const std::size_t rowValues = std::size_t{w} * channels_;
    if (srcRowStride == 0) srcRowStride = rowValues;
    if (srcRowStride < rowValues || source.size() < (std::size_t{h} - 1) * srcRowStride + rowValues) {
        throw std::invalid_argument("writeRegion: source buffer is smaller than the region");
    }
    if (x >= width_ || y >= height_) return;
    const std::uint32_t x1 = clippedEnd(x, w, width_);
    const std::uint32_t y1 = clippedEnd(y, h, height_);

    for (std::uint32_t ty = y / TILE_SIZE; ty <= (y1 - 1) / TILE_SIZE; ++ty) {
        for (std::uint32_t tx = x / TILE_SIZE; tx <= (x1 - 1) / TILE_SIZE; ++tx) {
            const TileKey key{tx, ty};
            PixelTile& tile = getOrCreateTile(key);
            const std::uint32_t tileX0 = tx * TILE_SIZE;
            const std::uint32_t tileY0 = ty * TILE_SIZE;
            const std::uint32_t cx0 = std::max(x, tileX0);
            const std::uint32_t cx1 = std::min(x1, tileX0 + tile.width);
            const std::uint32_t cy0 = std::max(y, tileY0);
            const std::uint32_t cy1 = std::min(y1, tileY0 + tile.height);
            const std::size_t count = std::size_t{cx1 - cx0} * channels_;
            for (std::uint32_t py = cy0; py < cy1; ++py) {
                const T* src = source.data() + std::size_t{py - y} * srcRowStride + std::size_t{cx0 - x} * channels_;
                copyRow(src, tile.data.data() + tile.index(cx0 - tileX0, py - tileY0), count);
            }
            markTileDirty(key, tile);
        }
    }
}

void SparseRasterLayer::writeRegion(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                                    std::span<const float> source, std::size_t srcRowStride) {
    writeRegionRows(x, y, w, h, source, srcRowStride, floatsToHalves);
}

void SparseRasterLayer::writeRegion(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                                    std::span<const std::uint16_t> source, std::size_t srcRowStride) {
    writeRegionRows(x, y, w, h, source, srcRowStride, [](const std::uint16_t* src, std::uint16_t* dst, std::size_t n) {
        std::copy_n(src, n, dst);
    });
}

void SparseRasterLayer::markDirtyRegion(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    if (w == 0 || h == 0 || x >= width_ || y >= height_) return;
    const std::uint32_t x1 = clippedEnd(x, w, width_);
    const std::uint32_t y1 = clippedEnd(y, h, height_);
    for (std::uint32_t ty = y / TILE_SIZE; ty <= (y1 - 1) / TILE_SIZE; ++ty) {
        for (std::uint32_t tx = x / TILE_SIZE; tx <= (x1 - 1) / TILE_SIZE; ++tx) {
            const TileKey key{tx, ty};
            if (PixelTile* tile = getTile(key)) tile->dirty = true;
            dirty_.insert(key);
        }
    }
}

std::vector<TileKey> SparseRasterLayer::takeDirtyTiles() {
    std::vector<TileKey> keys(dirty_.begin(), dirty_.end());
    std::sort(keys.begin(), keys.end(), lessRowMajor);
    for (const TileKey& key : keys) {
        if (PixelTile* tile = getTile(key)) tile->dirty = false;
    }
    dirty_.clear();
    return keys;
}

std::size_t SparseRasterLayer::residentBytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& entry : tiles_) bytes += entry.second.byteSize();
    return bytes;
}

// -----------------------------------------------------------------------------
// LayerNode
// -----------------------------------------------------------------------------

LayerNode::LayerNode(std::string name, LayerType type, Content content)
    : name_(std::move(name)), type_(type), content_(std::move(content)) {}

std::unique_ptr<LayerNode> LayerNode::createGroup(std::string name) {
    return std::unique_ptr<LayerNode>(new LayerNode(std::move(name), LayerType::GROUP, std::monostate{}));
}

std::unique_ptr<LayerNode> LayerNode::createRaster(std::string name, std::uint32_t width, std::uint32_t height) {
    return std::unique_ptr<LayerNode>(
        new LayerNode(std::move(name), LayerType::RASTER_PIXEL, SparseRasterLayer(width, height, 4, 0.0f)));
}

std::unique_ptr<LayerNode> LayerNode::createAdjustment(std::string name, AdjustmentContent adjustment) {
    return std::unique_ptr<LayerNode>(
        new LayerNode(std::move(name), LayerType::PARAMETRIC_ADJUSTMENT, std::move(adjustment)));
}

std::unique_ptr<LayerNode> LayerNode::createVector(std::string name, VectorContent shape) {
    return std::unique_ptr<LayerNode>(new LayerNode(std::move(name), LayerType::VECTOR_SHAPE, std::move(shape)));
}

std::unique_ptr<LayerNode> LayerNode::createSmartObject(std::string name, SmartObjectContent object) {
    return std::unique_ptr<LayerNode>(new LayerNode(std::move(name), LayerType::SMART_OBJECT, std::move(object)));
}

void LayerNode::setOpacity(float opacity) noexcept {
    opacity_ = std::isfinite(opacity) ? clamp01(opacity) : 1.0f;
    markCompositeDirty();
}

void LayerNode::setVisible(bool visible) noexcept {
    visible_ = visible;
    markCompositeDirty();
}

void LayerNode::setBlendMode(BlendMode mode) noexcept {
    blendMode_ = mode;
    markCompositeDirty();
}

void LayerNode::setMaskEnabled(bool enabled) noexcept {
    maskEnabled_ = enabled;
    markCompositeDirty();
}

void LayerNode::removeMask() noexcept {
    mask_.reset();
    markCompositeDirty();
}

void LayerNode::setTransform(const LayerTransform& transform) noexcept {
    for (float v : {transform.translateX, transform.translateY, transform.scaleX, transform.scaleY, transform.rotation,
                    transform.pivotX, transform.pivotY}) {
        if (!std::isfinite(v)) return;
    }
    if (std::fabs(transform.scaleX) < 1e-4f || std::fabs(transform.scaleY) < 1e-4f) return;  // would be singular
    transform_ = transform;
    markCompositeDirty();
}

std::optional<std::array<float, 4>> LayerNode::contentBounds() const {
    if (const SparseRasterLayer* raster = this->raster()) {
        return std::array<float, 4>{0.0f, 0.0f, static_cast<float>(raster->width()), static_cast<float>(raster->height())};
    }
    if (const SmartObjectContent* object = smartObject(); object && object->pixels) {
        return std::array<float, 4>{0.0f, 0.0f, static_cast<float>(object->pixels->width()),
                                    static_cast<float>(object->pixels->height())};
    }
    if (const VectorContent* shape = vectorShape(); shape && shape->points.size() >= 2) {
        std::array<float, 4> bounds{shape->points[0], shape->points[1], shape->points[0], shape->points[1]};
        for (std::size_t i = 0; i + 1 < shape->points.size(); i += 2) {
            bounds = {std::min(bounds[0], shape->points[i]), std::min(bounds[1], shape->points[i + 1]),
                      std::max(bounds[2], shape->points[i]), std::max(bounds[3], shape->points[i + 1])};
        }
        const float half = shape->strokeColor[3] > 0.0f ? shape->strokeWidth * 0.5f : 0.0f;
        return std::array<float, 4>{bounds[0] - half, bounds[1] - half, bounds[2] + half, bounds[3] + half};
    }
    return std::nullopt;
}

void LayerNode::markCompositeDirty() noexcept {
    LayerNode* root = this;
    while (root->parent_) root = root->parent_;
    root->compositeDirty_ = true;
}

bool LayerNode::takeCompositeDirty() noexcept {
    const bool dirty = compositeDirty_;
    compositeDirty_ = false;
    return dirty;
}

SparseRasterLayer& LayerNode::addMask(std::uint32_t width, std::uint32_t height) {
    mask_ = std::make_unique<SparseRasterLayer>(width, height, 1, 1.0f);
    maskEnabled_ = true;
    mask_->markDirtyRegion(0, 0, width, height);  // the GPU copy must switch to "reveal all"
    markCompositeDirty();
    return *mask_;
}

LayerNode& LayerNode::addChild(std::unique_ptr<LayerNode>&& child) {
    return insertChild(children_.size(), std::move(child));
}

LayerNode& LayerNode::insertChild(std::size_t index, std::unique_ptr<LayerNode>&& child) {
    if (!child) throw std::invalid_argument("LayerNode::insertChild: child is null");
    if (!isGroup()) throw std::logic_error("LayerNode '" + name_ + "' is not a group and cannot have children");
    if (child->parent_ != nullptr) throw std::logic_error("LayerNode '" + child->name_ + "' already has a parent");
    if (child.get() == this || child->isAncestorOf(*this)) {
        throw std::logic_error("LayerNode: adding '" + child->name_ + "' would create a cycle");
    }
    if (index > children_.size()) throw std::out_of_range("LayerNode::insertChild: index out of range");
    child->parent_ = this;
    auto it = children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
    markCompositeDirty();
    return **it;
}

std::unique_ptr<LayerNode> LayerNode::removeChild(const LayerNode& child) {
    const auto it = std::find_if(children_.begin(), children_.end(),
                                 [&](const std::unique_ptr<LayerNode>& c) { return c.get() == &child; });
    if (it == children_.end()) throw std::invalid_argument("LayerNode::removeChild: not a child of '" + name_ + "'");
    std::unique_ptr<LayerNode> removed = std::move(*it);
    children_.erase(it);
    removed->parent_ = nullptr;
    markCompositeDirty();
    return removed;
}

void LayerNode::moveChild(std::size_t from, std::size_t to) {
    if (from >= children_.size() || to >= children_.size()) {
        throw std::out_of_range("LayerNode::moveChild: index out of range");
    }
    if (from == to) return;
    auto node = std::move(children_[from]);
    children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(from));
    children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(to), std::move(node));
    markCompositeDirty();
}

LayerNode& LayerNode::child(std::size_t index) {
    if (index >= children_.size()) throw std::out_of_range("LayerNode::child: index out of range");
    return *children_[index];
}

const LayerNode& LayerNode::child(std::size_t index) const {
    if (index >= children_.size()) throw std::out_of_range("LayerNode::child: index out of range");
    return *children_[index];
}

bool LayerNode::isAncestorOf(const LayerNode& other) const noexcept {
    for (const LayerNode* p = other.parent_; p != nullptr; p = p->parent_) {
        if (p == this) return true;
    }
    return false;
}

std::vector<TileKey> takeDirtyTiles(LayerNode& root) {
    std::unordered_set<TileKey, TileKeyHash> merged;
    root.visit([&](LayerNode& node, std::size_t) {
        if (SparseRasterLayer* raster = node.raster()) {
            for (const TileKey& key : raster->takeDirtyTiles()) merged.insert(key);
        }
        if (SparseRasterLayer* mask = node.mask()) {
            for (const TileKey& key : mask->takeDirtyTiles()) merged.insert(key);
        }
    });
    std::vector<TileKey> keys(merged.begin(), merged.end());
    std::sort(keys.begin(), keys.end(), lessRowMajor);
    return keys;
}

std::vector<TileKey> takeDirtyTiles(LayerNode& root, std::uint32_t canvasWidth, std::uint32_t canvasHeight) {
    const std::uint32_t tilesX = (canvasWidth + TILE_SIZE - 1) / TILE_SIZE;
    const std::uint32_t tilesY = (canvasHeight + TILE_SIZE - 1) / TILE_SIZE;
    std::vector<TileKey> keys;
    if (root.takeCompositeDirty()) {
        (void)takeDirtyTiles(root);  // everything is redone: drop the per-layer lists
        for (std::uint32_t ty = 0; ty < tilesY; ++ty) {
            for (std::uint32_t tx = 0; tx < tilesX; ++tx) keys.push_back({tx, ty});
        }
        return keys;
    }
    std::unordered_set<TileKey, TileKeyHash> merged;
    auto add = [&](const TileKey& key) {
        if (key.tx < tilesX && key.ty < tilesY) merged.insert(key);
    };
    root.visit([&](LayerNode& node, std::size_t) {
        if (SparseRasterLayer* raster = node.raster()) {
            const std::vector<TileKey> dirty = raster->takeDirtyTiles();
            if (node.transform().isIdentity()) {
                for (const TileKey& key : dirty) add(key);
            } else {
                // A moved layer's tile lands wherever its transform puts it (plus
                // a pixel of bilinear reach).
                const Affine m = node.transform().matrix();
                for (const TileKey& key : dirty) {
                    const float x0 = static_cast<float>(key.tx * TILE_SIZE), y0 = static_cast<float>(key.ty * TILE_SIZE);
                    const std::array<float, 4> b = mappedBounds(m, {x0, y0, x0 + TILE_SIZE, y0 + TILE_SIZE});
                    const auto first = [](float v) { return static_cast<std::uint32_t>(std::max(0.0f, std::floor((v - 1.0f) / TILE_SIZE))); };
                    const auto last = [](float v) { return static_cast<std::int64_t>(std::floor((v + 1.0f) / TILE_SIZE)); };
                    for (std::int64_t ty = first(b[1]); ty <= std::min<std::int64_t>(last(b[3]), tilesY - 1); ++ty) {
                        for (std::int64_t tx = first(b[0]); tx <= std::min<std::int64_t>(last(b[2]), tilesX - 1); ++tx) {
                            add({static_cast<std::uint32_t>(tx), static_cast<std::uint32_t>(ty)});
                        }
                    }
                }
            }
        }
        if (SparseRasterLayer* mask = node.mask()) {
            for (const TileKey& key : mask->takeDirtyTiles()) add(key);
        }
    });
    keys.assign(merged.begin(), merged.end());
    std::sort(keys.begin(), keys.end(), lessRowMajor);
    return keys;
}

// -----------------------------------------------------------------------------
// Compositing
// -----------------------------------------------------------------------------

float blendChannel(BlendMode mode, float backdrop, float source) noexcept {
    const float cb = clamp01(backdrop), cs = clamp01(source);  // for the display-referred modes
    switch (mode) {
    case BlendMode::NORMAL: return source;
    case BlendMode::MULTIPLY: return backdrop * source;
    case BlendMode::DARKEN: return std::min(backdrop, source);
    case BlendMode::LIGHTEN: return std::max(backdrop, source);
    case BlendMode::DIFFERENCE: return std::fabs(backdrop - source);
    case BlendMode::SCREEN: return cb + cs - cb * cs;
    case BlendMode::OVERLAY:  // HardLight with the layers swapped
        return cb <= 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
    case BlendMode::COLOR_DODGE:
        if (cb <= 0.0f) return 0.0f;
        if (cs >= 1.0f) return 1.0f;
        return std::min(1.0f, cb / (1.0f - cs));
    case BlendMode::COLOR_BURN:
        if (cb >= 1.0f) return 1.0f;
        if (cs <= 0.0f) return 0.0f;
        return 1.0f - std::min(1.0f, (1.0f - cb) / cs);
    case BlendMode::HARD_LIGHT:
        if (cs <= 0.5f) return cb * 2.0f * cs;
        return cb + (2.0f * cs - 1.0f) - cb * (2.0f * cs - 1.0f);
    case BlendMode::SOFT_LIGHT: {
        if (cs <= 0.5f) return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
        const float d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : std::sqrt(cb);
        return cb + (2.0f * cs - 1.0f) * (d - cb);
    }
    case BlendMode::EXCLUSION: return cb + cs - 2.0f * cb * cs;
    case BlendMode::HUE:
    case BlendMode::SATURATION:
    case BlendMode::COLOR:
    case BlendMode::LUMINOSITY: break;  // non-separable: see blendColor
    }
    return source;
}

namespace {

using Color = std::array<float, 3>;

float lum(const Color& c) noexcept { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

Color clipColor(Color c) noexcept {
    const float l = lum(c);
    const float n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
    for (float& v : c) {
        if (n < 0.0f && l - n > 0.0f) v = l + (v - l) * l / (l - n);
        if (x > 1.0f && x - l > 0.0f) v = l + (v - l) * (1.0f - l) / (x - l);
    }
    return c;
}

Color setLum(Color c, float l) noexcept {
    const float d = l - lum(c);
    for (float& v : c) v += d;
    return clipColor(c);
}

float sat(const Color& c) noexcept { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

Color setSat(Color c, float s) noexcept {
    std::array<std::size_t, 3> order{0, 1, 2};  // min, mid, max
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return c[a] < c[b]; });
    const float lo = c[order[0]], hi = c[order[2]];
    Color out{};
    if (hi > lo) {
        out[order[1]] = (c[order[1]] - lo) * s / (hi - lo);
        out[order[2]] = s;
    }
    return out;
}

}  // namespace

std::array<float, 3> blendColor(BlendMode mode, const std::array<float, 3>& backdrop, const std::array<float, 3>& source) noexcept {
    switch (mode) {
    case BlendMode::HUE:
    case BlendMode::SATURATION:
    case BlendMode::COLOR:
    case BlendMode::LUMINOSITY: {
        const Color cb{clamp01(backdrop[0]), clamp01(backdrop[1]), clamp01(backdrop[2])};
        const Color cs{clamp01(source[0]), clamp01(source[1]), clamp01(source[2])};
        switch (mode) {
        case BlendMode::HUE: return setLum(setSat(cs, sat(cb)), lum(cb));
        case BlendMode::SATURATION: return setLum(setSat(cb, sat(cs)), lum(cb));
        case BlendMode::COLOR: return setLum(cs, lum(cb));
        default: return setLum(cb, lum(cs));
        }
    }
    default:
        return {blendChannel(mode, backdrop[0], source[0]), blendChannel(mode, backdrop[1], source[1]),
                blendChannel(mode, backdrop[2], source[2])};
    }
}

// -----------------------------------------------------------------------------
// Transforms and paths
// -----------------------------------------------------------------------------

std::array<float, 2> applyAffine(const Affine& m, float x, float y) noexcept {
    return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
}

std::optional<Affine> invertAffine(const Affine& m) noexcept {
    const float det = m[0] * m[3] - m[1] * m[2];
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return std::nullopt;
    const float a = m[3] / det, b = -m[1] / det, c = -m[2] / det, d = m[0] / det;
    return Affine{a, b, c, d, -(a * m[4] + c * m[5]), -(b * m[4] + d * m[5])};
}

bool LayerTransform::isIdentity() const noexcept {
    return translateX == 0.0f && translateY == 0.0f && scaleX == 1.0f && scaleY == 1.0f && rotation == 0.0f;
}

Affine LayerTransform::matrix() const noexcept {
    const float radians = rotation * 3.14159265358979f / 180.0f;
    const float cs = std::cos(radians), sn = std::sin(radians);
    const float a = cs * scaleX, b = sn * scaleX, c = -sn * scaleY, d = cs * scaleY;
    return {a, b, c, d, pivotX + translateX - (a * pivotX + c * pivotY), pivotY + translateY - (b * pivotX + d * pivotY)};
}

std::vector<Polyline> flattenPath(const VectorContent& shape, const Affine& m, float tolerance) {
    std::vector<Polyline> out;
    std::size_t point = 0;  // next x, y pair
    const std::size_t pairs = shape.points.size() / 2;
    auto has = [&](std::size_t n) { return point + n <= pairs; };
    auto next = [&]() {  // callers check has() first
        const std::array<float, 2> p = applyAffine(m, shape.points[point * 2], shape.points[point * 2 + 1]);
        ++point;
        return p;
    };
    tolerance = std::max(tolerance, 0.01f);
    for (const PathVerb verb : shape.verbs) {
        switch (verb) {
        case PathVerb::MOVE_TO:
            if (!has(1)) return out;
            out.push_back({{next()}, false});
            break;
        case PathVerb::LINE_TO:
            if (!has(1)) return out;
            if (out.empty()) out.push_back({{{0.0f, 0.0f}}, false});
            out.back().points.push_back(next());
            break;
        case PathVerb::CUBIC_TO: {
            if (!has(3)) return out;
            if (out.empty()) out.push_back({{{0.0f, 0.0f}}, false});
            const std::array<float, 2> p0 = out.back().points.back();
            const std::array<float, 2> p1 = next(), p2 = next(), p3 = next();
            // Enough segments that the chord error stays under `tolerance`.
            const float length = std::hypot(p1[0] - p0[0], p1[1] - p0[1]) + std::hypot(p2[0] - p1[0], p2[1] - p1[1]) +
                                 std::hypot(p3[0] - p2[0], p3[1] - p2[1]);
            const int segments = std::clamp(static_cast<int>(std::ceil(std::sqrt(length / tolerance) * 0.5f)), 1, 256);
            for (int i = 1; i <= segments; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(segments), u = 1.0f - t;
                const float w0 = u * u * u, w1 = 3.0f * u * u * t, w2 = 3.0f * u * t * t, w3 = t * t * t;
                out.back().points.push_back({w0 * p0[0] + w1 * p1[0] + w2 * p2[0] + w3 * p3[0],
                                             w0 * p0[1] + w1 * p1[1] + w2 * p2[1] + w3 * p3[1]});
            }
            break;
        }
        case PathVerb::CLOSE:
            if (!out.empty()) {
                out.back().closed = true;
                const auto start = out.back().points.front();
                out.push_back({{start}, false});  // a following LINE_TO continues from the start
            }
            break;
        }
    }
    std::erase_if(out, [](const Polyline& line) { return line.points.size() < 2; });
    return out;
}

CompositedTile compositeTileCPU(const LayerNode& root, TileKey key, std::uint32_t canvasWidth,
                                std::uint32_t canvasHeight) {
    if (canvasWidth == 0 || canvasHeight == 0 || key.tx >= (canvasWidth + TILE_SIZE - 1) / TILE_SIZE ||
        key.ty >= (canvasHeight + TILE_SIZE - 1) / TILE_SIZE) {
        throw std::out_of_range("compositeTileCPU: tile outside the canvas");
    }
    CompositedTile out;
    out.width = std::min(TILE_SIZE, canvasWidth - key.tx * TILE_SIZE);
    out.height = std::min(TILE_SIZE, canvasHeight - key.ty * TILE_SIZE);
    out.rgba.assign(std::size_t{out.width} * out.height * 4, 0.0f);
    compositeInto(root, key, out.width, out.height, out.rgba);
    return out;
}

CompositedTileHalf compositeTileHalf(const LayerNode& root, TileKey key, std::uint32_t canvasWidth,
                                     std::uint32_t canvasHeight) {
    CompositedTileHalf out;
    if (const SparseRasterLayer* raster = passThroughRaster(root, canvasWidth, canvasHeight)) {
        if (key.tx >= raster->tilesX() || key.ty >= raster->tilesY()) {
            throw std::out_of_range("compositeTileHalf: tile outside the canvas");
        }
        out.width = std::min(TILE_SIZE, canvasWidth - key.tx * TILE_SIZE);
        out.height = std::min(TILE_SIZE, canvasHeight - key.ty * TILE_SIZE);
        out.rgba.resize(std::size_t{out.width} * out.height * 4);  // the layer's tile has the same shape
        if (const PixelTile* tile = raster->getTile(key)) {
            copyOverTransparent(tile->data.data(), out.rgba.data(), std::size_t{out.width} * out.height);
        } else {
            std::fill(out.rgba.begin(), out.rgba.end(), std::uint16_t{0});
        }
        return out;
    }
    const CompositedTile tile = compositeTileCPU(root, key, canvasWidth, canvasHeight);
    out.width = tile.width;
    out.height = tile.height;
    out.rgba.resize(tile.rgba.size());
    floatsToHalves(tile.rgba.data(), out.rgba.data(), tile.rgba.size());
    return out;
}

}  // namespace darkhouse
