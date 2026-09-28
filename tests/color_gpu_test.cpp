// GPU colour nodes against their CPU references (color_adjust.hpp,
// tone_curve.hpp, lens_correction.hpp): white balance, HSL colour mixer,
// colour grading, tone curves and lens corrections, on an image that sweeps hue,
// chroma and lightness and includes greys, black and HDR values. Runs under
// validation; any validation error fails it.
//
// Exit codes: 0 pass, 1 fail, 77 skipped (no Vulkan 1.3 device).

#include "color_adjust.hpp"
#include "color_nodes.hpp"
#include "layer_stack.hpp"
#include "lens_correction.hpp"
#include "render_pipeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

constexpr std::uint32_t kWidth = 97;
constexpr std::uint32_t kHeight = 61;

// Rows sweep lightness, columns hue; chroma varies with the column block.
// The first rows are greys, black and HDR highlights. Alpha varies too.
std::vector<float> makeImage() {
    std::vector<float> rgba(std::size_t{kWidth} * kHeight * 4);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            Rgb rgb;
            if (y == 0) {
                const float v = static_cast<float>(x) / (kWidth - 1);
                rgb = {v, v, v};  // grey ramp including black
            } else if (y == 1) {
                const float v = 1.0f + 6.0f * static_cast<float>(x) / (kWidth - 1);
                rgb = {v, v * 0.7f, v * 0.4f};  // HDR
            } else {
                const float lightness = 0.08f + 0.9f * static_cast<float>(y - 2) / (kHeight - 3);
                const float hue = 360.0f * static_cast<float>(x) / kWidth;
                const float chroma = 0.02f + 0.2f * static_cast<float>(x % 7) / 6.0f;
                const float angle = hue * 3.14159265f / 180.0f;
                rgb = oklabToLinearSrgb({lightness, chroma * std::cos(angle), chroma * std::sin(angle)});
            }
            float* p = &rgba[(std::size_t{y} * kWidth + x) * 4];
            for (int c = 0; c < 3; ++c) p[c] = std::max(rgb[static_cast<std::size_t>(c)], 0.0f);
            p[3] = 0.25f + 0.75f * static_cast<float>((x + y) % 4) / 3.0f;
        }
    }
    // The GPU reads FP16: quantize so both sides start from the same values.
    for (float& v : rgba) v = halfToFloat(floatToHalf(v));
    return rgba;
}

std::vector<float> runNode(VulkanContext& gpu, std::unique_ptr<ComputeNode> node, const std::vector<float>& rgba) {
    GPUTexture input = gpu.createTexture(kWidth, kHeight, PixelFormat::R16G16B16A16_SFLOAT,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    std::vector<std::uint16_t> halves(rgba.size());
    floatsToHalves(rgba.data(), halves.data(), rgba.size());
    const VulkanContext::RegionUpload upload{0, 0, kWidth, kHeight, std::as_bytes(std::span<const std::uint16_t>(halves))};
    gpu.uploadRegions(input, std::span<const VulkanContext::RegionUpload>(&upload, 1));

    std::vector<float> out;
    {
        RenderPipelineGraph graph;
        const RenderPipelineGraph::NodeId id = graph.addNode(std::move(node));
        graph.bindExternalInput(id, 0, input);
        gpu.submitAndWait([&](VkCommandBuffer cmd) { graph.evaluateGraph(cmd); });
        GPUTexture output = graph.outputOf(id);
        const std::vector<std::byte> bytes = gpu.downloadTexture(output);
        std::vector<std::uint16_t> outHalves(bytes.size() / 2);
        std::memcpy(outHalves.data(), bytes.data(), bytes.size());
        out.resize(outHalves.size());
        halvesToFloats(outHalves.data(), out.data(), outHalves.size());
    }
    gpu.destroyTexture(input);
    return out;
}

// GPU vs CPU: FP16 storage (which may round toward zero) plus fp32 transcendental differences.
void compare(const char* name, const std::vector<float>& gpu, const std::vector<float>& input,
             const std::function<Rgb(const Rgb&)>& reference) {
    std::size_t outliers = 0;
    double worst = 0.0, sum = 0.0;
    for (std::size_t p = 0; p * 4 < input.size(); ++p) {
        const Rgb expected = reference({input[p * 4], input[p * 4 + 1], input[p * 4 + 2]});
        for (int c = 0; c < 3; ++c) {
            const double diff = std::fabs(double(gpu[p * 4 + c]) - expected[static_cast<std::size_t>(c)]);
            const double tolerance = 3e-3 * std::fabs(expected[static_cast<std::size_t>(c)]) + 2e-4;
            worst = std::max(worst, diff);
            sum += diff;
            if (diff > tolerance) ++outliers;
        }
        if (gpu[p * 4 + 3] != input[p * 4 + 3]) ++outliers;  // alpha passes through untouched
    }
    std::printf("  %-34s mean |gpu-cpu| %.2e, worst %.2e, outliers %zu\n", name, sum / double(input.size()), worst,
                outliers);
    CHECK(outliers == 0);
}

// Whole-image comparison for resampling nodes: every channel, alpha included.
void compareImages(const char* name, const std::vector<float>& gpu, const std::vector<float>& cpu) {
    std::size_t outliers = 0;
    double worst = 0.0, sum = 0.0;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        const double diff = std::fabs(double(gpu[i]) - cpu[i]);
        worst = std::max(worst, diff);
        sum += diff;
        if (diff > 3e-3 * std::fabs(cpu[i]) + 5e-4) ++outliers;
    }
    std::printf("  %-34s mean |gpu-cpu| %.2e, worst %.2e, outliers %zu\n", name, sum / double(cpu.size()), worst, outliers);
    CHECK(outliers == 0);
}

}  // namespace

int main() {
    std::unique_ptr<VulkanContext> gpu;
    try {
        VulkanContextOptions options;
        options.applicationName = "DarkHouse colour GPU test";
        options.enableValidation = true;
        gpu = std::make_unique<VulkanContext>(options);
    } catch (const std::exception& e) {
        std::cout << "SKIP: no Vulkan 1.3 device: " << e.what() << '\n';
        return 77;
    }
    std::printf("device: %s%s\n", gpu->deviceName().c_str(), gpu->validationEnabled() ? " (validation on)" : "");
    const std::vector<float> image = makeImage();

    try {
        std::printf("white balance\n");
        for (const WhiteBalanceParams& p : {WhiteBalanceParams{}, WhiteBalanceParams{3200.0f, 0.0f},
                                            WhiteBalanceParams{11000.0f, -60.0f}, WhiteBalanceParams{4800.0f, 150.0f}}) {
            auto node = std::make_unique<WhiteBalanceNode>(*gpu, DARKHOUSE_SHADER_DIR);
            node->setParams(p);
            const WhiteBalancePush push = whiteBalancePush(p);
            const std::string name = std::to_string(static_cast<int>(p.temperature)) + " K, tint " +
                                     std::to_string(static_cast<int>(p.tint));
            compare(name.c_str(), runNode(*gpu, std::move(node), image), image,
                    [&](const Rgb& c) { return applyWhiteBalance(push, c); });
        }

        std::printf("hsl\n");
        HslParams mixed;
        mixed.hue = {40.0f, -80.0f, 100.0f, 20.0f, -100.0f, 60.0f, 0.0f, -30.0f};
        mixed.saturation = {-100.0f, 50.0f, 20.0f, -40.0f, 100.0f, -60.0f, 30.0f, 0.0f};
        mixed.luminance = {30.0f, -50.0f, 100.0f, -100.0f, 10.0f, 70.0f, -20.0f, 45.0f};
        for (const HslParams& p : {HslParams{}, mixed}) {
            auto node = std::make_unique<HslNode>(*gpu, DARKHOUSE_SHADER_DIR);
            node->setParams(p);
            compare(isIdentity(p) ? "identity" : "all bands", runNode(*gpu, std::move(node), image), image,
                    [&](const Rgb& c) { return applyHsl(p, c); });
        }

        std::printf("color grading\n");
        ColorGradingParams grade;
        grade.vibrance = 40.0f;
        grade.saturation = -20.0f;
        grade.shadows = {250.0f, 80.0f, 20.0f};
        grade.midtones = {40.0f, 30.0f, -10.0f};
        grade.highlights = {70.0f, 60.0f, 30.0f};
        grade.global = {150.0f, 10.0f, 5.0f};
        grade.blending = 20.0f;
        grade.balance = 35.0f;
        ColorGradingParams presenceOnly;
        presenceOnly.vibrance = -70.0f;
        presenceOnly.saturation = 80.0f;
        for (const ColorGradingParams& p : {ColorGradingParams{}, grade, presenceOnly}) {
            auto node = std::make_unique<ColorGradingNode>(*gpu, DARKHOUSE_SHADER_DIR);
            node->setParams(p);
            const ColorGradingPush push = colorGradingPush(p);
            compare(isIdentity(p) ? "identity" : p.shadows.saturation > 0.0f ? "wheels + presence" : "presence",
                    runNode(*gpu, std::move(node), image), image, [&](const Rgb& c) { return applyColorGrading(push, c); });
        }

        std::printf("tone curve\n");
        ToneCurveParams contrast;
        contrast.curves[0] = curvePreset(CurvePreset::STRONG_CONTRAST);
        ToneCurveParams channels = contrast;
        channels.curves[0] = curvePreset(CurvePreset::FADED);
        channels.curves[1].count = 3;
        channels.curves[1].points = {CurvePoint{0.0f, 0.05f}, CurvePoint{0.4f, 0.55f}, CurvePoint{1.0f, 1.0f}};
        channels.curves[3].count = 4;
        channels.curves[3].points = {CurvePoint{0.0f, 0.0f}, CurvePoint{0.3f, 0.2f}, CurvePoint{0.7f, 0.75f},
                                     CurvePoint{0.9f, 0.85f}};
        for (const ToneCurveParams& p : {ToneCurveParams{}, contrast, channels}) {
            auto node = std::make_unique<ToneCurveNode>(*gpu, DARKHOUSE_SHADER_DIR);
            node->setParams(p);
            const ToneCurveTables tables = node->tables();
            compare(isIdentity(p) ? "identity" : p.curves[1].count ? "composite + channels" : "strong contrast",
                    runNode(*gpu, std::move(node), image), image, [&](const Rgb& c) { return applyToneCurve(tables, c); });
        }
        {
            // A parameter change re-uploads the tables on the next evaluation.
            auto node = std::make_unique<ToneCurveNode>(*gpu, DARKHOUSE_SHADER_DIR);
            ToneCurveNode& curve = *node;
            RenderPipelineGraph graph;
            GPUTexture source = gpu->createTexture(kWidth, kHeight, PixelFormat::R16G16B16A16_SFLOAT,
                                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            std::vector<std::uint16_t> halves(image.size());
            floatsToHalves(image.data(), halves.data(), image.size());
            const VulkanContext::RegionUpload upload{0, 0, kWidth, kHeight,
                                                     std::as_bytes(std::span<const std::uint16_t>(halves))};
            gpu->uploadRegions(source, std::span<const VulkanContext::RegionUpload>(&upload, 1));
            const RenderPipelineGraph::NodeId id = graph.addNode(std::move(node));
            graph.bindExternalInput(id, 0, source);
            gpu->submitAndWait([&](VkCommandBuffer cmd) { graph.evaluateGraph(cmd); });
            curve.setParams(contrast);
            gpu->submitAndWait([&](VkCommandBuffer cmd) { graph.evaluateGraph(cmd); });
            GPUTexture output = graph.outputOf(id);
            const std::vector<std::byte> bytes = gpu->downloadTexture(output);
            std::vector<std::uint16_t> out(bytes.size() / 2);
            std::memcpy(out.data(), bytes.data(), bytes.size());
            std::vector<float> result(out.size());
            halvesToFloats(out.data(), result.data(), out.size());
            const ToneCurveTables tables = curve.tables();
            compare("re-evaluated after an edit", result, image, [&](const Rgb& c) { return applyToneCurve(tables, c); });
            graph.clear();
            gpu->destroyTexture(source);
        }

        std::printf("lens correction\n");
        LensCorrectionParams profileOnly;
        ResolvedLensProfile profile;
        profile.name = "test profile";
        profile.distortionModel = DistortionModel::PTLENS;
        profile.distortion = {0.01f, -0.06f, 0.02f};
        profile.tcaModel = TcaModel::POLY3;
        profile.tcaRed = {1.002f, 0.0f, 0.001f};
        profile.tcaBlue = {0.998f, 0.0f, -0.001f};
        profile.hasVignetting = true;
        profile.vignetting = {-0.6f, 0.2f, -0.05f};
        applyProfile(profileOnly, profile);
        LensCorrectionParams everything = profileOnly;
        everything.distortionAmount = 150.0f;
        everything.manualDistortion = -30.0f;
        everything.manualVignetting = 40.0f;
        everything.manualCaRed = 60.0f;
        everything.manualCaBlue = -60.0f;
        everything.scale = 90.0f;  // zoomed out: clamped edges get sampled
        LensCorrectionParams poly5;
        profile.distortionModel = DistortionModel::POLY5;
        profile.distortion = {0.03f, -0.01f, 0.0f};
        profile.tcaModel = TcaModel::LINEAR;
        applyProfile(poly5, profile);
        const std::pair<const char*, LensCorrectionParams> lensCases[] = {
            {"identity", LensCorrectionParams{}}, {"profile (ptlens, poly3 TCA, pa)", profileOnly},
            {"profile + manual, scale 90 %", everything}, {"poly5 + linear TCA", poly5}};
        for (const auto& [name, p] : lensCases) {
            auto node = std::make_unique<LensCorrectionNode>(*gpu, DARKHOUSE_SHADER_DIR);
            node->setParams(p);
            const std::vector<float> result = runNode(*gpu, std::move(node), image);
            compareImages(name, result, applyLensCorrection(lensCorrectionPush(p, kWidth, kHeight), kWidth, kHeight, image));
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        return 1;
    }

    gpu->waitIdle();
    const std::uint32_t errors = gpu->validationErrorCount();
    if (errors > 0) std::cout << "FAIL: " << errors << " validation error(s)\n";
    if (g_failures || errors) return 1;
    std::cout << "colour GPU nodes: all checks passed\n";
    return 0;
}
