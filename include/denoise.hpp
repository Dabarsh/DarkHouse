// DarkHouse — noise reduction.
//
// Algorithm (identical on the GPU node and in the CPU reference below):
//
//  1. Variance stabilisation. Photon shot noise grows with the square root of
//     the signal; taking sqrt() of linear RGB makes it roughly uniform, so a
//     single threshold per band works across shadows and highlights.
//  2. Orthonormal opponent transform (Y, C1, C2). Luma and chroma noise are
//     treated separately (chroma noise is blotchy and can be removed much
//     more aggressively), and white noise keeps equal variance per channel.
//  3. Laplacian pyramid: G_(k+1) = downsample(G_k) with a separable 5-tap
//     binomial filter; detail band d_k = G_k - up(G_(k+1)) with a fixed-weight
//     bilinear upsampler. The decimated pyramid costs ~1.33x one full-size
//     pass, and reconstruction is exact when nothing is shrunk.
//  4. Noise estimate: sigma = MAD(Haar HH) / 0.6745 per channel at full
//     resolution. The diagonal Haar coefficient ignores edges and gradients,
//     so the median is dominated by noise even on detailed images. Per-band
//     noise follows from the pyramid's white-noise response.
//  5. Wiener-style shrinkage d' = d * d^2 / (d^2 + T^2) of every band, with
//     T = strength * scale * weight[k] * level_noise[k] * sigma; it never
//     flips signs and has no hard-threshold ringing.
//  6. Reconstruction R_k = up(R_(k+1)) + d'_k, back to RGB, then squared.
//     "Detail" keeps a share of the removed finest luma structure (grain),
//     which reads as natural rather than plastic.
//
// Constants shared with the shaders live in shaders/denoise_config.h.
#pragma once

#include "denoise_config.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace darkhouse {

// Parameters of the "denoise" develop node. The serialized form
// (edit_nodes.serialized_params) is these 4 floats in host byte order.
struct DenoiseParams {
    float luminance = 0.5f;    // [0, 1] luma noise reduction
    float chrominance = 0.5f;  // [0, 1] colour noise reduction
    float detail = 0.2f;       // [0, 1] share of removed fine luma structure kept (grain)
    float noiseLevel = 0.0f;   // 0 = estimate automatically; > 0 = noise sigma in the stabilised (sqrt) domain
};
static_assert(sizeof(DenoiseParams) == 16, "DenoiseParams must stay 4 floats (serialized form)");

// Clamps every field to its valid range; non-finite values become defaults.
[[nodiscard]] DenoiseParams sanitize(const DenoiseParams& params) noexcept;
// True when the node would not change the image (both strengths zero).
[[nodiscard]] bool isIdentity(const DenoiseParams& params) noexcept;

// Pyramid depth (detail bands) used for a w x h image: 1 .. DH_DENOISE_MAX_LEVELS.
[[nodiscard]] std::uint32_t denoiseLevelCount(std::uint32_t width, std::uint32_t height) noexcept;
// Size of pyramid level `level` (0 = full size); each level halves, rounding up.
[[nodiscard]] std::array<std::uint32_t, 2> denoiseLevelSize(std::uint32_t width, std::uint32_t height,
                                                            std::uint32_t level) noexcept;

struct DenoiseStatistics {
    std::array<float, 3> sigma{};  // noise sigma per channel (Y, C1, C2) in the stabilised domain
    std::uint32_t levels = 0;      // detail bands used
};

// Band thresholds [level][channel] for the given statistics and parameters.
[[nodiscard]] std::array<std::array<float, 3>, DH_DENOISE_MAX_LEVELS> denoiseThresholds(
    const DenoiseParams& params, const std::array<float, 3>& sigma) noexcept;

// CPU reference of the GPU DenoiseNode: the same passes, filters, sampling
// pattern and fp16 storage between passes, so GPU output can be checked
// against it. `rgba` is interleaved linear RGBA (width * height * 4 floats);
// alpha passes through. Single-threaded and unoptimised: for tests and tools.
DenoiseStatistics denoiseReference(std::span<const float> rgba, std::uint32_t width, std::uint32_t height,
                                   const DenoiseParams& params, std::vector<float>& out);

namespace denoise_detail {

// Standard deviation of each detail band for unit white noise in all three
// stabilised channels (what DH_DENOISE_LEVEL_NOISE encodes). Deterministic.
[[nodiscard]] std::array<float, DH_DENOISE_MAX_LEVELS> measureLevelNoise(std::uint32_t size, std::uint32_t seed);

// log2-histogram bin of |x| used by the noise estimator.
[[nodiscard]] std::uint32_t histogramBin(float magnitude) noexcept;
// Median of a histogram as the GPU sigma pass computes it (0 when empty).
[[nodiscard]] float histogramMedian(std::span<const std::uint32_t> bins) noexcept;

}  // namespace denoise_detail
}  // namespace darkhouse
