// GPU denoise tests: runs DenoiseNode through a RenderPipelineGraph on the
// real device and compares it with the CPU reference (denoiseReference),
// plus quality, the bypass path, odd sizes and validation cleanliness.
//
// Exit codes: 0 pass, 1 fail, 77 skipped (no Vulkan 1.3 device).

#include "denoise.hpp"
#include "denoise_node.hpp"
#include "denoise_test_images.hpp"
#include "layer_stack.hpp"
#include "render_pipeline.hpp"
#include "vulkan_context.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef DARKHOUSE_SHADER_DIR
#define DARKHOUSE_SHADER_DIR "shaders"
#endif

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

struct GpuResult {
    std::vector<float> pixels;
    DenoiseStatistics stats;
};

GpuResult runOnGpu(VulkanContext& gpu, const std::vector<float>& rgba, std::uint32_t width, std::uint32_t height,
                   const DenoiseParams& params) {
    GPUTexture input = gpu.createTexture(width, height, PixelFormat::R16G16B16A16_SFLOAT,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    std::vector<std::uint16_t> halves(rgba.size());
    for (std::size_t i = 0; i < rgba.size(); ++i) halves[i] = floatToHalf(rgba[i]);
    const VulkanContext::RegionUpload upload{0, 0, width, height, std::as_bytes(std::span<const std::uint16_t>(halves))};
    gpu.uploadRegions(input, std::span<const VulkanContext::RegionUpload>(&upload, 1));

    GpuResult result;
    {
        RenderPipelineGraph graph;
        auto node = std::make_unique<DenoiseNode>(gpu, DARKHOUSE_SHADER_DIR);
        node->setParams(params);
        DenoiseNode* denoise = node.get();
        const RenderPipelineGraph::NodeId id = graph.addNode(std::move(node));
        graph.bindExternalInput(id, 0, input);
        gpu.submitAndWait([&](VkCommandBuffer cmd) { graph.evaluateGraph(cmd); });
        result.stats = denoise->statistics();

        GPUTexture output = graph.outputOf(id);  // handle copy; the node keeps ownership
        const std::vector<std::byte> bytes = gpu.downloadTexture(output);
        std::vector<std::uint16_t> outHalves(bytes.size() / 2);
        std::memcpy(outHalves.data(), bytes.data(), bytes.size());
        result.pixels.resize(outHalves.size());
        for (std::size_t i = 0; i < outHalves.size(); ++i) result.pixels[i] = halfToFloat(outHalves[i]);
    }
    gpu.destroyTexture(input);
    return result;
}

// Compares GPU and CPU output; differences come from fp32 rounding (fused
// multiply-adds, sqrt/exp2 precision) ahead of fp16 storage.
void compare(const char* name, const std::vector<float>& gpu, const std::vector<float>& cpu) {
    double sumDiff = 0.0;
    double signedSum = 0.0;
    float worst = 0.0f;
    std::size_t outliers = 0;
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        const float diff = std::fabs(gpu[i] - cpu[i]);
        sumDiff += diff;
        signedSum += double(gpu[i]) - double(cpu[i]);
        worst = std::max(worst, diff);
        if (diff > 3e-3f * std::max(1.0f, std::fabs(cpu[i]))) ++outliers;  // ~3 fp16 ulps
    }
    const double meanDiff = sumDiff / static_cast<double>(std::max<std::size_t>(gpu.size(), 1));
    std::printf("  %-22s mean |gpu-cpu| %.2e (signed %+.2e), worst %.2e, outliers %zu / %zu\n", name, meanDiff,
                signedSum / static_cast<double>(std::max<std::size_t>(gpu.size(), 1)), worst, outliers, gpu.size());
    CHECK(gpu.size() == cpu.size());
    CHECK(meanDiff < 1e-4);
    CHECK(outliers <= gpu.size() / 10000);  // allow isolated rounding differences
}

}  // namespace

int main() {
    std::unique_ptr<VulkanContext> gpu;
    try {
        VulkanContextOptions options;
        options.applicationName = "DarkHouse denoise GPU test";
        options.enableValidation = true;
        gpu = std::make_unique<VulkanContext>(options);
    } catch (const std::exception& e) {
        std::cout << "SKIP: no Vulkan 1.3 device: " << e.what() << '\n';
        return 77;
    }
    std::printf("device: %s%s\n", gpu->deviceName().c_str(), gpu->validationEnabled() ? " (validation on)" : "");

    try {
        struct Case {
            const char* name;
            std::uint32_t width, height;
            float sigma;  // added noise (stabilised domain); 0 = clean
            DenoiseParams params;
            bool checkQuality;
        };
        DenoiseParams strong;
        strong.luminance = 0.8f;
        strong.chrominance = 0.9f;
        strong.detail = 0.3f;
        DenoiseParams manual;
        manual.noiseLevel = 0.01f;
        DenoiseParams off;
        off.luminance = off.chrominance = 0.0f;
        const Case cases[] = {
            {"noisy 512x384", 512, 384, 0.02f, strong, true},
            {"odd size 301x173", 301, 173, 0.015f, DenoiseParams{}, false},
            {"manual noise level", 256, 200, 0.02f, manual, false},
            {"tiny 7x5 (1 level)", 7, 5, 0.02f, strong, false},
            {"bypass (strength 0)", 128, 96, 0.02f, off, false},
        };

        for (const Case& c : cases) {
            std::printf("%s\n", c.name);
            const std::vector<float> clean = test::quantizeHalf(test::makeCleanImage(c.width, c.height), roundTripHalf);
            const std::vector<float> input =
                c.sigma > 0.0f ? test::quantizeHalf(test::addStabilizedNoise(clean, c.sigma, 5), roundTripHalf) : clean;

            std::vector<float> reference;
            const DenoiseStatistics cpuStats = denoiseReference(input, c.width, c.height, c.params, reference);
            const GpuResult result = runOnGpu(*gpu, input, c.width, c.height, c.params);

            if (isIdentity(c.params)) {
                CHECK(result.pixels == input);  // plain copy
                CHECK(result.stats.sigma[0] == 0.0f);
                continue;
            }
            std::printf("  sigma gpu %.5f %.5f %.5f | cpu %.5f %.5f %.5f | levels %u\n", result.stats.sigma[0],
                        result.stats.sigma[1], result.stats.sigma[2], cpuStats.sigma[0], cpuStats.sigma[1],
                        cpuStats.sigma[2], result.stats.levels);
            CHECK(result.stats.levels == cpuStats.levels);
            for (std::size_t ch = 0; ch < 3; ++ch) {
                CHECK(std::fabs(result.stats.sigma[ch] - cpuStats.sigma[ch]) <= 0.01f * cpuStats.sigma[ch] + 1e-7f);
            }
            compare("output vs reference", result.pixels, reference);
            if (c.checkQuality) {
                const double before = test::psnr(input, clean);
                const double after = test::psnr(result.pixels, clean);
                std::printf("  PSNR %.2f dB -> %.2f dB (+%.2f dB)\n", before, after, after - before);
                CHECK(after - before > 4.0);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        return 1;
    }

    gpu->waitIdle();
    const std::uint32_t errors = gpu->validationErrorCount();
    if (errors > 0) std::cout << "FAIL: " << errors << " validation error(s)\n";
    CHECK(errors == 0);
    std::cout << (g_failures == 0 ? "PASS" : "FAILED") << '\n';
    return g_failures == 0 ? 0 : 1;
}
