// Masking panel: the photo's masks (local adjustments), their components and
// the tools that shape them on the canvas.

#include "ui/panels.hpp"

#include "ui/masking.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace darkhouse::ui {
namespace {

constexpr std::array<MaskShape, kMaskShapeCount> kShapes{MaskShape::BRUSH,           MaskShape::LINEAR_GRADIENT,
                                                         MaskShape::RADIAL_GRADIENT, MaskShape::LUMINANCE_RANGE,
                                                         MaskShape::COLOR_RANGE,     MaskShape::SUBJECT,
                                                         MaskShape::SKY};

MaskComponent defaultComponent(MaskShape shape, MaskMode mode) {
    MaskComponent c;
    c.shape = shape;
    c.mode = mode;
    switch (shape) {
    case MaskShape::LINEAR_GRADIENT:  // a graduated filter over the top of the frame
        c.start = {0.5f, 0.15f};
        c.end = {0.5f, 0.55f};
        break;
    case MaskShape::RADIAL_GRADIENT:
        c.start = {0.5f, 0.5f};
        c.size = {0.25f, 0.3f};
        c.feather = 0.5f;
        break;
    case MaskShape::LUMINANCE_RANGE:  // the shadows
        c.low = 0.0f;
        c.high = 0.35f;
        c.falloff = 0.1f;
        break;
    case MaskShape::COLOR_RANGE:
        c.hue = 29.0f;
        c.hueWidth = 60.0f;
        c.low = 0.03f;
        c.falloff = 0.2f;
        break;
    default: break;
    }
    return c;
}

MaskTool toolFor(MaskShape shape) {
    switch (shape) {
    case MaskShape::BRUSH: return MaskTool::BRUSH;
    case MaskShape::LINEAR_GRADIENT: return MaskTool::DRAW_LINEAR;
    case MaskShape::RADIAL_GRADIENT: return MaskTool::DRAW_RADIAL;
    default: return MaskTool::NONE;
    }
}

// Shape menu for adding a component; returns the chosen shape.
std::optional<MaskShape> shapeMenu(const char* id) {
    std::optional<MaskShape> chosen;
    if (ImGui::BeginPopup(id)) {
        for (MaskShape shape : kShapes) {
            const bool placeholder = shape == MaskShape::SUBJECT || shape == MaskShape::SKY;
            if (ImGui::MenuItem(toString(shape), placeholder ? "placeholder" : nullptr)) chosen = shape;
            if (placeholder) ImGui::SetItemTooltip("Heuristic until the AI segmentation models are available");
        }
        ImGui::EndPopup();
    }
    return chosen;
}

// A toggle button that stays highlighted while on.
bool toggleButton(const char* label, bool on) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHovered);
    }
    const bool pressed = ImGui::Button(label);
    if (on) ImGui::PopStyleColor(2);
    return pressed;
}

float percent(float fraction) { return fraction * 100.0f; }

}  // namespace

void MaskingPanel::draw(PanelContext& ctx) {
    MaskingState& m = ctx.masking;
    m.panelVisible = true;
    m.masks.sync(ctx.app, ctx.frame.frameIndex);
    m.clampSelection();
    LocalAdjustments& all = m.masks.values();

    SliderResult edits;       // slider-like edits: live while dragging, saved on release
    bool structural = false;  // one-shot edits: saved at once

    // --- New mask ---------------------------------------------------------------------------
    const bool full = all.masks.size() >= kMaxMasks;
    ImGui::BeginDisabled(full);
    if (ImGui::Button("+ New Mask")) ImGui::OpenPopup("##NewMask");
    ImGui::EndDisabled();
    if (const std::optional<MaskShape> shape = shapeMenu("##NewMask")) {
        LocalAdjustment mask;
        // Numbered past the masks already there, so names stay distinct across sessions.
        maskCounter_ = std::max(maskCounter_, static_cast<int>(all.masks.size()));
        mask.name = std::string(toString(*shape)) + " " + std::to_string(++maskCounter_);
        mask.components = {defaultComponent(*shape, MaskMode::ADD)};
        all.masks.push_back(std::move(mask));
        m.selectedMask = static_cast<int>(all.masks.size()) - 1;
        m.selectedComponent = 0;
        m.tool = toolFor(*shape);
        structural = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu masks", all.masks.size(), kMaxMasks);
    if (all.masks.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("Masks apply local edits (exposure, contrast, colour...) to part of the photo: paint "
                            "with a brush, drag a linear or radial gradient, or select by luminance or colour.");
        ImGui::PopTextWrapPos();
    }

    // --- Mask list ---------------------------------------------------------------------------
    int removeMask = -1;
    for (int i = 0; i < static_cast<int>(all.masks.size()); ++i) {
        LocalAdjustment& mask = all.masks[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        if (ImGui::Checkbox("##on", &mask.enabled)) structural = true;
        ImGui::SetItemTooltip("Apply this mask's adjustments");
        ImGui::SameLine();
        char suffix[32];
        std::snprintf(suffix, sizeof suffix, "%.0f%%", percent(mask.amount));
        const float suffixWidth = ImGui::CalcTextSize(suffix).x + ImGui::GetFrameHeight() + 12.0f;
        if (ImGui::Selectable(mask.name.c_str(), m.selectedMask == i, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(std::max(ImGui::GetContentRegionAvail().x - suffixWidth, 20.0f), 0.0f))) {
            if (m.selectedMask != i) m.selectedComponent = 0;
            m.selectedMask = i;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", suffix);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeMask = i;
        ImGui::SetItemTooltip("Delete this mask");
        ImGui::PopID();
    }
    if (removeMask >= 0) {
        all.masks.erase(all.masks.begin() + removeMask);
        if (m.selectedMask >= removeMask) --m.selectedMask;
        structural = true;
    }
    m.clampSelection();

    LocalAdjustment* mask = m.mask();
    if (mask) {
        // --- Selected mask ---------------------------------------------------------------------
        ImGui::SeparatorText("Mask");
        char name[96];
        std::snprintf(name, sizeof name, "%s", mask->name.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##name", name, sizeof name)) mask->name = name;
        if (ImGui::IsItemDeactivatedAfterEdit()) structural = true;
        if (ImGui::Checkbox("Invert", &mask->invert)) structural = true;
        ImGui::SameLine();
        bool overlay = m.showOverlay;
        if (ImGui::Checkbox("Show overlay", &overlay)) m.showOverlay = overlay;
        ImGui::SetItemTooltip("Show the selected mask in red on the canvas");
        float amount = percent(mask->amount);
        edits |= adjustmentSlider("Amount", amount, 0.0f, 100.0f, 100.0f, "%.0f%%");
        mask->amount = amount / 100.0f;

        // --- Components --------------------------------------------------------------------------
        ImGui::SeparatorText("Components");
        int removeComponent = -1;
        for (int j = 0; j < static_cast<int>(mask->components.size()); ++j) {
            MaskComponent& c = mask->components[static_cast<std::size_t>(j)];
            ImGui::PushID(j);
            char label[64];
            std::snprintf(label, sizeof label, "%s %s%s", j == 0 ? "" : toString(c.mode), toString(c.shape),
                          c.invert ? " (inverted)" : "");
            if (ImGui::Selectable(label[0] == ' ' ? label + 1 : label, m.selectedComponent == j,
                                  ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 8.0f, 20.0f), 0.0f))) {
                if (m.selectedComponent != j) m.tool = MaskTool::NONE;
                m.selectedComponent = j;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(mask->components.size() == 1);
            if (ImGui::SmallButton("x")) removeComponent = j;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (removeComponent >= 0) {
            mask->components.erase(mask->components.begin() + removeComponent);
            if (m.selectedComponent >= removeComponent) --m.selectedComponent;
            m.tool = MaskTool::NONE;
            structural = true;
        }
        const bool componentsFull = mask->components.size() >= kMaxComponentsPerMask;
        ImGui::BeginDisabled(componentsFull);
        constexpr std::array<std::pair<const char*, MaskMode>, 3> kModes{
            {{"Add", MaskMode::ADD}, {"Subtract", MaskMode::SUBTRACT}, {"Intersect", MaskMode::INTERSECT}}};
        for (const auto& [modeLabel, mode] : kModes) {
            if (mode != MaskMode::ADD) ImGui::SameLine();
            const std::string popup = std::string("##") + modeLabel;
            if (ImGui::Button(modeLabel)) ImGui::OpenPopup(popup.c_str());
            ImGui::SetItemTooltip("%s a shape to this mask", modeLabel);
            if (const std::optional<MaskShape> shape = shapeMenu(popup.c_str())) {
                mask->components.push_back(defaultComponent(*shape, mode));
                m.selectedComponent = static_cast<int>(mask->components.size()) - 1;
                m.tool = toolFor(*shape);
                structural = true;
            }
        }
        ImGui::EndDisabled();

        // --- Selected component --------------------------------------------------------------------
        m.clampSelection();
        if (MaskComponent* c = m.component()) {
            ImGui::SeparatorText(toString(c->shape));
            ImGui::PushID("component");
            if (m.selectedComponent > 0) {
                int mode = static_cast<int>(c->mode);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
                if (ImGui::Combo("##mode", &mode, "Add\0Subtract\0Intersect\0")) {
                    c->mode = static_cast<MaskMode>(mode);
                    structural = true;
                }
                ImGui::SameLine();
            }
            if (ImGui::Checkbox("Invert shape", &c->invert)) structural = true;
            float opacity = percent(c->opacity);
            edits |= adjustmentSlider("Opacity", opacity, 0.0f, 100.0f, 100.0f, "%.0f%%");
            c->opacity = opacity / 100.0f;

            switch (c->shape) {
            case MaskShape::BRUSH: {
                if (toggleButton(m.tool == MaskTool::BRUSH ? "Painting on the canvas" : "Paint on the canvas",
                                 m.tool == MaskTool::BRUSH)) {
                    m.tool = m.tool == MaskTool::BRUSH ? MaskTool::NONE : MaskTool::BRUSH;
                }
                ImGui::SetItemTooltip("Left-drag on the canvas paints; hold Alt to erase; middle-drag pans");
                ImGui::SameLine();
                ImGui::Checkbox("Erase", &m.brushErase);
                adjustmentSlider("Size", m.brushSize, 1.0f, 800.0f, 60.0f, "%.0f px", 0, 0, ImGuiSliderFlags_Logarithmic);
                adjustmentSlider("Feather", m.brushFeather, 0.0f, 100.0f, 50.0f, "%.0f");
                adjustmentSlider("Flow", m.brushFlow, 1.0f, 100.0f, 80.0f, "%.0f");
                ImGui::TextDisabled("%zu dab%s", c->dabs.size(), c->dabs.size() == 1 ? "" : "s");
                ImGui::SameLine();
                ImGui::BeginDisabled(c->dabs.empty());
                if (ImGui::SmallButton("Undo stroke")) {
                    // Strokes are runs of dabs; remove back to the last big jump.
                    std::size_t keep = c->dabs.size() - 1;
                    while (keep > 0) {
                        const BrushDab& a = c->dabs[keep - 1];
                        const BrushDab& b = c->dabs[keep];
                        if (std::hypot(a.x - b.x, a.y - b.y) > std::max(a.radius, b.radius) * 0.6f) break;
                        --keep;
                    }
                    c->dabs.resize(keep);
                    structural = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) {
                    c->dabs.clear();
                    structural = true;
                }
                ImGui::EndDisabled();
                break;
            }
            case MaskShape::LINEAR_GRADIENT: {
                if (toggleButton(m.tool == MaskTool::DRAW_LINEAR ? "Dragging on the canvas" : "Drag on the canvas",
                                 m.tool == MaskTool::DRAW_LINEAR)) {
                    m.tool = m.tool == MaskTool::DRAW_LINEAR ? MaskTool::NONE : MaskTool::DRAW_LINEAR;
                }
                ImGui::SetItemTooltip("Drag from where the effect is full to where it ends");
                float start[2] = {percent(c->start[0]), percent(c->start[1])};
                float end[2] = {percent(c->end[0]), percent(c->end[1])};
                edits |= adjustmentSlider("Start X", start[0], 0.0f, 100.0f, 50.0f, "%.1f%%");
                edits |= adjustmentSlider("Start Y", start[1], 0.0f, 100.0f, 15.0f, "%.1f%%");
                edits |= adjustmentSlider("End X", end[0], 0.0f, 100.0f, 50.0f, "%.1f%%");
                edits |= adjustmentSlider("End Y", end[1], 0.0f, 100.0f, 55.0f, "%.1f%%");
                c->start = {start[0] / 100.0f, start[1] / 100.0f};
                c->end = {end[0] / 100.0f, end[1] / 100.0f};
                break;
            }
            case MaskShape::RADIAL_GRADIENT: {
                if (toggleButton(m.tool == MaskTool::DRAW_RADIAL ? "Dragging on the canvas" : "Drag on the canvas",
                                 m.tool == MaskTool::DRAW_RADIAL)) {
                    m.tool = m.tool == MaskTool::DRAW_RADIAL ? MaskTool::NONE : MaskTool::DRAW_RADIAL;
                }
                ImGui::SetItemTooltip("Drag out from the centre; hold Shift for a circle");
                float centre[2] = {percent(c->start[0]), percent(c->start[1])};
                float radii[2] = {percent(c->size[0]), percent(c->size[1])};
                float feather = percent(c->feather);
                edits |= adjustmentSlider("Centre X", centre[0], 0.0f, 100.0f, 50.0f, "%.1f%%");
                edits |= adjustmentSlider("Centre Y", centre[1], 0.0f, 100.0f, 50.0f, "%.1f%%");
                edits |= adjustmentSlider("Width", radii[0], 0.5f, 150.0f, 25.0f, "%.1f%%");
                edits |= adjustmentSlider("Height", radii[1], 0.5f, 150.0f, 30.0f, "%.1f%%");
                edits |= adjustmentSlider("Angle", c->angle, 0.0f, 360.0f, 0.0f, "%.0f deg");
                edits |= adjustmentSlider("Feather", feather, 0.0f, 100.0f, 50.0f, "%.0f");
                c->start = {centre[0] / 100.0f, centre[1] / 100.0f};
                c->size = {radii[0] / 100.0f, radii[1] / 100.0f};
                c->feather = feather / 100.0f;
                break;
            }
            case MaskShape::LUMINANCE_RANGE: {
                float low = percent(c->low), high = percent(c->high), falloff = percent(c->falloff);
                edits |= adjustmentSlider("From", low, 0.0f, 100.0f, 0.0f, "%.0f", IM_COL32(0, 0, 0, 255),
                                          IM_COL32(255, 255, 255, 255));
                edits |= adjustmentSlider("To", high, 0.0f, 100.0f, 35.0f, "%.0f", IM_COL32(0, 0, 0, 255),
                                          IM_COL32(255, 255, 255, 255));
                edits |= adjustmentSlider("Smoothness", falloff, 0.1f, 100.0f, 10.0f, "%.0f");
                c->low = low / 100.0f;
                c->high = std::max(high, low) / 100.0f;
                c->falloff = falloff / 100.0f;
                ImGui::TextDisabled("Lightness (Oklab L) of the photo under the mask, 0 = black, 100 = white");
                break;
            }
            case MaskShape::COLOR_RANGE: {
                if (toggleButton("Pick colour", m.tool == MaskTool::PICK_COLOR)) {
                    m.tool = m.tool == MaskTool::PICK_COLOR ? MaskTool::NONE : MaskTool::PICK_COLOR;
                }
                ImGui::SetItemTooltip("Click the photo to select its colour");
                ImGui::SameLine();
                ImGui::ColorButton("##swatch", ImGui::ColorConvertU32ToFloat4(oklchColor(0.7f, 0.14f, c->hue)),
                                   ImGuiColorEditFlags_NoTooltip);
                edits |= adjustmentSlider("Hue", c->hue, 0.0f, 360.0f, 29.0f, "%.0f deg", oklchColor(0.7f, 0.14f, 0.0f),
                                          oklchColor(0.7f, 0.14f, 359.0f));
                edits |= adjustmentSlider("Range", c->hueWidth, 1.0f, 180.0f, 60.0f, "%.0f deg");
                float chroma = c->low * 1000.0f, falloff = percent(c->falloff);
                edits |= adjustmentSlider("Min chroma", chroma, 0.0f, 200.0f, 30.0f, "%.0f");
                edits |= adjustmentSlider("Smoothness", falloff, 0.1f, 100.0f, 20.0f, "%.0f");
                c->low = chroma / 1000.0f;
                c->falloff = falloff / 100.0f;
                break;
            }
            case MaskShape::SUBJECT:
            case MaskShape::SKY:
                ImGui::PushTextWrapPos();
                ImGui::TextDisabled(c->shape == MaskShape::SUBJECT
                                        ? "Placeholder: a centre-weighted ellipse, until the subject segmentation "
                                          "model is available."
                                        : "Placeholder: blue or bright near-white areas in the upper frame, until the "
                                          "sky segmentation model is available.");
                ImGui::PopTextWrapPos();
                break;
            }
            ImGui::PopID();
        }

        // --- Local adjustments ------------------------------------------------------------------------
        ImGui::SeparatorText("Adjustments");
        LocalAdjustParams& p = mask->params;
        ImGui::PushID("adjust");
        edits |= adjustmentSlider("Exposure", p.exposure, -4.0f, 4.0f, 0.0f, "%+.2f EV", IM_COL32(20, 20, 20, 255),
                                  IM_COL32(235, 235, 235, 255));
        edits |= adjustmentSlider("Contrast", p.contrast, -100.0f, 100.0f, 0.0f, "%+.0f");
        edits |= adjustmentSlider("Highlights", p.highlights, -100.0f, 100.0f, 0.0f, "%+.0f");
        edits |= adjustmentSlider("Shadows", p.shadows, -100.0f, 100.0f, 0.0f, "%+.0f");
        edits |= adjustmentSlider("Temp", p.temperature, -100.0f, 100.0f, 0.0f, "%+.0f", IM_COL32(70, 120, 230, 255),
                                  IM_COL32(240, 200, 70, 255));
        edits |= adjustmentSlider("Tint", p.tint, -100.0f, 100.0f, 0.0f, "%+.0f", IM_COL32(60, 190, 80, 255),
                                  IM_COL32(210, 70, 200, 255));
        edits |= adjustmentSlider("Saturation", p.saturation, -100.0f, 100.0f, 0.0f, "%+.0f",
                                  IM_COL32(128, 128, 128, 255), IM_COL32(230, 60, 60, 255));
        if (ImGui::SmallButton("Reset adjustments")) {
            p = LocalAdjustParams{};
            structural = true;
        }
        ImGui::PopID();
    }

    if (structural || edits.released) {
        m.masks.commit(ctx, false);
    } else if (edits.changed) {
        m.masks.commit(ctx, ImGui::IsAnyItemActive());
    }
    // The shell keeps the engine's overlay in step after all panels have drawn.
}

}  // namespace darkhouse::ui
