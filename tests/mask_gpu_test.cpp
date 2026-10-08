// GPU masks and local adjustments against the CPU reference (mask_engine.hpp):
// every component shape and combine mode, inverted masks, 6 masks across two
// array layers, the adjusted image, incremental brush strokes (only new dabs
// uploaded), undo (full repaint) and the overlay. Runs under validation and
// synchronization validation; any validation error fails it.
//
// Exit codes: 0 pass, 1 fail, 77 skipped (no Vulkan 1.3 device).

#include "layer_stack.hpp"
#include "local_adjust_node.hpp"
#include "mask_engine.hpp"
#include "render_pipeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
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

constexpr std::uint32_t kWidth = 160;
constexpr std::uint32_t kHeight = 100;

// A blue "sky" over warm ground, with a grey ramp and saturated patches.
std::vector<float> makeImage() {
    std::vector<float> rgba(std::size_t{kWidth} * kHeight * 4);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const float u = static_cast<float>(x) / (kWidth - 1), v = static_cast<float>(y) / (kHeight - 1);
            Rgb rgb = v < 0.4f ? Rgb{0.15f + 0.1f * u, 0.3f + 0.1f * u, 0.8f} : Rgb{0.5f * u + 0.02f, 0.3f * u + 0.02f, 0.1f * u};
            if (x % 40 < 8 && y > 60) rgb = {0.8f, 0.05f, 0.05f};  // red patches
            float* p = &rgba[(std::size_t{y} * kWidth + x) * 4];
            p[0] = rgb[0];
            p[1] = rgb[1];
            p[2] = rgb[2];
            p[3] = 1.0f;
        }
    }
    for (float& v : rgba) v = halfToFloat(floatToHalf(v));
    return rgba;
}

LocalAdjustments makeMasks() {
    LocalAdjustments all;
    auto component = [](MaskShape shape, MaskMode mode = MaskMode::ADD) {
        MaskComponent c;
        c.shape = shape;
        c.mode = mode;
        return c;
    };

    LocalAdjustment brush;  // painted, with an erased hole
    brush.name = "Brush";
    MaskComponent b = component(MaskShape::BRUSH);
    b.dabs = {{0.3f, 0.5f, 0.12f, 0.5f, 1.0f, false}, {0.35f, 0.55f, 0.1f, 0.3f, 0.7f, false},
              {0.3f, 0.5f, 0.03f, 0.0f, 1.0f, true}};
    brush.components = {b};
    brush.params.exposure = 1.0f;
    all.masks.push_back(brush);

    LocalAdjustment gradients;  // radial minus a linear gradient, intersected with a luminance range
    gradients.name = "Gradients";
    MaskComponent radial = component(MaskShape::RADIAL_GRADIENT);
    radial.start = {0.6f, 0.6f};
    radial.size = {0.3f, 0.35f};
    radial.angle = 30.0f;
    radial.feather = 0.4f;
    MaskComponent linear = component(MaskShape::LINEAR_GRADIENT, MaskMode::SUBTRACT);
    linear.start = {0.9f, 0.2f};
    linear.end = {0.7f, 0.5f};
    linear.opacity = 0.8f;
    MaskComponent range = component(MaskShape::LUMINANCE_RANGE, MaskMode::INTERSECT);
    range.low = 0.2f;
    range.high = 0.7f;
    range.falloff = 0.1f;
    gradients.components = {radial, linear, range};
    gradients.params.saturation = -60.0f;
    gradients.params.temperature = 40.0f;
    gradients.amount = 0.8f;
    all.masks.push_back(gradients);

    LocalAdjustment colour;  // the red patches
    colour.name = "Colour";
    MaskComponent c = component(MaskShape::COLOR_RANGE);
    c.hue = 29.0f;
    c.hueWidth = 50.0f;
    c.low = 0.04f;
    colour.components = {c};
    colour.params.saturation = 80.0f;
    colour.params.contrast = 30.0f;
    all.masks.push_back(colour);

    LocalAdjustment sky;
    sky.name = "Sky";
    sky.components = {component(MaskShape::SKY)};
    sky.params.exposure = -0.7f;
    sky.params.highlights = -50.0f;
    sky.params.tint = 20.0f;
    all.masks.push_back(sky);

    LocalAdjustment subject;  // inverted: everything but the subject
    subject.name = "Not subject";
    subject.invert = true;
    MaskComponent s = component(MaskShape::SUBJECT);
    s.opacity = 0.7f;
    subject.components = {s};
    subject.params.shadows = 40.0f;
    all.masks.push_back(subject);

    LocalAdjustment disabled;  // generated (5th and 6th mask: second layer) but not applied
    disabled.name = "Disabled";
    disabled.enabled = false;
    MaskComponent inverted = component(MaskShape::LINEAR_GRADIENT);
    inverted.invert = true;
    disabled.components = {inverted};
    disabled.params.exposure = 3.0f;
    all.masks.push_back(disabled);
    return all;
}

std::vector<float> toFloats(const std::vector<std::byte>& bytes) {
    std::vector<std::uint16_t> halves(bytes.size() / 2);
    std::memcpy(halves.data(), bytes.data(), bytes.size());
    std::vector<float> out(halves.size());
    halvesToFloats(halves.data(), out.data(), halves.size());
    return out;
}

struct Result {
    std::vector<float> image;
    std::vector<std::vector<float>> maskLayers;
};

class Harness {
public:
    Harness(VulkanContext& gpu, const std::vector<float>& image) : gpu_(gpu) {
        input_ = gpu.createTexture(kWidth, kHeight, PixelFormat::R16G16B16A16_SFLOAT,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        std::vector<std::uint16_t> halves(image.size());
        floatsToHalves(image.data(), halves.data(), image.size());
        const VulkanContext::RegionUpload upload{0, 0, kWidth, kHeight, std::as_bytes(std::span<const std::uint16_t>(halves))};
        gpu.uploadRegions(input_, std::span<const VulkanContext::RegionUpload>(&upload, 1));
        auto node = std::make_unique<LocalAdjustNode>(gpu, DARKHOUSE_SHADER_DIR);
        node_ = node.get();
        id_ = graph_.addNode(std::move(node));
        graph_.bindExternalInput(id_, 0, input_);
    }
    ~Harness() {
        graph_.clear();
        gpu_.destroyTexture(input_);
    }

    Result run(const LocalAdjustments& adjustments, int overlay = -1) {
        node_->updateUniforms(serialize(adjustments));
        node_->setOverlay(overlay);
        gpu_.submitAndWait([&](VkCommandBuffer cmd) { graph_.evaluateGraph(cmd); });
        Result result;
        GPUTexture output = graph_.outputOf(id_);
        result.image = toFloats(gpu_.downloadTexture(output));
        GPUTexture& masks = node_->maskTexture();
        for (std::uint32_t layer = 0; layer < masks.layers; ++layer) result.maskLayers.push_back(toFloats(gpu_.downloadTexture(masks, layer)));
        return result;
    }

private:
    VulkanContext& gpu_;
    GPUTexture input_;
    RenderPipelineGraph graph_;
    LocalAdjustNode* node_ = nullptr;
    RenderPipelineGraph::NodeId id_ = 0;
};

// GPU masks and image against the CPU reference.
void compare(const char* name, const Result& gpu, const LocalAdjustments& adjustments, const std::vector<float>& image,
             int overlay = -1) {
    const LocalAdjustments clean = sanitize(adjustments);
    std::vector<std::vector<float>> masks;
    std::size_t maskOutliers = 0;
    double maskWorst = 0.0;
    for (std::size_t i = 0; i < clean.masks.size(); ++i) {
        masks.push_back(evaluateMask(clean.masks[i], kWidth, kHeight, image));
        const std::vector<float>& layer = gpu.maskLayers.at(i / 4);
        for (std::size_t p = 0; p < masks.back().size(); ++p) {
            const double diff = std::fabs(double(layer[p * 4 + i % 4]) - masks.back()[p]);
            maskWorst = std::max(maskWorst, diff);
            if (diff > 2e-3) ++maskOutliers;
        }
    }

    std::size_t imageOutliers = 0;
    double imageWorst = 0.0;
    for (std::size_t p = 0; p * 4 < image.size(); ++p) {
        Rgb rgb{image[p * 4], image[p * 4 + 1], image[p * 4 + 2]};
        for (std::size_t i = 0; i < clean.masks.size(); ++i) {
            // The GPU blends with the FP16 mask it stored.
            const float weight = halfToFloat(floatToHalf(masks[i][p])) * clean.masks[i].amount;
            if (!clean.masks[i].enabled || weight <= 0.0f) continue;
            const Rgb adjusted = applyLocalAdjust(clean.masks[i].params, rgb);
            for (int c = 0; c < 3; ++c) rgb[c] += (adjusted[c] - rgb[c]) * weight;
        }
        if (overlay >= 0) {
            const float a = 0.6f * halfToFloat(floatToHalf(masks[static_cast<std::size_t>(overlay)][p]));
            const Rgb red{0.9f, 0.02f, 0.02f};
            for (int c = 0; c < 3; ++c) rgb[c] += (red[c] - rgb[c]) * a;
        }
        for (int c = 0; c < 3; ++c) {
            const double diff = std::fabs(double(gpu.image[p * 4 + c]) - rgb[c]);
            imageWorst = std::max(imageWorst, diff);
            if (diff > 4e-3 * std::fabs(rgb[c]) + 5e-4) ++imageOutliers;
        }
        if (gpu.image[p * 4 + 3] != image[p * 4 + 3]) ++imageOutliers;
    }
    std::printf("  %-28s masks worst %.1e (%zu outliers), image worst %.1e (%zu outliers)\n", name, maskWorst,
                maskOutliers, imageWorst, imageOutliers);
    CHECK(maskOutliers == 0);
    CHECK(imageOutliers == 0);
}

}  // namespace

int main() {
    std::unique_ptr<VulkanContext> gpu;
    try {
        VulkanContextOptions options;
        options.applicationName = "DarkHouse mask GPU test";
        options.enableValidation = true;
        gpu = std::make_unique<VulkanContext>(options);
    } catch (const std::exception& e) {
        std::cout << "SKIP: no Vulkan 1.3 device: " << e.what() << '\n';
        return 77;
    }
    std::printf("device: %s%s\n", gpu->deviceName().c_str(), gpu->validationEnabled() ? " (validation on)" : "");
    const std::vector<float> image = makeImage();

    try {
        Harness harness(*gpu, image);

        // No masks: a plain copy.
        const Result none = harness.run({});
        CHECK(none.image == image);

        LocalAdjustments masks = makeMasks();
        Result result = harness.run(masks);
        CHECK(result.maskLayers.size() == 2);  // 6 masks: 4 + 2 channels
        compare("all shapes, 6 masks", result, masks, image);
        // Every mask selects a real part of the image (not nothing, not everything)...
        for (std::size_t i = 0; i < masks.masks.size(); ++i) {
            double sum = 0.0;
            const std::vector<float>& layer = result.maskLayers[i / 4];
            for (std::size_t p = 0; p < layer.size() / 4; ++p) sum += layer[p * 4 + i % 4];
            const double coverage = sum / static_cast<double>(layer.size() / 4);
            std::printf("    mask %zu (%s): %.1f%% coverage\n", i, masks.masks[i].name.c_str(), coverage * 100.0);
            CHECK(coverage > 0.01 && coverage < 0.99);
        }
        // ... and the adjustments visibly change the photo.
        double change = 0.0;
        for (std::size_t v = 0; v < image.size(); ++v) change += std::fabs(result.image[v] - image[v]);
        CHECK(change / static_cast<double>(image.size()) > 0.01);

        // Painting continues: only the new dabs are rasterized and uploaded.
        masks.masks[0].components[0].dabs.push_back({0.7f, 0.3f, 0.08f, 0.6f, 0.8f, false});
        masks.masks[0].components[0].dabs.push_back({0.72f, 0.32f, 0.02f, 0.0f, 1.0f, true});
        compare("brush stroke added", harness.run(masks), masks, image);

        // Undo back to the first dab: the brush layer is repainted.
        masks.masks[0].components[0].dabs.resize(1);
        compare("brush undo", harness.run(masks), masks, image);

        // A second brush component (another channel of the brush texture) and the overlay.
        MaskComponent extra;
        extra.shape = MaskShape::BRUSH;
        extra.mode = MaskMode::SUBTRACT;
        extra.dabs = {{0.3f, 0.5f, 0.05f, 0.2f, 1.0f, false}};
        masks.masks[0].components.push_back(extra);
        compare("two brushes + overlay", harness.run(masks, 0), masks, image, 0);
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        return 1;
    }

    gpu->waitIdle();
    const std::uint32_t errors = gpu->validationErrorCount();
    if (errors > 0) std::cout << "FAIL: " << errors << " validation error(s)\n";
    if (g_failures || errors) return 1;
    std::cout << "GPU masks: all checks passed\n";
    return 0;
}
