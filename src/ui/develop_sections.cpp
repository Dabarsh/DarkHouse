// Develop sections: Basic (white balance, tone, presence), Color Mixer,
// Color Grading and Detail, each bound to its GPU develop node.

#include "ui/develop_sections.hpp"

#include "denoise_node.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <span>
#include <utility>

namespace darkhouse::ui {
namespace {

// Gradient endpoints of the HSL sliders: what moving the slider does to the band's colour.
struct BandGradient {
    ImU32 left;
    ImU32 right;
};

BandGradient bandGradient(std::size_t band, int mode) {
    const float hue = kHslBandHues[band];
    switch (mode) {
    case 0: return {oklchColor(0.72f, 0.14f, hue - kHslMaxHueShift), oklchColor(0.72f, 0.14f, hue + kHslMaxHueShift)};
    case 1: return {oklchColor(0.72f, 0.0f, hue), oklchColor(0.72f, 0.2f, hue)};
    default: return {oklchColor(0.35f, 0.1f, hue), oklchColor(0.92f, 0.08f, hue)};
    }
}

// Separator with a title and a small right-aligned "Reset" button on its
// line. Returns true when the button was pressed.
bool sectionHeader(const char* title) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SeparatorText(title);
    ImGui::PushID(title);
    const float width = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - width));
    const bool pressed = ImGui::SmallButton("Reset");
    ImGui::PopID();
    return pressed;
}

}  // namespace

// -----------------------------------------------------------------------------
// Basic
// -----------------------------------------------------------------------------

void BasicSection::draw(PanelContext& ctx) {
    whiteBalance_.sync(ctx.app);
    tone_.sync(ctx.app);
    presence_.sync(ctx.app);

    // White balance: the scene illuminant, adapted to the 5500 K reference.
    if (sectionHeader("White Balance")) whiteBalance_.reset(ctx);
    WhiteBalanceParams& wb = whiteBalance_.values();
    SliderResult wbResult = adjustmentSlider("Temp", wb.temperature, kMinTemperature, kMaxTemperature,
                                             kReferenceTemperature, "%.0f K", IM_COL32(70, 120, 230, 255),
                                             IM_COL32(240, 200, 70, 255), ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Colour temperature of the light in the scene. Lower cools the photo, higher warms it.");
    wbResult |= adjustmentSlider("Tint", wb.tint, -kTintRange, kTintRange, 0.0f, "%+.0f", IM_COL32(60, 190, 80, 255),
                                 IM_COL32(210, 70, 200, 255));
    whiteBalance_.commit(ctx, wbResult);

    // Tone: the exposure node's parameters (contrast and the tone sliders are
    // stored as [-1, 1] and shown as [-100, 100]).
    if (sectionHeader("Tone")) tone_.reset(ctx);
    ExposureParams& tone = tone_.values();
    float contrast = tone.contrast * 100.0f;
    float highlights = tone.highlights * 100.0f;
    float shadows = tone.shadows * 100.0f;
    SliderResult toneResult = adjustmentSlider("Exposure", tone.exposureEV, -5.0f, 5.0f, 0.0f, "%+.2f EV",
                                               IM_COL32(20, 20, 20, 255), IM_COL32(235, 235, 235, 255));
    toneResult |= adjustmentSlider("Contrast", contrast, -100.0f, 100.0f, 0.0f, "%+.0f");
    toneResult |= adjustmentSlider("Highlights", highlights, -100.0f, 100.0f, 0.0f, "%+.0f");
    toneResult |= adjustmentSlider("Shadows", shadows, -100.0f, 100.0f, 0.0f, "%+.0f");
    tone.contrast = contrast / 100.0f;
    tone.highlights = highlights / 100.0f;
    tone.shadows = shadows / 100.0f;
    tone_.commit(ctx, toneResult);

    ColorGradingParams& presence = presence_.values();
    if (sectionHeader("Presence")) {
        presence.vibrance = presence.saturation = 0.0f;
        presence_.commit(ctx, SliderResult{true, true});
    }
    SliderResult presenceResult = adjustmentSlider("Vibrance", presence.vibrance, -100.0f, 100.0f, 0.0f, "%+.0f",
                                                   IM_COL32(128, 128, 128, 255), IM_COL32(230, 110, 60, 255));
    presenceResult |= adjustmentSlider("Saturation", presence.saturation, -100.0f, 100.0f, 0.0f, "%+.0f",
                                       IM_COL32(128, 128, 128, 255), IM_COL32(230, 60, 60, 255));
    presence_.commit(ctx, presenceResult);
}

// -----------------------------------------------------------------------------
// Tone curve
// -----------------------------------------------------------------------------

void ToneCurveSection::draw(PanelContext& ctx) {
    curves_.sync(ctx.app);
    ToneCurveParams& params = curves_.values();
    if (sectionHeader("Point curve")) curves_.reset(ctx);

    constexpr std::array<const char*, kCurveChannelCount> kNames{"RGB", "Red", "Green", "Blue"};
    const std::array<ImU32, kCurveChannelCount> colors{IM_COL32(235, 235, 235, 255), IM_COL32(235, 80, 70, 255),
                                                       IM_COL32(90, 200, 90, 255), IM_COL32(80, 140, 240, 255)};
    for (int c = 0; c < static_cast<int>(kCurveChannelCount); ++c) {
        if (c > 0) ImGui::SameLine();
        const auto channel = static_cast<std::size_t>(c);
        const bool edited = !isIdentity(params.curves[channel]);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(colors[channel]));
        if (ImGui::RadioButton(kNames[channel], channel_ == c)) channel_ = c;
        ImGui::PopStyleColor();
        if (edited) ImGui::SetItemTooltip("%s curve edited", kNames[channel]);
    }

    SliderResult result;
    Curve& curve = params.curves[static_cast<std::size_t>(channel_)];
    std::array<std::pair<const Curve*, ImU32>, kCurveChannelCount - 1> others{};
    std::size_t otherCount = 0;
    for (std::size_t c = 0; c < kCurveChannelCount; ++c) {
        if (c != static_cast<std::size_t>(channel_)) others[otherCount++] = {&params.curves[c], colors[c]};
    }
    const float size = std::clamp(ImGui::GetContentRegionAvail().x, 120.0f, 320.0f);
    result |= curveEditor("##curve", curve, colors[static_cast<std::size_t>(channel_)], size,
                          std::span<const std::pair<const Curve*, ImU32>>(others.data(), otherCount));

    // Presets for the selected channel.
    ImGui::SetNextItemWidth(std::min(size, ImGui::GetContentRegionAvail().x * 0.6f));
    if (ImGui::BeginCombo("##Preset", "Preset...")) {
        for (CurvePreset preset : {CurvePreset::LINEAR, CurvePreset::MEDIUM_CONTRAST, CurvePreset::STRONG_CONTRAST,
                                   CurvePreset::FADED}) {
            if (ImGui::Selectable(toString(preset))) {
                curve = curvePreset(preset);
                result.changed = result.released = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%u point%s", std::max(curve.count, 2u), std::max(curve.count, 2u) == 1 ? "" : "s");
    curves_.commit(ctx, result);
}

// -----------------------------------------------------------------------------
// Lens corrections
// -----------------------------------------------------------------------------

void LensCorrectionSection::draw(PanelContext& ctx) {
    lens_.sync(ctx.app);
    LensCorrectionParams& lens = lens_.values();
    SliderResult result;

    if (sectionHeader("Profile")) {
        const LensCorrectionParams defaults;
        lens.profileEnabled = 0;
        lens.distortionAmount = defaults.distortionAmount;
        lens.vignettingAmount = defaults.vignettingAmount;
        lens.removeChromaticAberration = defaults.removeChromaticAberration;
        result.changed = result.released = true;
    }
    drawProfile(ctx, lens, result);

    if (sectionHeader("Manual")) {
        const LensCorrectionParams defaults;
        lens.manualDistortion = defaults.manualDistortion;
        lens.manualVignetting = defaults.manualVignetting;
        lens.manualVignettingMidpoint = defaults.manualVignettingMidpoint;
        lens.manualCaRed = defaults.manualCaRed;
        lens.manualCaBlue = defaults.manualCaBlue;
        lens.constrainCrop = defaults.constrainCrop;
        lens.scale = defaults.scale;
        result.changed = result.released = true;
    }
    result |= adjustmentSlider("Distortion", lens.manualDistortion, -100.0f, 100.0f, 0.0f, "%+.0f");
    ImGui::SetItemTooltip("Positive removes barrel distortion, negative removes pincushion");
    result |= adjustmentSlider("Vignetting", lens.manualVignetting, -100.0f, 100.0f, 0.0f, "%+.0f",
                               IM_COL32(40, 40, 40, 255), IM_COL32(230, 230, 230, 255));
    ImGui::SetItemTooltip("Positive brightens the corners, negative darkens them");
    ImGui::BeginDisabled(lens.manualVignetting == 0.0f);
    result |= adjustmentSlider("Midpoint", lens.manualVignettingMidpoint, 0.0f, 100.0f, 50.0f, "%.0f");
    ImGui::EndDisabled();
    result |= adjustmentSlider("Red / Cyan", lens.manualCaRed, -100.0f, 100.0f, 0.0f, "%+.0f", IM_COL32(60, 200, 210, 255),
                               IM_COL32(220, 60, 60, 255));
    result |= adjustmentSlider("Blue / Yellow", lens.manualCaBlue, -100.0f, 100.0f, 0.0f, "%+.0f",
                               IM_COL32(220, 200, 60, 255), IM_COL32(70, 110, 230, 255));
    result |= adjustmentSlider("Scale", lens.scale, 50.0f, 150.0f, 100.0f, "%.0f %%");
    bool constrain = lens.constrainCrop != 0;
    if (ImGui::Checkbox("Constrain Crop", &constrain)) {
        lens.constrainCrop = constrain ? 1u : 0u;
        result.changed = result.released = true;
    }
    ImGui::SetItemTooltip("Zoom in just enough that corrected edges never show past the photo");
    lens_.commit(ctx, result);
}

void LensCorrectionSection::drawProfile(PanelContext& ctx, LensCorrectionParams& lens, SliderResult& result) {
    if (!searched_) {
        searched_ = true;
        directory_ = LensDatabase::findDirectory(ctx.app.config().lensDatabaseDirectory);
        if (directory_) {
            database_ = std::async(std::launch::async, [dir = *directory_] {
                            return std::shared_ptr<const LensDatabase>(std::make_shared<LensDatabase>(LensDatabase::loadDirectory(dir)));
                        }).share();
        }
    }
    const LensDatabase* database = nullptr;
    if (database_.valid() && database_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        database = database_.get().get();
    }

    // The photo's lens.
    const AssetRecord* asset = ctx.library.findAsset(ctx.app.activeAssetId());
    const AssetMetadata* meta = asset ? &asset->metadata : nullptr;
    const std::string lensName = meta && meta->lens ? *meta->lens : std::string();
    const float focal = meta && meta->focalLength ? static_cast<float>(*meta->focalLength) : 0.0f;
    const float aperture = meta && meta->aperture ? static_cast<float>(*meta->aperture) : 0.0f;
    ImGui::TextDisabled("Lens");
    ImGui::SameLine();
    if (lensName.empty()) {
        ImGui::TextUnformatted(asset ? "not recorded in the photo" : "no photo open");
    } else {
        ImGui::TextWrapped("%s  (%.0f mm, f/%.1f)", lensName.c_str(), focal, aperture);
    }

    const LensProfile* match = nullptr;
    if (!directory_) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("No lens database found. Install the lensfun data (lensfun-data / liblensfun-data-v1) or "
                           "start with --lens-db <dir>.");
        ImGui::PopTextWrapPos();
    } else if (!database) {
        ImGui::TextDisabled("Loading lens profiles...");
    } else {
        match = lensName.empty() ? nullptr : database->findLens(lensName, meta && meta->cameraMake ? *meta->cameraMake : "");
        ImGui::TextDisabled("Profile");
        ImGui::SameLine();
        if (match) {
            ImGui::TextWrapped("%s", match->name().c_str());
        } else {
            ImGui::TextUnformatted("none for this lens");
        }
        ImGui::SetItemTooltip("%zu lenses, %zu cameras from %s", database->lenses().size(), database->cameras().size(),
                              database->directory().string().c_str());
    }

    // Enabling resolves the matched profile for the photo's focal length and
    // aperture; the coefficients are saved with the edit.
    bool enabled = lens.profileEnabled != 0;
    ImGui::BeginDisabled(!enabled && !match);
    if (ImGui::Checkbox("Enable Profile Corrections", &enabled)) {
        if (enabled && match) {
            float crop = 0.0f;
            if (meta && meta->cameraModel) {
                if (const CameraProfile* camera = database->findCamera(meta->cameraMake.value_or(""), *meta->cameraModel)) {
                    crop = camera->cropFactor;
                }
            }
            applyProfile(lens, resolveLensProfile(*match, focal, aperture, crop));
        } else {
            lens.profileEnabled = 0;
        }
        result.changed = result.released = true;
    }
    ImGui::EndDisabled();
    if (lens.profileEnabled) {
        ImGui::TextDisabled("Applied: %s", lens.profileName);
        ImGui::BeginDisabled(lens.distortionModel == DistortionModel::NONE);
        result |= adjustmentSlider("Distortion##profile", lens.distortionAmount, 0.0f, 200.0f, 100.0f, "%.0f");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!lens.hasVignetting);
        result |= adjustmentSlider("Vignetting##profile", lens.vignettingAmount, 0.0f, 200.0f, 100.0f, "%.0f");
        ImGui::EndDisabled();
        bool removeCa = lens.removeChromaticAberration != 0;
        ImGui::BeginDisabled(lens.tcaModel == TcaModel::NONE);
        if (ImGui::Checkbox("Remove Chromatic Aberration", &removeCa)) {
            lens.removeChromaticAberration = removeCa ? 1u : 0u;
            result.changed = result.released = true;
        }
        ImGui::EndDisabled();
    }
}

// -----------------------------------------------------------------------------
// Color mixer
// -----------------------------------------------------------------------------

void ColorMixerSection::draw(PanelContext& ctx) {
    hsl_.sync(ctx.app);
    HslParams& hsl = hsl_.values();
    if (sectionHeader("8 hue bands")) hsl_.reset(ctx);
    constexpr std::array<const char*, 3> kModes{"Hue", "Saturation", "Luminance"};
    for (int m = 0; m < 3; ++m) {
        if (m > 0) ImGui::SameLine();
        if (ImGui::RadioButton(kModes[static_cast<std::size_t>(m)], mode_ == m)) mode_ = m;
    }

    std::array<float, kHslBands>& values = mode_ == 0 ? hsl.hue : mode_ == 1 ? hsl.saturation : hsl.luminance;
    SliderResult result;
    for (std::size_t band = 0; band < kHslBands; ++band) {
        const BandGradient gradient = bandGradient(band, mode_);
        result |= adjustmentSlider(kHslBandNames[band], values[band], -100.0f, 100.0f, 0.0f, "%+.0f", gradient.left,
                                   gradient.right);
    }
    hsl_.commit(ctx, result);
}

// -----------------------------------------------------------------------------
// Color grading
// -----------------------------------------------------------------------------

void ColorGradingSection::draw(PanelContext& ctx) {
    grading_.sync(ctx.app);
    ColorGradingParams& grading = grading_.values();
    if (sectionHeader("3-way wheels")) {
        const ColorGradingParams neutral;
        grading.shadows = neutral.shadows;
        grading.midtones = neutral.midtones;
        grading.highlights = neutral.highlights;
        grading.global = neutral.global;
        grading.blending = neutral.blending;
        grading.balance = neutral.balance;
        grading_.commit(ctx, SliderResult{true, true});
    }

    SliderResult result;
    struct Zone {
        const char* name;
        ColorWheel* wheel;
    };
    const std::array<Zone, 3> zones{{{"Shadows", &grading.shadows},
                                     {"Midtones", &grading.midtones},
                                     {"Highlights", &grading.highlights}}};
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float diameter = std::clamp((ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f, 60.0f, 140.0f);
    if (ImGui::BeginTable("##Wheels", 3, ImGuiTableFlags_SizingFixedFit)) {
        for (const Zone& zone : zones) {
            ImGui::TableNextColumn();
            ImGui::PushID(zone.name);
            ImGui::TextDisabled("%s", zone.name);
            result |= colorWheel("wheel", zone.wheel->hue, zone.wheel->saturation, diameter);
            result |= compactSlider("##lum", zone.wheel->luminance, -100.0f, 100.0f, 0.0f, "Lum %+.0f", diameter);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Global wheel next to blending / balance.
    ImGui::Spacing();
    if (ImGui::BeginTable("##Global", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextColumn();
        ImGui::PushID("Global");
        ImGui::TextDisabled("Global");
        result |= colorWheel("wheel", grading.global.hue, grading.global.saturation, diameter);
        result |= compactSlider("##lum", grading.global.luminance, -100.0f, 100.0f, 0.0f, "Lum %+.0f", diameter);
        ImGui::PopID();
        ImGui::TableNextColumn();
        const float width = std::max(ImGui::GetContentRegionAvail().x, 80.0f);
        ImGui::TextDisabled("Blending");
        result |= compactSlider("##blending", grading.blending, 0.0f, 100.0f, 50.0f, "%.0f", width);
        ImGui::SetItemTooltip("How much the shadow, midtone and highlight ranges overlap");
        ImGui::TextDisabled("Balance");
        result |= compactSlider("##balance", grading.balance, -100.0f, 100.0f, 0.0f, "%+.0f", width);
        ImGui::SetItemTooltip("Positive moves the split between the ranges towards the shadows, so the "
                              "highlight tint covers more of the image");
        ImGui::EndTable();
    }
    grading_.commit(ctx, result);
}

// -----------------------------------------------------------------------------
// Detail (noise reduction)
// -----------------------------------------------------------------------------

void DetailSection::draw(PanelContext& ctx) {
    denoise_.sync(ctx.app);
    DenoiseParams& noise = denoise_.values();
    const std::optional<std::uint32_t> node = denoise_.index();

    bool enabled = node.has_value();
    if (ImGui::Checkbox("Noise Reduction", &enabled)) {
        std::vector<EditNodeRecord> stack = ctx.app.developStack();
        if (enabled) {
            // First in the stack: the noise model assumes scene-linear light, before any tone change.
            stack = withDevelopNode(std::move(stack), EditNodeRecord{0, std::string(DenoiseNode::kTypeName),
                                                                      DenoiseNode::pack(DenoiseParams{})});
        } else {
            stack.erase(stack.begin() + *node);
        }
        ctx.app.postEvent(SetDevelopStackEvent{std::move(stack), true});
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", node ? "multiscale, GPU" : "off");
    if (!node) return;

    const auto* denoise = dynamic_cast<const DenoiseNode*>(ctx.app.developNode(*node));
    const DenoiseStatistics stats = denoise ? denoise->statistics() : DenoiseStatistics{};

    float luminance = noise.luminance * 100.0f;
    float colour = noise.chrominance * 100.0f;
    float detail = noise.detail * 100.0f;
    SliderResult result = adjustmentSlider("Luminance", luminance, 0.0f, 100.0f, 50.0f, "%.0f");
    result |= adjustmentSlider("Color", colour, 0.0f, 100.0f, 50.0f, "%.0f");
    result |= adjustmentSlider("Detail", detail, 0.0f, 100.0f, 20.0f, "%.0f");
    noise.luminance = luminance / 100.0f;
    noise.chrominance = colour / 100.0f;
    noise.detail = detail / 100.0f;

    // Noise level: measured from the image on every render, or set by hand
    // (sigma after the square-root variance stabilisation, see denoise.hpp).
    constexpr float kMinLevel = 0.001f;
    constexpr float kMaxLevel = 0.25f;
    bool automatic = noise.noiseLevel <= 0.0f;
    if (ImGui::Checkbox("Auto noise level", &automatic)) {
        // Manual starts from the current estimate, so the image does not jump.
        const float measured = stats.sigma[0] > 0.0f ? stats.sigma[0] : 0.02f;
        noise.noiseLevel = automatic ? 0.0f : std::clamp(measured, kMinLevel, kMaxLevel);
        result.changed = result.released = true;
    }
    if (automatic) {
        if (stats.sigma[0] > 0.0f) {
            ImGui::TextDisabled("Measured  luma %.4f  color %.4f / %.4f", stats.sigma[0], stats.sigma[1], stats.sigma[2]);
            ImGui::SetItemTooltip("Noise sigma per opponent channel, estimated from the finest wavelet band\n"
                                  "(median absolute deviation) in the square-root domain.");
        }
    } else {
        result |= adjustmentSlider("Noise level", noise.noiseLevel, kMinLevel, kMaxLevel, 0.02f, "%.4f", 0, 0,
                                   ImGuiSliderFlags_Logarithmic);
    }
    if (stats.levels > 0) {
        std::string line = std::to_string(stats.levels) + (stats.levels == 1 ? " detail band" : " detail bands");
        for (const auto& timing : ctx.app.developTimings()) {
            if (timing.id != *node) continue;
            char gpuTime[48];
            std::snprintf(gpuTime, sizeof gpuTime, ", %.1f ms on the GPU", timing.milliseconds);
            line += gpuTime;
        }
        ImGui::TextDisabled("%s", line.c_str());
    }
    if (ImGui::SmallButton("Reset Noise Reduction")) {
        noise = DenoiseParams{};
        result.changed = result.released = true;
    }
    denoise_.commit(ctx, result);
}

}  // namespace darkhouse::ui
