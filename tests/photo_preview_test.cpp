// Live photo preview, end to end: imports photos into a catalog, opens them
// through DarkHouseApp (async decode -> document -> develop graph -> display
// transform) and reads back what the viewport would show.
//
//   1. an opaque 8-bit PNG appears on the canvas, sRGB round trip within 1 LSB
//   2. a +1 EV exposure edit is applied live to that photo
//   3. opening a smaller photo with alpha resizes the canvas and keeps alpha
//   4. a photo whose file is gone reports FAILED and clears the canvas
//
// Runs with Vulkan validation (sync validation via CTest's environment).
// Exit codes: 0 pass, 1 fail, 77 skipped (no Vulkan 1.3 device).
//
// Usage: darkhouse_photo_preview_test <work directory>

#include "app_controller.hpp"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace darkhouse;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::cout << "FAIL line " << __LINE__ << ": " #condition << '\n';              \
        }                                                                                  \
    } while (false)

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

Image makePhoto(std::uint32_t width, std::uint32_t height, bool withAlpha) {
    Image image{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* p = &image.rgba[(std::size_t{y} * width + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 255 / (width - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (height - 1));
            p[2] = static_cast<std::uint8_t>((x * 7 + y * 13) % 256);
            p[3] = withAlpha ? static_cast<std::uint8_t>(255 - (x * 255 / (width - 1))) : 255;
        }
    }
    return image;
}

float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// Largest per-channel difference between the displayed bytes and `expected`
// (a function of the source bytes), plus the mean difference.
template <class Expected>
std::pair<int, double> compare(const Image& shown, const Image& source, Expected expected) {
    int worst = 0;
    double sum = 0.0;
    for (std::size_t i = 0; i < source.rgba.size(); ++i) {
        const int diff = std::abs(static_cast<int>(shown.rgba[i]) - expected(source.rgba, i));
        worst = std::max(worst, diff);
        sum += diff;
    }
    return {worst, sum / static_cast<double>(source.rgba.size())};
}

// Drives the scenario from inside the frame loop and captures the display output.
class ProbeFrontEnd final : public FrontEnd {
public:
    ProbeFrontEnd(std::string photo, std::string transparent, std::string missing)
        : photo_(std::move(photo)), transparent_(std::move(transparent)), missing_(std::move(missing)) {}

    void attachGpu(DarkHouseApp&, VulkanContext& gpu) override { gpu_ = &gpu; }
    void detachGpu() noexcept override {
        if (gpu_) validationErrors = gpu_->validationErrorCount();  // the context goes away with the run
        gpu_ = nullptr;
    }
    bool pumpPlatformEvents(DarkHouseApp&) override { return true; }
    [[nodiscard]] bool interactive() const noexcept override { return false; }

    void drawFrame(DarkHouseApp& app, const FrameContext& frame) override {
        const PhotoStatus& photo = app.photo();
        switch (step_) {
        case 0:  // photo on the canvas
            if (photo.state == PhotoStatus::State::READY && photo.assetId == photo_ && capture(frame, baseline)) {
                ExposureParams params;
                params.exposureEV = 1.0f;
                app.postEvent(SetDevelopParamsEvent{0, ExposureNode::pack(params), false});
                step_ = 1;
            }
            break;
        case 1:  // the next frame renders the edit
            if (capture(frame, exposed)) {
                app.postEvent(OpenAssetEvent{transparent_});
                step_ = 2;
            }
            break;
        case 2:
            if (photo.state == PhotoStatus::State::READY && photo.assetId == transparent_ && capture(frame, withAlpha)) {
                app.postEvent(OpenAssetEvent{missing_});
                step_ = 3;
            }
            break;
        case 3:
            if (photo.state == PhotoStatus::State::FAILED && capture(frame, failed)) {
                failedError = photo.error;
                step_ = 4;
            }
            break;
        default: break;
        }
    }

    [[nodiscard]] int step() const noexcept { return step_; }

    Image baseline, exposed, withAlpha, failed;
    std::string failedError;
    std::uint32_t validationErrors = 0;

private:
    bool capture(const FrameContext& frame, Image& out) {
        if (!gpu_ || !frame.canvasOutput || !frame.canvasOutput->valid()) return false;
        GPUTexture texture = *frame.canvasOutput;
        if (texture.format != PixelFormat::R8G8B8A8_UNORM) {
            std::cout << "FAIL: canvas output is not RGBA8\n";
            return false;
        }
        const std::vector<std::byte> bytes = gpu_->downloadTexture(texture);
        out.width = texture.width;
        out.height = texture.height;
        out.rgba.resize(bytes.size());
        std::memcpy(out.rgba.data(), bytes.data(), bytes.size());
        return true;
    }

    std::string photo_, transparent_, missing_;
    VulkanContext* gpu_ = nullptr;
    int step_ = 0;
};

std::string importOne(AssetManager& assets, const fs::path& path) { return assets.importFile(path.string()).get().id; }

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = fs::absolute(fs::path(argc > 1 ? argv[1] : "photo_preview_test_work") / "photo_preview");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    const Image opaque = makePhoto(300, 200, false);
    const Image translucent = makePhoto(64, 96, true);
    const fs::path opaquePath = dir / "opaque.png";
    const fs::path translucentPath = dir / "translucent.png";
    const fs::path missingPath = dir / "missing.png";
    stbi_write_png(opaquePath.string().c_str(), 300, 200, 4, opaque.rgba.data(), 300 * 4);
    stbi_write_png(translucentPath.string().c_str(), 64, 96, 4, translucent.rgba.data(), 64 * 4);
    stbi_write_png(missingPath.string().c_str(), 8, 8, 4, opaque.rgba.data(), 8 * 4);

    const fs::path catalog = dir / "catalog.sqlite";
    std::string opaqueId, translucentId, missingId;
    {
        AssetManager assets;
        assets.initializeCatalog(catalog.string());
        opaqueId = importOne(assets, opaquePath);
        translucentId = importOne(assets, translucentPath);
        missingId = importOne(assets, missingPath);
    }
    fs::remove(missingPath);

    AppConfig config;
    config.catalogPath = catalog;
    config.shaderDirectory = DARKHOUSE_SHADER_DIR;
    config.initialMode = AppMode::CANVAS;
    config.enableValidationLayers = true;
    config.exitWhenIdle = true;
    config.maxFrames = 5000;  // safety net
    config.targetFramesPerSecond = 1000.0;
    DarkHouseApp app(config);
    auto probe = std::make_unique<ProbeFrontEnd>(opaqueId, translucentId, missingId);
    ProbeFrontEnd& frontEnd = *probe;
    app.setFrontEnd(std::move(probe));
    app.initialize();
    if (!app.canvasAvailable()) {
        std::cout << "SKIP: no Vulkan 1.3 device (canvas unavailable)\n";
        return 77;
    }
    app.postEvent(OpenAssetEvent{opaqueId});
    app.run();

    CHECK(frontEnd.step() == 4);

    // 1. The photo, displayed through linear FP16 and back to sRGB.
    CHECK(frontEnd.baseline.width == 300 && frontEnd.baseline.height == 200);
    if (frontEnd.baseline.rgba.size() == opaque.rgba.size()) {
        const auto [worst, mean] = compare(frontEnd.baseline, opaque,
                                           [](const std::vector<std::uint8_t>& src, std::size_t i) { return int{src[i]}; });
        std::printf("baseline: worst %d, mean %.3f LSB\n", worst, mean);
        CHECK(worst <= 1);    // triangular dither of +-1 LSB
        CHECK(mean < 0.45);
    }

    // 2. +1 EV doubles linear light.
    if (frontEnd.exposed.rgba.size() == opaque.rgba.size()) {
        const auto [worst, mean] = compare(frontEnd.exposed, opaque, [](const std::vector<std::uint8_t>& src, std::size_t i) {
            if (i % 4 == 3) return int{src[i]};
            const float doubled = 2.0f * srgbToLinear(static_cast<float>(src[i]) / 255.0f);
            return static_cast<int>(std::lround(linearToSrgb(doubled) * 255.0f));
        });
        std::printf("exposure +1 EV: worst %d, mean %.3f LSB\n", worst, mean);
        CHECK(worst <= 2);
        CHECK(mean < 0.6);
    }

    // 3. Smaller photo with alpha: the canvas follows its size, alpha survives.
    CHECK(frontEnd.withAlpha.width == 64 && frontEnd.withAlpha.height == 96);
    if (frontEnd.withAlpha.rgba.size() == translucent.rgba.size()) {
        const auto [worst, mean] = compare(frontEnd.withAlpha, translucent,
                                           [](const std::vector<std::uint8_t>& src, std::size_t i) {
                                               // Fully transparent pixels composite to 0 in every channel.
                                               return src[i - i % 4 + 3] == 0 ? 0 : int{src[i]};
                                           });
        std::printf("alpha photo: worst %d, mean %.3f LSB\n", worst, mean);
        CHECK(worst <= 1);
    }

    // 4. Missing file: FAILED, and the previous photo is not left on screen.
    CHECK(app.photo().state == PhotoStatus::State::FAILED);
    CHECK(frontEnd.failedError.find("cannot read") != std::string::npos);
    bool cleared = !frontEnd.failed.rgba.empty();
    for (std::size_t i = 3; i < frontEnd.failed.rgba.size(); i += 4) cleared = cleared && frontEnd.failed.rgba[i] == 0;
    CHECK(cleared);

    const std::uint32_t errors = frontEnd.validationErrors;
    if (errors > 0) std::cout << "FAIL: " << errors << " validation error(s)\n";
    if (g_failures || errors) return 1;
    std::cout << "photo preview: all checks passed\n";
    return 0;
}
