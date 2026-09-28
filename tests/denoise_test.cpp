// Denoise reference tests: configuration constants, noise estimation,
// reconstruction exactness, quality (PSNR) and clean-image preservation.
//
// Usage: darkhouse_denoise_test [--measure]
//   --measure  print the measured per-band white-noise factors
//              (DH_DENOISE_LEVEL_NOISE) and exit

#include "denoise.hpp"
#include "denoise_test_images.hpp"
#include "layer_stack.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string_view>

using namespace darkhouse;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::cout << "FAIL line " << __LINE__ << ": " #condition << '\n';              \
        }                                                                                  \
    } while (false)

float roundTripHalf(float value) { return halfToFloat(floatToHalf(value)); }

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--measure") {
        const auto measured = denoise_detail::measureLevelNoise(1024, 1);
        std::printf("#define DH_DENOISE_LEVEL_NOISE");
        for (std::size_t k = 0; k < measured.size(); ++k) std::printf("%s %.4f", k ? "," : "", measured[k]);
        std::printf("\n");
        return 0;
    }

    // 1. The per-band white-noise constants match the pyramid they describe.
    {
        constexpr std::array<float, DH_DENOISE_MAX_LEVELS> configured{DH_DENOISE_LEVEL_NOISE};
        const auto measured = denoise_detail::measureLevelNoise(1024, 7);
        for (std::size_t k = 0; k < configured.size(); ++k) {
            std::printf("band %zu: configured %.4f measured %.4f\n", k, configured[k], measured[k]);
            CHECK(std::fabs(configured[k] - measured[k]) <= 0.03f * measured[k]);
        }
    }

    // 2. Level geometry.
    CHECK(denoiseLevelCount(6000, 4000) == DH_DENOISE_MAX_LEVELS);
    CHECK(denoiseLevelCount(64, 64) == 4);
    CHECK(denoiseLevelCount(3, 1) == 1);
    CHECK((denoiseLevelSize(6000, 4001, 1) == std::array<std::uint32_t, 2>{3000, 2001}));
    CHECK((denoiseLevelSize(5, 5, 2) == std::array<std::uint32_t, 2>{2, 2}));

    // 3. Histogram median estimator on pure Gaussian samples.
    {
        std::mt19937 rng(3);
        std::normal_distribution<float> noise(0.0f, 0.03f);
        std::array<std::uint32_t, DH_DENOISE_HIST_BINS> bins{};
        for (int i = 0; i < 200000; ++i) ++bins[denoise_detail::histogramBin(std::fabs(noise(rng)))];
        const float sigma = denoise_detail::histogramMedian(bins) * static_cast<float>(DH_DENOISE_MAD_TO_SIGMA);
        std::printf("histogram sigma: %.5f (true 0.03)\n", sigma);
        CHECK(std::fabs(sigma / 0.03f - 1.0f) < 0.03f);
        std::array<std::uint32_t, DH_DENOISE_HIST_BINS> empty{};
        CHECK(denoise_detail::histogramMedian(empty) == 0.0f);
    }

    constexpr std::uint32_t kWidth = 512;
    constexpr std::uint32_t kHeight = 384;
    const std::vector<float> clean = test::quantizeHalf(test::makeCleanImage(kWidth, kHeight), roundTripHalf);

    // 4. Strength 0 is an exact copy.
    {
        std::vector<float> out;
        DenoiseParams off;
        off.luminance = off.chrominance = 0.0f;
        denoiseReference(clean, kWidth, kHeight, off, out);
        CHECK(out == clean);
    }

    // 5. With negligible thresholds the pyramid reconstructs its input (up to
    //    fp16 storage of the coarse levels and the sqrt/square round trip).
    {
        DenoiseParams tiny;
        tiny.luminance = tiny.chrominance = 1.0f;
        tiny.detail = 0.0f;
        tiny.noiseLevel = 1e-9f;
        std::vector<float> out;
        denoiseReference(clean, kWidth, kHeight, tiny, out);
        float worst = 0.0f;
        for (std::size_t i = 0; i < out.size(); ++i) {
            worst = std::max(worst, std::fabs(out[i] - clean[i]) / std::max(clean[i], 0.05f));
        }
        std::printf("near-identity worst relative error: %.5f\n", worst);
        CHECK(worst < 4e-3f);
    }

    // 6. Noise estimate and quality on a noisy image.
    {
        constexpr float kSigma = 0.02f;  // stabilised domain
        const std::vector<float> noisy =
            test::quantizeHalf(test::addStabilizedNoise(clean, kSigma, 11), roundTripHalf);
        DenoiseParams params;
        params.luminance = 0.8f;
        params.chrominance = 0.9f;
        params.detail = 0.0f;
        std::vector<float> out;
        const DenoiseStatistics stats = denoiseReference(noisy, kWidth, kHeight, params, out);
        std::printf("estimated sigma: Y %.5f C1 %.5f C2 %.5f (true %.5f), levels %u\n", stats.sigma[0],
                    stats.sigma[1], stats.sigma[2], kSigma, stats.levels);
        for (float sigma : stats.sigma) CHECK(std::fabs(sigma - kSigma) < 0.12f * kSigma);
        const double before = test::psnr(noisy, clean);
        const double after = test::psnr(out, clean);
        std::printf("PSNR noisy %.2f dB -> denoised %.2f dB (+%.2f dB)\n", before, after, after - before);
        CHECK(after - before > 5.0);
    }

    // 7. A clean image is left essentially alone (no plastic look).
    {
        std::vector<float> out;
        const DenoiseStatistics stats = denoiseReference(clean, kWidth, kHeight, DenoiseParams{}, out);
        const double fidelity = test::psnr(out, clean);
        std::printf("clean image: estimated sigma Y %.5f, PSNR vs input %.2f dB\n", stats.sigma[0], fidelity);
        CHECK(fidelity > 40.0);
    }

    std::cout << (g_failures == 0 ? "PASS" : "FAILED") << '\n';
    return g_failures == 0 ? 0 : 1;
}
