// darkhouse_denoise_bench — GPU noise-reduction throughput.
//
// Runs the denoise node on a synthetic noisy image and reports its GPU time
// (timestamp queries; wall-clock submit-and-wait on devices without them),
// next to the exposure node, a single-pass, bandwidth-bound baseline for the
// same image. --cpu adds the single-threaded CPU reference.
//
// Usage: darkhouse_denoise_bench [--size WxH] [--iterations N] [--shaders DIR] [--cpu]
// Exit codes: 0 done, 1 error, 2 bad arguments, 77 no Vulkan 1.3 device.

#include "denoise.hpp"
#include "denoise_node.hpp"
#include "denoise_test_images.hpp"
#include "layer_stack.hpp"
#include "render_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace darkhouse;

namespace {

struct Options {
    std::uint32_t width = 4096;
    std::uint32_t height = 2731;  // ~11 MP, a 3:2 frame at a 4K preview size
    int iterations = 20;
    std::string shaders = DARKHOUSE_SHADER_DIR;
    bool cpu = false;
};

struct Stats {
    double min = 0.0;
    double median = 0.0;
};

Stats summarize(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return {samples.front(), samples[samples.size() / 2]};
}

// GPU milliseconds of `node` evaluated alone on `input`, one sample per iteration
// (after a warm-up run that also allocates the node's images).
std::vector<double> timeNode(VulkanContext& gpu, std::unique_ptr<ComputeNode> node, GPUTexture& input, int iterations,
                             bool& usedTimestamps) {
    RenderPipelineGraph graph;
    graph.enableProfiling(gpu);
    const RenderPipelineGraph::NodeId id = graph.addNode(std::move(node));
    graph.bindExternalInput(id, 0, input);
    std::vector<double> samples;
    for (int i = 0; i <= iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        gpu.submitAndWait([&](VkCommandBuffer commandBuffer) { graph.evaluateGraph(commandBuffer); });
        const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const std::vector<RenderPipelineGraph::NodeTiming> timings = graph.readTimings();
        usedTimestamps = !timings.empty();
        if (i > 0) samples.push_back(usedTimestamps ? timings.front().milliseconds : wall);
    }
    return samples;
}

int usage(const char* why) {
    std::cerr << "darkhouse_denoise_bench: " << why
              << "\nusage: darkhouse_denoise_bench [--size WxH] [--iterations N] [--shaders DIR] [--cpu]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (arg == "--size" && value) {
            unsigned w = 0, h = 0;
            if (std::sscanf(value, "%ux%u", &w, &h) != 2 || w == 0 || h == 0 || w > 16384 || h > 16384) {
                return usage("--size needs WxH, each 1..16384");
            }
            options.width = w;
            options.height = h;
            ++i;
        } else if (arg == "--iterations" && value) {
            options.iterations = std::atoi(value);
            if (options.iterations < 1) return usage("--iterations needs a positive number");
            ++i;
        } else if (arg == "--shaders" && value) {
            options.shaders = value;
            ++i;
        } else if (arg == "--cpu") {
            options.cpu = true;
        } else {
            return usage("unknown argument");
        }
    }

    std::unique_ptr<VulkanContext> gpu;
    try {
        VulkanContextOptions contextOptions;
        contextOptions.applicationName = "DarkHouse denoise benchmark";
        contextOptions.enableValidation = false;  // timing run
        gpu = std::make_unique<VulkanContext>(contextOptions);
    } catch (const std::exception& e) {
        std::cout << "SKIP: no Vulkan 1.3 device: " << e.what() << '\n';
        return 77;
    }

    try {
        const std::uint32_t w = options.width;
        const std::uint32_t h = options.height;
        const double megapixels = static_cast<double>(w) * h / 1e6;
        std::printf("device   %s\nimage    %u x %u (%.1f MP), noisy synthetic, %d iterations\n", gpu->deviceName().c_str(),
                    w, h, megapixels, options.iterations);

        const std::vector<float> clean = test::makeCleanImage(w, h);
        const std::vector<float> noisy = test::addStabilizedNoise(clean, 0.02f, 3);
        std::vector<std::uint16_t> halves(noisy.size());
        floatsToHalves(noisy.data(), halves.data(), noisy.size());
        GPUTexture input = gpu->createTexture(w, h, PixelFormat::R16G16B16A16_SFLOAT,
                                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        const VulkanContext::RegionUpload upload{0, 0, w, h, std::as_bytes(std::span<const std::uint16_t>(halves))};
        gpu->uploadRegions(input, std::span<const VulkanContext::RegionUpload>(&upload, 1));

        bool timestamps = false;
        auto report = [&](const char* name, const std::vector<double>& samples, const char* note) {
            const Stats stats = summarize(samples);
            std::printf("%-9s median %8.2f ms   min %8.2f ms   %7.1f MP/s   %s\n", name, stats.median, stats.min,
                        megapixels / stats.median * 1e3, note);
            return stats;
        };

        auto denoise = std::make_unique<DenoiseNode>(*gpu, options.shaders);
        denoise->setParams(DenoiseParams{});
        const Stats denoiseStats =
            report("denoise", timeNode(*gpu, std::move(denoise), input, options.iterations, timestamps),
                   ("5 passes, " + std::to_string(denoiseLevelCount(w, h)) + " levels").c_str());
        const Stats exposureStats = report(
            "exposure", timeNode(*gpu, std::make_unique<ExposureNode>(*gpu, options.shaders), input, options.iterations, timestamps),
            "1 pass: read + write once (bandwidth baseline)");
        std::printf("         denoise costs %.1fx one full-resolution read+write pass\n",
                    denoiseStats.median / std::max(exposureStats.median, 1e-6));
        std::printf("timing   %s\n", timestamps ? "GPU timestamps" : "wall clock around submit-and-wait (no timestamps)");

        if (options.cpu) {
            std::vector<float> out;
            const auto start = std::chrono::steady_clock::now();
            (void)denoiseReference(noisy, w, h, DenoiseParams{}, out);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::printf("cpu ref  %8.2f ms (%.1f MP/s, single thread) -> GPU speed-up %.0fx\n", ms, megapixels / ms * 1e3,
                        ms / std::max(denoiseStats.median, 1e-6));
        }
        gpu->destroyTexture(input);
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
