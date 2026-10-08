// Workspaces, colour pipeline and frame pacing, end to end through the real
// desktop shell (GuiEngine + DarkHouseShell in a window) with Vulkan
// validation, synchronization checks included via CTest's environment.
//
//   1. Opening a photo from the Catalog lands in Develop, and only the
//      Develop workspace's windows are drawn.
//   2. A full develop stack (lens corrections, white balance, exposure /
//      tone, tone curve, HSL, colour grading and a masked local adjustment)
//      renders what the CPU references compute, pixel for pixel.
//   3. Frame pacing: a simulated slider drag sends a new exposure every frame;
//      the UI keeps drawing while evaluations run, edits coalesce, the last
//      value wins and is saved on release.
//   4. Canvas & Compositing: only its windows are drawn; a vector layer added
//      to the document shows in the viewport through the develop stack; the
//      red channel view is grey.
//   5. Split, then back to Develop: the windows follow every switch and the
//      viewport keeps the composite.
//
// Any validation error fails the test. Exit codes: 0 pass, 1 fail, 77
// skipped (no display or no Vulkan 1.3 device).
//
// Usage: darkhouse_workspace_gui_test <work directory>

#include "app_controller.hpp"
#include "color_adjust.hpp"
#include "gui_engine.hpp"
#include "image_decoder.hpp"
#include "lens_correction.hpp"
#include "mask_engine.hpp"
#include "render_pipeline.hpp"
#include "tone_curve.hpp"
#include "ui/panel.hpp"
#include "ui/shell.hpp"
#include "ui/workspace_layout.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
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

constexpr std::uint32_t kWidth = 240, kHeight = 160;
constexpr int kDragFrames = 60;

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

// Hue across, lightness down, a grey strip at the bottom: every HSL band,
// shadows and highlights get something to act on.
Image makePhoto() {
    Image image{kWidth, kHeight, std::vector<std::uint8_t>(std::size_t{kWidth} * kHeight * 4)};
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const float h = static_cast<float>(x) / kWidth * 6.0f;
            const float v = 0.15f + 0.8f * (1.0f - static_cast<float>(y) / kHeight);
            const float s = y > kHeight * 5 / 6 ? 0.0f : 0.7f;
            const float f = h - std::floor(h);
            const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
            float rgb[3];
            switch (static_cast<int>(h) % 6) {
            case 0: rgb[0] = v, rgb[1] = t, rgb[2] = p; break;
            case 1: rgb[0] = q, rgb[1] = v, rgb[2] = p; break;
            case 2: rgb[0] = p, rgb[1] = v, rgb[2] = t; break;
            case 3: rgb[0] = p, rgb[1] = q, rgb[2] = v; break;
            case 4: rgb[0] = t, rgb[1] = p, rgb[2] = v; break;
            default: rgb[0] = v, rgb[1] = p, rgb[2] = q; break;
            }
            std::uint8_t* out = &image.rgba[(std::size_t{y} * kWidth + x) * 4];
            for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>(std::lround(rgb[c] * 255.0f));
            out[3] = 255;
        }
    }
    return image;
}

float quantize(float v) { return halfToFloat(floatToHalf(v)); }  // the FP16 textures between nodes
float linearToSrgb(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// The develop stack under test.
struct Stack {
    LensCorrectionParams lens;
    WhiteBalanceParams whiteBalance{4300.0f, 12.0f};
    ExposureParams exposure{0.4f, -0.3f, 0.25f, 0.15f};
    ToneCurveParams curve;
    HslParams hsl;
    ColorGradingParams grading;
    LocalAdjustments local;

    Stack() {
        lens.manualVignetting = 35.0f;
        lens.manualDistortion = 15.0f;
        curve.curves[0] = curvePreset(CurvePreset::MEDIUM_CONTRAST);
        curve.curves[3].count = 3;
        curve.curves[3].points = {CurvePoint{0.0f, 0.04f}, CurvePoint{0.5f, 0.46f}, CurvePoint{1.0f, 1.0f}};
        hsl.hue = {20.0f, 0.0f, -30.0f, 40.0f, 0.0f, -20.0f, 0.0f, 10.0f};
        hsl.saturation = {-30.0f, 20.0f, 0.0f, 40.0f, -60.0f, 0.0f, 25.0f, 0.0f};
        hsl.luminance = {10.0f, 0.0f, -20.0f, 0.0f, 30.0f, -10.0f, 0.0f, 0.0f};
        grading.vibrance = 25.0f;
        grading.saturation = -10.0f;
        grading.shadows = {220.0f, 40.0f, 5.0f};
        grading.highlights = {50.0f, 30.0f, -5.0f};
        LocalAdjustment mask;
        mask.name = "Sky";
        MaskComponent gradient;
        gradient.shape = MaskShape::LINEAR_GRADIENT;
        gradient.start = {0.5f, 0.0f};
        gradient.end = {0.5f, 0.7f};
        mask.components = {gradient};
        mask.params.exposure = -0.8f;
        mask.params.saturation = 30.0f;
        local.masks = {mask};
    }

    [[nodiscard]] std::vector<EditNodeRecord> records() const {
        return {{0, std::string(LensCorrectionNode::kTypeName), packParams(lens)},
                {1, "white_balance", packParams(whiteBalance)},
                {2, "exposure", ExposureNode::pack(exposure)},
                {3, "tone_curve", packParams(curve)},
                {4, "hsl", packParams(hsl)},
                {5, "color_grading", packParams(grading)},
                {6, "local_adjust", serialize(local)}};
    }

    // What the viewport should show: the CPU reference of every node, with the
    // FP16 storage between them, then the display encoding.
    [[nodiscard]] Image reference(const Image& photo) const {
        std::vector<float> rgba(photo.rgba.size());
        for (std::size_t i = 0; i < rgba.size(); ++i) {
            rgba[i] = quantize(i % 4 == 3 ? photo.rgba[i] / 255.0f : decode_detail::srgbToLinear8(photo.rgba[i]));
        }
        rgba = applyLensCorrection(lensCorrectionPush(lens, kWidth, kHeight), kWidth, kHeight, rgba);
        const WhiteBalancePush wb = whiteBalancePush(whiteBalance);
        const ToneCurveTables tables = bakeToneCurve(curve);
        const ColorGradingPush grade = colorGradingPush(grading);
        auto stage = [&](auto&& op) {
            for (std::size_t i = 0; i < rgba.size(); i += 4) {
                const Rgb out = op(Rgb{rgba[i], rgba[i + 1], rgba[i + 2]});
                for (int c = 0; c < 3; ++c) rgba[i + static_cast<std::size_t>(c)] = quantize(out[static_cast<std::size_t>(c)]);
                rgba[i + 3] = quantize(rgba[i + 3]);
            }
        };
        stage([&](const Rgb& c) { return applyWhiteBalance(wb, c); });
        stage([&](const Rgb& c) {
            return applyTone(c, exposure.exposureEV, exposure.highlights, exposure.shadows, exposure.contrast);
        });
        stage([&](const Rgb& c) { return applyToneCurve(tables, c); });
        stage([&](const Rgb& c) { return applyHsl(hsl, c); });
        stage([&](const Rgb& c) { return applyColorGrading(grade, c); });
        const std::vector<float> alpha = evaluateMask(local.masks[0], kWidth, kHeight, rgba);
        for (std::size_t p = 0; p < alpha.size(); ++p) {
            const Rgb original{rgba[p * 4], rgba[p * 4 + 1], rgba[p * 4 + 2]};
            const Rgb adjusted = applyLocalAdjust(local.masks[0].params, original);
            const float m = alpha[p] * local.masks[0].amount;
            for (std::size_t c = 0; c < 3; ++c) rgba[p * 4 + c] = quantize(original[c] + (adjusted[c] - original[c]) * m);
        }
        Image out{kWidth, kHeight, std::vector<std::uint8_t>(rgba.size())};
        for (std::size_t i = 0; i < rgba.size(); ++i) {
            const float v = i % 4 == 3 ? std::clamp(rgba[i], 0.0f, 1.0f) : linearToSrgb(rgba[i]);
            out.rgba[i] = static_cast<std::uint8_t>(std::lround(v * 255.0f));
        }
        return out;
    }
};

double meanOf(const Image& image, int channel) {
    double sum = 0.0;
    for (std::size_t i = static_cast<std::size_t>(channel); i < image.rgba.size(); i += 4) sum += image.rgba[i];
    return sum / static_cast<double>(image.rgba.size() / 4);
}

const std::uint8_t* pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    return &image.rgba[(std::size_t{y} * image.width + x) * 4];
}

// The window instance of `panel` in the workspace of `mode`.
std::string windowName(AppMode mode, ui::PanelId panel) {
    const ui::PanelInfo& info = ui::panelInfo(panel);
    return std::string(info.title) + "###DarkHouse." + ui::workspaceShortTitle(mode) + ".v" +
           std::to_string(ui::WorkspaceLayoutManager::kLayoutVersion) + "." + info.key;
}

// Begun this frame: the workspace draws it (tabs behind others included).
bool drawn(AppMode mode, ui::PanelId panel) {
    const ImGuiWindow* window = ImGui::FindWindowByName(windowName(mode, panel).c_str());
    return window && window->Active;
}

// The real GUI, with a script run after each drawn frame.
class ScriptedFrontEnd final : public FrontEnd {
public:
    ScriptedFrontEnd(std::unique_ptr<GuiEngine> gui, std::string assetId)
        : gui_(std::move(gui)), assetId_(std::move(assetId)) {}

    void configureGpu(VulkanContextOptions& options) override { gui_->configureGpu(options); }
    [[nodiscard]] bool requiresGpu() const noexcept override { return true; }
    void attachGpu(DarkHouseApp& app, VulkanContext& gpu) override {
        gpu_ = &gpu;
        gui_->attachGpu(app, gpu);
    }
    void detachGpu() noexcept override {
        if (gpu_) validationErrors = gpu_->validationErrorCount();
        gui_->detachGpu();
        gpu_ = nullptr;
    }
    bool pumpPlatformEvents(DarkHouseApp& app) override { return gui_->pumpPlatformEvents(app); }
    [[nodiscard]] bool interactive() const noexcept override { return true; }
    [[nodiscard]] bool pacesFrames() const noexcept override { return gui_->pacesFrames(); }

    void drawFrame(DarkHouseApp& app, const FrameContext& frame) override {
        const auto now = std::chrono::steady_clock::now();
        gui_->drawFrame(app, frame);
        ++frames_;
        if (frames_ > 4000) {
            std::cout << "FAIL: the script stalled at step " << step_ << '\n';
            app.postEvent(QuitEvent{});
            return;
        }
        run(app, frame, now);
    }

    int step_ = 0;
    std::uint32_t validationErrors = 0;
    Image developed, afterDrag, withShape, redChannel, backInDevelop;
    std::vector<bool> workspaceChecks;          // one per checked frame, in order
    std::vector<double> dragIntervals;          // ms between drawn frames during the drag
    int busyFramesDuringDrag = 0;
    std::uint64_t evaluationsDuringDrag = 0;
    std::vector<EditNodeRecord> savedAfterDrag;
    AppMode firstModeAfterOpen = AppMode::CATALOG;
    std::vector<std::pair<AppMode, AppMode>> switches;  // requested, first mode seen

private:
    bool capture(const FrameContext& frame, Image& out) {
        if (!gpu_ || !frame.canvasOutput || !frame.canvasOutput->valid()) return false;
        GPUTexture texture = *frame.canvasOutput;
        const std::vector<std::byte> bytes = gpu_->downloadTexture(texture);
        out.width = texture.width;
        out.height = texture.height;
        out.rgba.resize(bytes.size());
        std::memcpy(out.rgba.data(), bytes.data(), bytes.size());
        return true;
    }

    // Only the windows of `mode` are drawn: its viewport and main panel, and
    // none of another workspace's.
    bool onlyWorkspaceDrawn(AppMode mode) {
        using ui::PanelId;
        bool ok = true;
        switch (mode) {
        case AppMode::DEVELOP:
            ok = drawn(mode, PanelId::VIEWPORT) && drawn(mode, PanelId::ADJUSTMENTS) && drawn(mode, PanelId::MASKING) &&
                 !drawn(mode, PanelId::LAYERS) && !drawn(mode, PanelId::TOOLS);
            break;
        case AppMode::CANVAS:
            ok = drawn(mode, PanelId::VIEWPORT) && drawn(mode, PanelId::TOOLS) && drawn(mode, PanelId::LAYERS) &&
                 drawn(mode, PanelId::CHANNELS) && drawn(mode, PanelId::PATHS) && drawn(mode, PanelId::PROPERTIES) &&
                 !drawn(mode, PanelId::ADJUSTMENTS) && !drawn(mode, PanelId::FILMSTRIP);
            break;
        case AppMode::HYBRID_SPLIT:
            ok = drawn(mode, PanelId::ASSET_GRID) && drawn(mode, PanelId::VIEWPORT) && drawn(mode, PanelId::ADJUSTMENTS) &&
                 !drawn(mode, PanelId::LAYERS);
            break;
        case AppMode::CATALOG: ok = drawn(mode, PanelId::ASSET_GRID) && !drawn(mode, PanelId::VIEWPORT); break;
        }
        for (AppMode other : ui::workspaceModes()) {
            if (other == mode) continue;
            for (ui::PanelId panel : ui::allPanels()) ok = ok && !drawn(other, panel);
        }
        return ok;
    }

    void switchTo(DarkHouseApp& app, AppMode mode) {
        app.postEvent(SwitchModeEvent{mode});
        pendingSwitch_ = mode;
    }

    void run(DarkHouseApp& app, const FrameContext& frame, std::chrono::steady_clock::time_point now) {
        if (pendingSwitch_) {
            switches.emplace_back(*pendingSwitch_, frame.mode);  // the frame after the request
            pendingSwitch_.reset();
        }
        const bool settled = !app.developBusy();
        switch (step_) {
        case 0:  // 1. the photo opened from the Catalog, in Develop
            if (app.photo().state == PhotoStatus::State::READY && app.photo().assetId == assetId_ && settled) {
                firstModeAfterOpen = frame.mode;
                workspaceChecks.push_back(onlyWorkspaceDrawn(AppMode::DEVELOP));
                app.postEvent(SetDevelopStackEvent{stack_.records(), true});
                step_ = 1;
            }
            break;
        case 1:  // 2. the full develop stack
            if (settled && app.developStack().size() == 7 && capture(frame, developed)) {
                step_ = 2;
                dragFrame_ = 0;
                evaluationsBefore_ = app.developEvaluations();
                last_ = now;
            }
            break;
        case 2: {  // 3. a slider drag: a new exposure every frame, released at the end
            if (dragFrame_ > 0) dragIntervals.push_back(std::chrono::duration<double, std::milli>(now - last_).count());
            last_ = now;
            busyFramesDuringDrag += app.developBusy() ? 1 : 0;
            ExposureParams params = stack_.exposure;
            const bool release = dragFrame_ == kDragFrames;
            params.exposureEV = release ? kReleasedExposure : 0.4f + 0.012f * static_cast<float>(dragFrame_);
            app.postEvent(SetDevelopParamsEvent{2, ExposureNode::pack(params), release, "exposure"});
            if (release) {
                evaluationsDuringDrag = app.developEvaluations() - evaluationsBefore_;
                step_ = 3;
            }
            ++dragFrame_;
            break;
        }
        case 3:
            if (settled && capture(frame, afterDrag)) {
                savedAfterDrag = app.assets().loadEditStack(assetId_);
                switchTo(app, AppMode::CANVAS);
                step_ = 4;
            }
            break;
        case 4:  // 4. Canvas & Compositing: a red rectangle over the photo
            if (frame.mode == AppMode::CANVAS) {
                workspaceChecks.push_back(onlyWorkspaceDrawn(AppMode::CANVAS));
                VectorContent shape;
                shape.verbs = {PathVerb::MOVE_TO, PathVerb::LINE_TO, PathVerb::LINE_TO, PathVerb::LINE_TO, PathVerb::CLOSE};
                shape.points = {40.0f, 60.0f, 100.0f, 60.0f, 100.0f, 120.0f, 40.0f, 120.0f};
                shape.fillColor = {1.0f, 0.0f, 0.0f, 1.0f};
                app.document().addChild(LayerNode::createVector("Red box", shape));
                step_ = 5;
            }
            break;
        case 5:
            if (settled && ++waitFrames_ > 2 && capture(frame, withShape)) {
                waitFrames_ = 0;
                app.postEvent(SetDisplayChannelEvent{DisplayChannel::RED});
                step_ = 6;
            }
            break;
        case 6:
            if (settled && ++waitFrames_ > 2 && capture(frame, redChannel)) {
                waitFrames_ = 0;
                app.postEvent(SetDisplayChannelEvent{DisplayChannel::COLOR});
                switchTo(app, AppMode::HYBRID_SPLIT);
                step_ = 7;
            }
            break;
        case 7:  // 5. Split, then back to Develop
            if (frame.mode == AppMode::HYBRID_SPLIT) {
                workspaceChecks.push_back(onlyWorkspaceDrawn(AppMode::HYBRID_SPLIT));
                switchTo(app, AppMode::DEVELOP);
                step_ = 8;
            }
            break;
        case 8:
            if (frame.mode == AppMode::DEVELOP && settled && ++waitFrames_ > 2 && capture(frame, backInDevelop)) {
                workspaceChecks.push_back(onlyWorkspaceDrawn(AppMode::DEVELOP));
                step_ = 9;
                app.postEvent(QuitEvent{});
            }
            break;
        default: break;
        }
    }

public:
    static constexpr float kReleasedExposure = 1.1f;
    Stack stack_;

private:
    std::unique_ptr<GuiEngine> gui_;
    VulkanContext* gpu_ = nullptr;
    std::string assetId_;
    std::uint64_t frames_ = 0;
    int dragFrame_ = 0;
    int waitFrames_ = 0;
    std::uint64_t evaluationsBefore_ = 0;
    std::chrono::steady_clock::time_point last_;
    std::optional<AppMode> pendingSwitch_;
};

// Where two captures differ: the worst channel difference, its pixel, and
// how many values differ by more than `tolerance`. Writes both images and an
// amplified difference next to the catalog for inspection.
int reportDifference(const char* name, const Image& actual, const Image& expected, int tolerance, const fs::path& dir) {
    if (actual.rgba.size() != expected.rgba.size() || actual.rgba.empty()) {
        std::printf("%s: sizes differ (%zu vs %zu values)\n", name, actual.rgba.size(), expected.rgba.size());
        return 1 << 30;
    }
    int worst = 0;
    std::size_t worstAt = 0, over = 0;
    Image diff{actual.width, actual.height, std::vector<std::uint8_t>(actual.rgba.size(), 255)};
    for (std::size_t i = 0; i < actual.rgba.size(); ++i) {
        const int d = std::abs(int{actual.rgba[i]} - int{expected.rgba[i]});
        if (d > worst) {
            worst = d;
            worstAt = i;
        }
        over += d > tolerance ? 1 : 0;
        if (i % 4 != 3) diff.rgba[i] = static_cast<std::uint8_t>(std::min(255, d * 16));
    }
    const std::size_t p = worstAt / 4;
    std::printf("%s: worst %d at (%zu, %zu) channel %zu (%u vs %u), %zu values off by more than %d\n", name, worst,
                p % actual.width, p / actual.width, worstAt % 4, actual.rgba[worstAt], expected.rgba[worstAt], over, tolerance);
    if (worst > tolerance) {
        const std::string base = (dir / name).string();
        stbi_write_png((base + "_actual.png").c_str(), static_cast<int>(actual.width), static_cast<int>(actual.height), 4,
                       actual.rgba.data(), static_cast<int>(actual.width) * 4);
        stbi_write_png((base + "_expected.png").c_str(), static_cast<int>(actual.width), static_cast<int>(actual.height), 4,
                       expected.rgba.data(), static_cast<int>(actual.width) * 4);
        stbi_write_png((base + "_diff.png").c_str(), static_cast<int>(actual.width), static_cast<int>(actual.height), 4,
                       diff.rgba.data(), static_cast<int>(actual.width) * 4);
    }
    return worst;
}

double percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<std::size_t>(q * static_cast<double>(values.size())))];
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = fs::absolute(fs::path(argc > 1 ? argv[1] : "workspace_gui_test_work") / "workspace_gui");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    const Image photo = makePhoto();
    const fs::path photoPath = dir / "photo.png";
    stbi_write_png(photoPath.string().c_str(), kWidth, kHeight, 4, photo.rgba.data(), kWidth * 4);
    const fs::path catalog = dir / "catalog.sqlite";
    std::string assetId;
    {
        AssetManager assets;
        assets.initializeCatalog(catalog.string());
        assetId = assets.importFile(photoPath.string()).get().id;
    }

    std::unique_ptr<GuiEngine> gui;
    try {
        GuiOptions options;
        options.window.title = "DarkHouse workspace test";
        options.window.width = 1280;
        options.window.height = 800;
        options.vsync = false;          // measure the loop, not the display
        options.lowPowerIdle = false;   // draw every frame
        options.iniPath = dir / "layout.ini";
        gui = std::make_unique<GuiEngine>(std::move(options), std::make_unique<ui::DarkHouseShell>());
    } catch (const std::exception& e) {
        std::cout << "SKIP: cannot open a window: " << e.what() << '\n';
        return 77;
    }

    AppConfig config;
    config.catalogPath = catalog;
    config.shaderDirectory = DARKHOUSE_SHADER_DIR;
    config.initialMode = AppMode::CATALOG;
    config.enableValidationLayers = true;
    config.targetFramesPerSecond = 1000.0;
    config.maxFrames = 6000;  // safety net
    DarkHouseApp app(config);
    auto scripted = std::make_unique<ScriptedFrontEnd>(std::move(gui), assetId);
    ScriptedFrontEnd& script = *scripted;
    app.setFrontEnd(std::move(scripted));
    try {
        app.initialize();
    } catch (const std::exception& e) {
        std::cout << "SKIP: " << e.what() << '\n';
        return 77;
    }
    if (!app.canvasAvailable()) {
        std::cout << "SKIP: no Vulkan 1.3 device (canvas unavailable)\n";
        return 77;
    }
    app.postEvent(OpenAssetEvent{assetId});
    app.run();

    CHECK(script.step_ == 9);

    // 1 / 4 / 5. Workspaces: opened in Develop, every switch visible on the
    // very next frame, and only the active workspace's windows drawn.
    CHECK(script.firstModeAfterOpen == AppMode::DEVELOP);
    CHECK(script.workspaceChecks.size() == 4);
    for (std::size_t i = 0; i < script.workspaceChecks.size(); ++i) {
        if (!script.workspaceChecks[i]) std::cout << "FAIL: workspace check " << i << " drew the wrong windows\n";
        CHECK(script.workspaceChecks[i]);
    }
    for (const auto& [requested, seen] : script.switches) {
        std::printf("switch to %s: next frame in %s\n", std::string(toString(requested)).c_str(), std::string(toString(seen)).c_str());
        CHECK(requested == seen);
    }

    // 2. The develop stack against the CPU references.
    const Image expected = script.stack_.reference(photo);
    CHECK(script.developed.width == kWidth && script.developed.height == kHeight);
    if (script.developed.rgba.size() == expected.rgba.size()) {
        int worst = 0;
        double sum = 0.0;
        std::size_t over2 = 0;
        for (std::size_t i = 0; i < expected.rgba.size(); ++i) {
            const int diff = std::abs(int{script.developed.rgba[i]} - int{expected.rgba[i]});
            worst = std::max(worst, diff);
            sum += diff;
            over2 += diff > 2 ? 1 : 0;
        }
        const double mean = sum / static_cast<double>(expected.rgba.size());
        std::printf("develop stack (7 nodes) vs CPU: worst %d, mean %.3f LSB, %zu values off by more than 2\n", worst,
                    mean, over2);
        (void)reportDifference("develop_stack", script.developed, expected, 2, dir);
        CHECK(worst <= 4);                                  // dither (+-1) and FP16 / transcendental differences
        CHECK(mean < 0.75);
        CHECK(over2 < expected.rgba.size() / 200);
    }

    // 3. Frame pacing during the drag.
    const double p50 = percentile(script.dragIntervals, 0.5), p95 = percentile(script.dragIntervals, 0.95);
    const double worstInterval = percentile(script.dragIntervals, 1.0);
    std::printf("drag: %d edits in %zu frames, %llu evaluations, %d frames drawn while one was in flight; "
                "frame interval p50 %.1f ms, p95 %.1f ms, max %.1f ms\n",
                kDragFrames + 1, script.dragIntervals.size() + 1, static_cast<unsigned long long>(script.evaluationsDuringDrag),
                script.busyFramesDuringDrag, p50, p95, worstInterval);
    CHECK(script.evaluationsDuringDrag >= 1 && script.evaluationsDuringDrag <= static_cast<std::uint64_t>(kDragFrames + 1));
    CHECK(p95 < 250.0);        // the UI keeps drawing: no frame waits for a full evaluation chain
    CHECK(worstInterval < 1000.0);
    CHECK(meanOf(script.afterDrag, 1) > meanOf(script.developed, 1) + 5.0);  // the released, brighter value is shown
    bool savedRelease = false;
    for (const EditNodeRecord& record : script.savedAfterDrag) {
        if (record.nodeType == "exposure") {
            ExposureParams saved{};
            std::memcpy(&saved, record.serializedParams.data(), std::min(sizeof saved, record.serializedParams.size()));
            savedRelease = std::fabs(saved.exposureEV - ScriptedFrontEnd::kReleasedExposure) < 1e-6f;
        }
    }
    CHECK(savedRelease);

    // 4. The vector layer through the develop stack, and the red channel view.
    if (script.withShape.rgba.size() == script.afterDrag.rgba.size() && !script.withShape.rgba.empty()) {
        const std::uint8_t* inside = pixel(script.withShape, 70, 90);
        const std::uint8_t* before = pixel(script.afterDrag, 70, 90);
        std::printf("red box: inside %u %u %u (was %u %u %u)\n", inside[0], inside[1], inside[2], before[0], before[1], before[2]);
        CHECK(inside[0] > inside[1] + 60 && inside[0] > inside[2] + 60);
        const std::uint8_t* outside = pixel(script.withShape, 200, 20);
        const std::uint8_t* outsideBefore = pixel(script.afterDrag, 200, 20);
        CHECK(std::abs(int{outside[0]} - int{outsideBefore[0]}) <= 1 && std::abs(int{outside[1]} - int{outsideBefore[1]}) <= 1);
    }
    bool grey = !script.redChannel.rgba.empty();
    for (std::size_t i = 0; grey && i < script.redChannel.rgba.size(); i += 4) {
        grey = script.redChannel.rgba[i] == script.redChannel.rgba[i + 1] && script.redChannel.rgba[i + 1] == script.redChannel.rgba[i + 2];
    }
    CHECK(grey);
    if (grey && script.redChannel.rgba.size() == script.withShape.rgba.size()) {
        CHECK(std::abs(int{pixel(script.redChannel, 70, 90)[0]} - int{pixel(script.withShape, 70, 90)[0]}) <= 1);
    }

    // 5. Back in Develop: the composite (with the box) in colour again.
    CHECK(reportDifference("back_in_develop", script.backInDevelop, script.withShape, 0, dir) == 0);

    const std::uint32_t errors = script.validationErrors;
    std::printf("validation errors: %u\n", errors);
    if (g_failures || errors) {
        if (errors) std::cout << "FAIL: " << errors << " validation error(s)\n";
        return 1;
    }
    std::cout << "workspaces, colour pipeline and frame pacing: all checks passed\n";
    return 0;
}
