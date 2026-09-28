#include "denoise.hpp"

#include "layer_stack.hpp"  // floatToHalf / halfToFloat (fp16 storage emulation)

#include <algorithm>
#include <cmath>
#include <random>

namespace darkhouse {
namespace {

constexpr float kInvSqrt2 = 0.70710678118654752f;
constexpr float kInvSqrt3 = 0.57735026918962576f;
constexpr float kInvSqrt6 = 0.40824829046386302f;
constexpr std::array<float, 5> kBinomial{0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};
constexpr std::array<float, DH_DENOISE_MAX_LEVELS> kLevelNoise{DH_DENOISE_LEVEL_NOISE};
constexpr std::array<float, DH_DENOISE_MAX_LEVELS> kLumaWeights{DH_DENOISE_LUMA_WEIGHTS};
constexpr std::array<float, DH_DENOISE_MAX_LEVELS> kChromaWeights{DH_DENOISE_CHROMA_WEIGHTS};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// One pyramid level: three interleaved float channels (Y, C1, C2).
struct Plane3 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> data;

    Plane3() = default;
    Plane3(std::uint32_t w, std::uint32_t h) : width(w), height(h), data(std::size_t{w} * h * 3, 0.0f) {}
    [[nodiscard]] Vec3 at(std::uint32_t x, std::uint32_t y) const noexcept {
        const float* p = &data[(std::size_t{y} * width + x) * 3];
        return {p[0], p[1], p[2]};
    }
    void set(std::uint32_t x, std::uint32_t y, Vec3 v) noexcept {
        float* p = &data[(std::size_t{y} * width + x) * 3];
        p[0] = v.x;
        p[1] = v.y;
        p[2] = v.z;
    }
};

float quantize(float value) noexcept { return halfToFloat(floatToHalf(value)); }
Vec3 quantize(Vec3 v) noexcept { return {quantize(v.x), quantize(v.y), quantize(v.z)}; }

// sqrt (variance stabilisation) followed by the orthonormal opponent transform.
Vec3 stabilize(const float* rgb) noexcept {
    const float r = std::sqrt(std::max(rgb[0], 0.0f));
    const float g = std::sqrt(std::max(rgb[1], 0.0f));
    const float b = std::sqrt(std::max(rgb[2], 0.0f));
    return {(r + g + b) * kInvSqrt3, (r - b) * kInvSqrt2, (r - 2.0f * g + b) * kInvSqrt6};
}

void destabilize(Vec3 s, float* rgb) noexcept {
    const float y = s.x * kInvSqrt3;
    const float r = y + s.y * kInvSqrt2 + s.z * kInvSqrt6;
    const float g = y - 2.0f * s.z * kInvSqrt6;
    const float b = y - s.y * kInvSqrt2 + s.z * kInvSqrt6;
    const float cr = std::max(r, 0.0f);
    const float cg = std::max(g, 0.0f);
    const float cb = std::max(b, 0.0f);
    rgb[0] = cr * cr;
    rgb[1] = cg * cg;
    rgb[2] = cb * cb;
}

std::uint32_t clampIndex(std::int64_t i, std::uint32_t size) noexcept {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(i, 0, static_cast<std::int64_t>(size) - 1));
}

// 5-tap binomial, stride 2, clamp-to-edge; horizontal pass first, then
// vertical, accumulating taps in order -2..2 (the shader's order).
Plane3 downsample(const Plane3& src, bool storeHalf) {
    Plane3 dst((src.width + 1) / 2, (src.height + 1) / 2);
    std::vector<Vec3> rows(5);
    for (std::uint32_t y = 0; y < dst.height; ++y) {
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            Vec3 sum;
            for (int j = -2; j <= 2; ++j) {
                const std::uint32_t sy = clampIndex(2 * static_cast<std::int64_t>(y) + j, src.height);
                Vec3 h;
                for (int i = -2; i <= 2; ++i) {
                    const Vec3 t = src.at(clampIndex(2 * static_cast<std::int64_t>(x) + i, src.width), sy);
                    const float w = kBinomial[static_cast<std::size_t>(i + 2)];
                    h.x += w * t.x;
                    h.y += w * t.y;
                    h.z += w * t.z;
                }
                const float w = kBinomial[static_cast<std::size_t>(j + 2)];
                sum.x += w * h.x;
                sum.y += w * h.y;
                sum.z += w * h.z;
            }
            dst.set(x, y, storeHalf ? quantize(sum) : sum);
        }
    }
    return dst;
}

// Bilinear upsample of `coarse` at fine texel (x, y), texel-centre aligned:
// even fine texels take 1/4 of the left/upper coarse neighbour and 3/4 of the
// covering one, odd texels 3/4 and 1/4 of the right/lower one.
Vec3 upsampleAt(const Plane3& coarse, std::uint32_t x, std::uint32_t y) noexcept {
    const std::int64_t cx = x / 2;
    const std::int64_t cy = y / 2;
    const std::int64_t nx = (x % 2 == 0) ? cx - 1 : cx + 1;
    const std::int64_t ny = (y % 2 == 0) ? cy - 1 : cy + 1;
    const std::uint32_t x0 = clampIndex(cx, coarse.width);
    const std::uint32_t x1 = clampIndex(nx, coarse.width);
    const std::uint32_t y0 = clampIndex(cy, coarse.height);
    const std::uint32_t y1 = clampIndex(ny, coarse.height);
    const Vec3 a = coarse.at(x0, y0);
    const Vec3 b = coarse.at(x1, y0);
    const Vec3 c = coarse.at(x0, y1);
    const Vec3 d = coarse.at(x1, y1);
    auto mix = [](float p, float q) { return 0.75f * p + 0.25f * q; };
    return {mix(mix(a.x, b.x), mix(c.x, d.x)), mix(mix(a.y, b.y), mix(c.y, d.y)), mix(mix(a.z, b.z), mix(c.z, d.z))};
}

float shrink(float d, float threshold) noexcept {
    if (threshold <= 0.0f) return d;
    const float d2 = d * d;
    return d * d2 / (d2 + threshold * threshold);
}

float finiteOr(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }

}  // namespace

DenoiseParams sanitize(const DenoiseParams& params) noexcept {
    const DenoiseParams defaults;
    DenoiseParams p;
    p.luminance = std::clamp(finiteOr(params.luminance, defaults.luminance), 0.0f, 1.0f);
    p.chrominance = std::clamp(finiteOr(params.chrominance, defaults.chrominance), 0.0f, 1.0f);
    p.detail = std::clamp(finiteOr(params.detail, defaults.detail), 0.0f, 1.0f);
    p.noiseLevel = std::clamp(finiteOr(params.noiseLevel, 0.0f), 0.0f, 1.0f);
    return p;
}

bool isIdentity(const DenoiseParams& params) noexcept {
    const DenoiseParams p = sanitize(params);
    return p.luminance == 0.0f && p.chrominance == 0.0f;
}

std::uint32_t denoiseLevelCount(std::uint32_t width, std::uint32_t height) noexcept {
    const std::uint32_t smallest = std::max(std::min(width, height), 1u);
    int log2 = 0;
    while ((smallest >> (log2 + 1)) != 0) ++log2;
    return static_cast<std::uint32_t>(std::clamp(log2 - 2, 1, DH_DENOISE_MAX_LEVELS));
}

std::array<std::uint32_t, 2> denoiseLevelSize(std::uint32_t width, std::uint32_t height, std::uint32_t level) noexcept {
    for (std::uint32_t i = 0; i < level; ++i) {
        width = (width + 1) / 2;
        height = (height + 1) / 2;
    }
    return {width, height};
}

std::array<std::array<float, 3>, DH_DENOISE_MAX_LEVELS> denoiseThresholds(const DenoiseParams& params,
                                                                           const std::array<float, 3>& sigma) noexcept {
    const DenoiseParams p = sanitize(params);
    std::array<std::array<float, 3>, DH_DENOISE_MAX_LEVELS> thresholds{};
    for (std::size_t k = 0; k < thresholds.size(); ++k) {
        const float luma = p.luminance * static_cast<float>(DH_DENOISE_LUMA_SCALE) * kLumaWeights[k] * kLevelNoise[k];
        const float chroma =
            p.chrominance * static_cast<float>(DH_DENOISE_CHROMA_SCALE) * kChromaWeights[k] * kLevelNoise[k];
        thresholds[k] = {luma * sigma[0], chroma * sigma[1], chroma * sigma[2]};
    }
    return thresholds;
}

namespace denoise_detail {

std::uint32_t histogramBin(float magnitude) noexcept {
    if (!(magnitude > 0.0f)) return 0;
    const float position = (std::log2(magnitude) - static_cast<float>(DH_DENOISE_HIST_MIN_EXP)) *
                           static_cast<float>(DH_DENOISE_HIST_BINS_PER_OCTAVE);
    return static_cast<std::uint32_t>(std::clamp(std::floor(position), 0.0f, static_cast<float>(DH_DENOISE_HIST_BINS - 1)));
}

float histogramMedian(std::span<const std::uint32_t> bins) noexcept {
    std::uint32_t total = 0;
    for (std::uint32_t count : bins) total += count;
    if (total == 0) return 0.0f;
    const float half = static_cast<float>(total) * 0.5f;
    std::uint32_t below = 0;
    for (std::size_t b = 0; b < bins.size(); ++b) {
        if (bins[b] > 0 && static_cast<float>(below + bins[b]) >= half) {
            const float fraction = (half - static_cast<float>(below)) / static_cast<float>(bins[b]);
            const float log2Value = static_cast<float>(DH_DENOISE_HIST_MIN_EXP) +
                                    (static_cast<float>(b) + fraction) / static_cast<float>(DH_DENOISE_HIST_BINS_PER_OCTAVE);
            return std::exp2(log2Value);
        }
        below += bins[b];
    }
    return 0.0f;
}

std::array<float, DH_DENOISE_MAX_LEVELS> measureLevelNoise(std::uint32_t size, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    std::vector<Plane3> pyramid;
    pyramid.emplace_back(size, size);
    for (float& value : pyramid.front().data) value = noise(rng);
    for (int k = 0; k < DH_DENOISE_MAX_LEVELS; ++k) pyramid.push_back(downsample(pyramid.back(), false));

    std::array<float, DH_DENOISE_MAX_LEVELS> result{};
    for (std::size_t k = 0; k < result.size(); ++k) {
        const Plane3& fine = pyramid[k];
        double sumSquares = 0.0;
        for (std::uint32_t y = 0; y < fine.height; ++y) {
            for (std::uint32_t x = 0; x < fine.width; ++x) {
                const Vec3 g = fine.at(x, y);
                const Vec3 u = upsampleAt(pyramid[k + 1], x, y);
                sumSquares += double(g.x - u.x) * (g.x - u.x) + double(g.y - u.y) * (g.y - u.y) +
                              double(g.z - u.z) * (g.z - u.z);
            }
        }
        result[k] = static_cast<float>(std::sqrt(sumSquares / (3.0 * fine.width * fine.height)));
    }
    return result;
}

}  // namespace denoise_detail

DenoiseStatistics denoiseReference(std::span<const float> rgba, std::uint32_t width, std::uint32_t height,
                                   const DenoiseParams& params, std::vector<float>& out) {
    out.assign(rgba.begin(), rgba.end());
    DenoiseStatistics stats;
    if (width == 0 || height == 0 || rgba.size() < std::size_t{width} * height * 4) return stats;
    const DenoiseParams p = sanitize(params);
    const std::uint32_t levels = denoiseLevelCount(width, height);
    stats.levels = levels;
    if (isIdentity(p)) return stats;  // the node copies its input unchanged

    // Level 0 is never stored on the GPU: passes recompute it from the fp16
    // input, so it stays unquantised here too.
    Plane3 g0(width, height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) g0.set(x, y, stabilize(&rgba[(std::size_t{y} * width + x) * 4]));
    }
    std::vector<Plane3> gaussian;  // G_1 .. G_L, fp16 like the GPU textures
    gaussian.push_back(downsample(g0, true));
    for (std::uint32_t k = 1; k < levels; ++k) gaussian.push_back(downsample(gaussian.back(), true));

    // Noise estimate: |Haar HH| of the 2x2 blocks under the sampled down0
    // workgroups (see DH_DENOISE_SAMPLE_STRIDE).
    std::array<std::array<std::uint32_t, DH_DENOISE_HIST_BINS>, 3> histogram{};
    const Plane3& g1 = gaussian.front();
    const std::uint32_t groupsX = (g1.width + DH_DENOISE_GROUP - 1) / DH_DENOISE_GROUP;
    const std::uint32_t groupsY = (g1.height + DH_DENOISE_GROUP - 1) / DH_DENOISE_GROUP;
    for (std::uint32_t gy = 0; gy < groupsY; ++gy) {
        for (std::uint32_t gx = 0; gx < groupsX; ++gx) {
            if ((gx + gy) % DH_DENOISE_SAMPLE_STRIDE != 0) continue;
            for (std::uint32_t ly = 0; ly < DH_DENOISE_GROUP; ++ly) {
                for (std::uint32_t lx = 0; lx < DH_DENOISE_GROUP; ++lx) {
                    const std::uint32_t x = gx * DH_DENOISE_GROUP + lx;
                    const std::uint32_t y = gy * DH_DENOISE_GROUP + ly;
                    if (x >= g1.width || y >= g1.height || 2 * x + 1 >= width || 2 * y + 1 >= height) continue;
                    const Vec3 a = g0.at(2 * x, 2 * y);
                    const Vec3 b = g0.at(2 * x + 1, 2 * y);
                    const Vec3 c = g0.at(2 * x, 2 * y + 1);
                    const Vec3 d = g0.at(2 * x + 1, 2 * y + 1);
                    const Vec3 hh{(a.x - b.x - c.x + d.x) * 0.5f, (a.y - b.y - c.y + d.y) * 0.5f,
                                  (a.z - b.z - c.z + d.z) * 0.5f};
                    ++histogram[0][denoise_detail::histogramBin(std::fabs(hh.x))];
                    ++histogram[1][denoise_detail::histogramBin(std::fabs(hh.y))];
                    ++histogram[2][denoise_detail::histogramBin(std::fabs(hh.z))];
                }
            }
        }
    }
    for (std::size_t c = 0; c < 3; ++c) {
        stats.sigma[c] = p.noiseLevel > 0.0f
                             ? p.noiseLevel
                             : denoise_detail::histogramMedian(histogram[c]) * static_cast<float>(DH_DENOISE_MAD_TO_SIGMA);
    }
    const auto thresholds = denoiseThresholds(p, stats.sigma);

    // Reconstruction, coarse to fine: R_L = G_L, R_k = up(R_(k+1)) + shrink(d_k).
    Plane3 reconstructed = gaussian.back();
    for (std::uint32_t k = levels - 1; k >= 1; --k) {
        const Plane3& g = gaussian[k - 1];       // G_k
        const Plane3& coarser = gaussian[k];     // G_(k+1)
        const auto& t = thresholds[k];
        Plane3 next(g.width, g.height);
        for (std::uint32_t y = 0; y < g.height; ++y) {
            for (std::uint32_t x = 0; x < g.width; ++x) {
                const Vec3 upR = upsampleAt(reconstructed, x, y);
                const Vec3 upG = upsampleAt(coarser, x, y);
                const Vec3 v = g.at(x, y);
                const Vec3 r{upR.x + shrink(v.x - upG.x, t[0]), upR.y + shrink(v.y - upG.y, t[1]),
                             upR.z + shrink(v.z - upG.z, t[2])};
                next.set(x, y, quantize(r));
            }
        }
        reconstructed = std::move(next);
    }

    // Final level: band d_0, detail (grain) retention, back to linear RGB.
    const auto& t0 = thresholds[0];
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const Vec3 s0 = g0.at(x, y);
            const Vec3 upG = upsampleAt(g1, x, y);
            const Vec3 upR = upsampleAt(reconstructed, x, y);
            const Vec3 d{s0.x - upG.x, s0.y - upG.y, s0.z - upG.z};
            Vec3 kept{shrink(d.x, t0[0]), shrink(d.y, t0[1]), shrink(d.z, t0[2])};
            kept.x += p.detail * (d.x - kept.x);
            float* pixel = &out[(std::size_t{y} * width + x) * 4];
            destabilize({upR.x + kept.x, upR.y + kept.y, upR.z + kept.z}, pixel);
            pixel[0] = quantize(pixel[0]);
            pixel[1] = quantize(pixel[1]);
            pixel[2] = quantize(pixel[2]);
            pixel[3] = quantize(rgba[(std::size_t{y} * width + x) * 4 + 3]);
        }
    }
    return stats;
}

}  // namespace darkhouse
