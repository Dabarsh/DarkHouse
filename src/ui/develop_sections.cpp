// Develop sections: Basic (white balance, tone, presence), Color Mixer,
// Color Grading and Detail, each bound to its GPU develop node.

#include "ui/develop_sections.hpp"

#include "denoise_node.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

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
