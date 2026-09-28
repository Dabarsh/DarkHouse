// Synthetic images for the denoise tests (CPU reference and GPU node).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace darkhouse::test {

// Linear RGBA test scene: smooth colour gradients, hard-edged patches, a fine
// sinusoidal texture and dark/bright regions, all kept >= 0.04 so noise in
// the stabilised domain is never clipped at zero.
inline std::vector<float> makeCleanImage(std::uint32_t width, std::uint32_t height) {
    std::vector<float> image(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
            float r = 0.08f + 0.5f * u;
            float g = 0.10f + 0.4f * v;
            float b = 0.30f - 0.2f * u * v;
            // Hard-edged patches.
            if (u > 0.1f && u < 0.35f && v > 0.15f && v < 0.45f) r = 0.7f, g = 0.2f, b = 0.1f;
            if (u > 0.55f && u < 0.9f && v > 0.2f && v < 0.35f) r = 0.1f, g = 0.5f, b = 0.8f;
            if (u > 0.4f && u < 0.6f && v > 0.6f && v < 0.9f) r = g = b = 0.9f;
            // Fine texture (period ~10 px) in one quadrant.
            if (u > 0.65f && v > 0.55f) {
                const float t = 0.08f * std::sin(static_cast<float>(x) * 0.6f) * std::sin(static_cast<float>(y) * 0.45f);
                r += t;
                g += t;
                b += t;
            }
            float* p = &image[(std::size_t{y} * width + x) * 4];
            p[0] = std::max(r, 0.04f);
            p[1] = std::max(g, 0.04f);
            p[2] = std::max(b, 0.04f);
            p[3] = 1.0f;
        }
    }
    return image;
}

// Adds Gaussian noise of standard deviation `sigma` to each channel in the
// stabilised (sqrt) domain: shot-noise-like, stronger in highlights in
// linear terms, exactly what the estimator is meant to measure.
inline std::vector<float> addStabilizedNoise(const std::vector<float>& clean, float sigma, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, sigma);
    std::vector<float> noisy = clean;
    for (std::size_t i = 0; i < noisy.size(); ++i) {
        if (i % 4 == 3) continue;
        const float s = std::sqrt(std::max(noisy[i], 0.0f)) + noise(rng);
        noisy[i] = s > 0.0f ? s * s : 0.0f;
    }
    return noisy;
}

// PSNR over RGB with peak 1.0.
inline double psnr(const std::vector<float>& a, const std::vector<float>& b) {
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i % 4 == 3) continue;
        const double d = double(a[i]) - double(b[i]);
        sum += d * d;
        ++count;
    }
    const double mse = sum / static_cast<double>(std::max<std::size_t>(count, 1));
    return mse > 0.0 ? 10.0 * std::log10(1.0 / mse) : 200.0;
}

// Rounds every value to fp16 and back, like uploading to an RGBA16F texture.
template <class F>
inline std::vector<float> quantizeHalf(const std::vector<float>& values, F roundTrip) {
    std::vector<float> out(values.size());
    std::transform(values.begin(), values.end(), out.begin(), roundTrip);
    return out;
}

}  // namespace darkhouse::test
