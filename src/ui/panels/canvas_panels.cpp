// Canvas & Compositing panels: the unified layer stack and the selected
// layer's properties.

#include "ui/panels.hpp"

#include "color_adjust.hpp"
#include "tone_curve.hpp"
#include "ui/canvas_state.hpp"
#include "ui/masking.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <glm/common.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace darkhouse::ui {
namespace {

// --- Layer helpers -------------------------------------------------------------

struct LayerStyle {
    const char* chip;
    ImVec4 color;
    const char* description;
};

LayerStyle layerStyle(LayerType type) {
    switch (type) {
    case LayerType::PARAMETRIC_ADJUSTMENT:
        return {"ADJ", theme::kLayerParametric, "Adjustment layer (changes the layers below it in its group)"};
    case LayerType::RASTER_PIXEL: return {"PX", theme::kLayerRaster, "Pixel layer (sparse FP16 tiles)"};
    case LayerType::VECTOR_SHAPE: return {"VEC", theme::kLayerVector, "Vector shape (resolution independent)"};
    case LayerType::SMART_OBJECT: return {"OBJ", theme::kLayerSmart, "Smart object (another catalog photo)"};
    case LayerType::GROUP: return {"GRP", theme::kLayerGroup, "Group (composited in isolation)"};
    }
    return {"?", theme::kLayerGroup, ""};
}

std::size_t rasterCount(const LayerNode& root, const LayerNode* excludeSubtree) {
    std::size_t count = 0;
    root.visit([&](const LayerNode& node, std::size_t) {
        if (node.raster() && !(excludeSubtree && (&node == excludeSubtree || excludeSubtree->isAncestorOf(node)))) ++count;
    });
    return count;
}

std::size_t indexInParent(const LayerNode& layer) {
    const LayerNode* parent = layer.parent();
    for (std::size_t i = 0; parent && i < parent->childCount(); ++i) {
        if (&parent->child(i) == &layer) return i;
    }
    return 0;
}

// Fills a raster layer with a test chart, in linear light: a grey ramp, a
// grid of colour patches and a hue sweep, so develop adjustments have
// something to act on before RAW decoding exists.
void paintTestChart(SparseRasterLayer& layer) {
    const std::uint32_t width = layer.width();
    const std::uint32_t height = layer.height();
    constexpr std::uint32_t kBand = 64;  // rows per writeRegion call
    std::vector<float> rows(static_cast<std::size_t>(width) * kBand * 4);
    static constexpr std::array<glm::vec3, 24> kPatches{{
        {0.17f, 0.09f, 0.06f}, {0.55f, 0.30f, 0.22f}, {0.12f, 0.19f, 0.34f}, {0.10f, 0.15f, 0.06f},
        {0.23f, 0.21f, 0.43f}, {0.13f, 0.52f, 0.41f}, {0.68f, 0.20f, 0.03f}, {0.08f, 0.10f, 0.39f},
        {0.53f, 0.08f, 0.12f}, {0.10f, 0.04f, 0.14f}, {0.34f, 0.50f, 0.05f}, {0.76f, 0.36f, 0.02f},
        {0.03f, 0.05f, 0.29f}, {0.07f, 0.29f, 0.07f}, {0.44f, 0.03f, 0.04f}, {0.84f, 0.57f, 0.01f},
        {0.51f, 0.08f, 0.29f}, {0.00f, 0.23f, 0.37f}, {0.88f, 0.88f, 0.86f}, {0.58f, 0.59f, 0.59f},
        {0.36f, 0.36f, 0.36f}, {0.19f, 0.19f, 0.19f}, {0.09f, 0.09f, 0.09f}, {0.03f, 0.03f, 0.03f}}};

    for (std::uint32_t y0 = 0; y0 < height; y0 += kBand) {
        const std::uint32_t bandRows = std::min(kBand, height - y0);
        for (std::uint32_t row = 0; row < bandRows; ++row) {
            const float v = (static_cast<float>(y0 + row) + 0.5f) / static_cast<float>(height);
            for (std::uint32_t x = 0; x < width; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                glm::vec3 color;
                if (v < 0.2f) {
                    color = glm::vec3(u);  // grey ramp, black to white
                } else if (v < 0.7f) {
                    const float pv = (v - 0.2f) / 0.5f;
                    const auto column = static_cast<std::size_t>(std::min(u * 6.0f, 5.999f));
                    const auto line = static_cast<std::size_t>(std::min(pv * 4.0f, 3.999f));
                    const float fu = u * 6.0f - static_cast<float>(column);
                    const float fv = pv * 4.0f - static_cast<float>(line);
                    const bool gutter = fu < 0.06f || fu > 0.94f || fv < 0.06f || fv > 0.94f;
                    color = gutter ? glm::vec3(0.02f) : kPatches[line * 6 + column];
                } else {
                    const float fade = (v - 0.7f) / 0.3f;  // full saturation fading to white
                    glm::vec3 hue;
                    ImGui::ColorConvertHSVtoRGB(u, 1.0f - fade, 1.0f, hue.r, hue.g, hue.b);
                    color = hue * hue * 0.9f;  // roughly linearised
                }
                float* texel = &rows[(static_cast<std::size_t>(row) * width + x) * 4];
                texel[0] = color.r;
                texel[1] = color.g;
                texel[2] = color.b;
                texel[3] = 1.0f;
            }
        }
        layer.writeRegion(0, y0, width, bandRows, rows);
    }
}

VectorContent rectangleShape(float cx, float cy, float w, float h) {
    VectorContent shape;
    shape.verbs = {PathVerb::MOVE_TO, PathVerb::LINE_TO, PathVerb::LINE_TO, PathVerb::LINE_TO, PathVerb::CLOSE};
    shape.points = {cx - w / 2, cy - h / 2, cx + w / 2, cy - h / 2, cx + w / 2, cy + h / 2, cx - w / 2, cy + h / 2};
    shape.fillColor = {0.85f, 0.35f, 0.25f, 1.0f};
    return shape;
}

VectorContent ellipseShape(float cx, float cy, float rx, float ry) {
    constexpr float k = 0.5523f;  // cubic Bezier circle approximation
    VectorContent shape;
    shape.verbs = {PathVerb::MOVE_TO, PathVerb::CUBIC_TO, PathVerb::CUBIC_TO, PathVerb::CUBIC_TO, PathVerb::CUBIC_TO,
                   PathVerb::CLOSE};
    shape.points = {cx + rx, cy,
                    cx + rx, cy + k * ry, cx + k * rx, cy + ry, cx, cy + ry,
                    cx - k * rx, cy + ry, cx - rx, cy + k * ry, cx - rx, cy,
                    cx - rx, cy - k * ry, cx - k * rx, cy - ry, cx, cy - ry,
                    cx + k * rx, cy - ry, cx + rx, cy - k * ry, cx + rx, cy};
    shape.fillColor = {0.30f, 0.55f, 0.90f, 1.0f};
    return shape;
}

// Parameter editor of an adjustment layer; true when a value changed.
bool editAdjustment(AdjustmentContent& content) {
    bool changed = false;
    auto slider = [&](const char* label, float& value, float lo, float hi, float def, const char* format,
                      ImU32 left = 0, ImU32 right = 0) { changed |= adjustmentSlider(label, value, lo, hi, def, format, left, right).changed; };
    try {
        if (content.nodeType == "exposure") {
            ExposureParams p = unpackParams<ExposureParams>(content.serializedParams, "exposure");
            float contrast = p.contrast * 100.0f, highlights = p.highlights * 100.0f, shadows = p.shadows * 100.0f;
            slider("Exposure", p.exposureEV, -5.0f, 5.0f, 0.0f, "%+.2f EV", IM_COL32(20, 20, 20, 255), IM_COL32(235, 235, 235, 255));
            slider("Contrast", contrast, -100.0f, 100.0f, 0.0f, "%+.0f");
            slider("Highlights", highlights, -100.0f, 100.0f, 0.0f, "%+.0f");
            slider("Shadows", shadows, -100.0f, 100.0f, 0.0f, "%+.0f");
            p.contrast = contrast / 100.0f;
            p.highlights = highlights / 100.0f;
            p.shadows = shadows / 100.0f;
            if (changed) content.serializedParams = ExposureNode::pack(p);
        } else if (content.nodeType == "white_balance") {
            WhiteBalanceParams p = unpackParams<WhiteBalanceParams>(content.serializedParams, "white_balance");
            changed |= adjustmentSlider("Temp", p.temperature, 2000.0f, 20000.0f, kReferenceTemperature, "%.0f K",
                                        IM_COL32(70, 120, 230, 255), IM_COL32(235, 200, 60, 255), ImGuiSliderFlags_Logarithmic)
                           .changed;
            slider("Tint", p.tint, -150.0f, 150.0f, 0.0f, "%+.0f", IM_COL32(60, 190, 70, 255), IM_COL32(210, 70, 200, 255));
            if (changed) content.serializedParams = packParams(p);
        } else if (content.nodeType == "tone_curve") {
            ToneCurveParams p = unpackParams<ToneCurveParams>(content.serializedParams, "tone_curve");
            const float size = std::clamp(ImGui::GetContentRegionAvail().x, 120.0f, 260.0f);
            changed |= curveEditor("##layerCurve", p.curves[0], IM_COL32(235, 235, 235, 255), size).changed;
            if (ImGui::BeginCombo("##layerPreset", "Preset...")) {
                for (CurvePreset preset : {CurvePreset::LINEAR, CurvePreset::MEDIUM_CONTRAST, CurvePreset::STRONG_CONTRAST,
                                           CurvePreset::FADED}) {
                    if (ImGui::Selectable(toString(preset))) {
                        p.curves[0] = curvePreset(preset);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (changed) content.serializedParams = packParams(p);
        } else if (content.nodeType == "hsl") {
            HslParams p = unpackParams<HslParams>(content.serializedParams, "hsl");
            static int mode = 0;  // shared by every HSL layer's editor
            constexpr std::array<const char*, 3> kModes{"Hue", "Saturation", "Luminance"};
            for (int m = 0; m < 3; ++m) {
                if (m > 0) ImGui::SameLine();
                if (ImGui::RadioButton(kModes[static_cast<std::size_t>(m)], mode == m)) mode = m;
            }
            std::array<float, kHslBands>& values = mode == 0 ? p.hue : mode == 1 ? p.saturation : p.luminance;
            for (std::size_t band = 0; band < kHslBands; ++band) slider(kHslBandNames[band], values[band], -100.0f, 100.0f, 0.0f, "%+.0f");
            if (changed) content.serializedParams = packParams(p);
        } else if (content.nodeType == "color_grading") {
            ColorGradingParams p = unpackParams<ColorGradingParams>(content.serializedParams, "color_grading");
            slider("Vibrance", p.vibrance, -100.0f, 100.0f, 0.0f, "%+.0f");
            slider("Saturation", p.saturation, -100.0f, 100.0f, 0.0f, "%+.0f");
            const float diameter = std::clamp((ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f, 50.0f, 110.0f);
            ColorWheel* wheels[3] = {&p.shadows, &p.midtones, &p.highlights};
            const char* names[3] = {"Shadows", "Midtones", "Highlights"};
            for (int i = 0; i < 3; ++i) {
                if (i > 0) ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextDisabled("%s", names[i]);
                changed |= colorWheel(names[i], wheels[i]->hue, wheels[i]->saturation, diameter).changed;
                ImGui::EndGroup();
            }
            if (changed) content.serializedParams = packParams(p);
        } else {
            ImGui::TextDisabled("No editor for %s layers", content.nodeType.c_str());
        }
    } catch (const std::invalid_argument&) {
        ImGui::TextDisabled("Unreadable %s parameters", content.nodeType.c_str());
    }
    return changed;
}

}  // namespace

// -----------------------------------------------------------------------------
// Layers
// -----------------------------------------------------------------------------

std::string LayersPanel::nextName(const char* base) { return std::string(base) + " " + std::to_string(nameCounter_++); }

void LayersPanel::addLayer(PanelContext& ctx, std::unique_ptr<LayerNode> layer) {
    ctx.canvas.insertLayer(ctx.app.document(), std::move(layer));
}

void LayersPanel::draw(PanelContext& ctx) {
    LayerNode& root = ctx.app.document();
    ctx.canvas.validate(root);
    LayerNode*& selected = ctx.canvas.selectedLayer;
    const bool canEdit = selected && selected != &root;

    // Blend mode and opacity of the selected layer, above the stack.
    ImGui::BeginDisabled(!canEdit);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    BlendMode mode = canEdit ? selected->blendMode() : BlendMode::NORMAL;
    if (blendModeCombo("##Blend", mode) && canEdit) selected->setBlendMode(mode);
    ImGui::SetItemTooltip("Blend mode");
    ImGui::SameLine();
    float opacity = canEdit ? selected->opacity() * 100.0f : 100.0f;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##Opacity", &opacity, 0.0f, 100.0f, "Opacity %.0f%%") && canEdit) {
        selected->setOpacity(opacity / 100.0f);
        invalidateComposite(root);
    }
    ImGui::EndDisabled();

    // Stack, top to bottom.
    const float toolbarHeight = ImGui::GetFrameHeightWithSpacing();
    const float listHeight = std::max(ImGui::GetContentRegionAvail().y - toolbarHeight, ImGui::GetFrameHeightWithSpacing() * 3.0f);
    if (ImGui::BeginChild("##LayerList", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders)) {
        ImGui::TextDisabled("%s  |  %u x %u", root.name().c_str(), ctx.app.canvasWidth(), ctx.app.canvasHeight());
        for (std::size_t i = root.childCount(); i-- > 0;) drawLayerRow(ctx, root.child(i), 0);
    }
    ImGui::EndChild();

    // Toolbar: add, delete, reorder.
    if (ImGui::Button("+ Add")) ImGui::OpenPopup("##AddLayer");
    drawAddMenu(ctx);
    ImGui::SameLine();
    const bool canDelete = canEdit && rasterCount(root, selected) > 0;
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button("Delete")) {
        LayerNode* parent = selected->parent();
        parent->removeChild(*selected);  // destroyed here
        selected = parent == &root ? nullptr : parent;
        invalidateComposite(root);
    }
    ImGui::EndDisabled();
    if (canEdit && !canDelete) ImGui::SetItemTooltip("The document keeps at least one raster layer");
    ImGui::SameLine();
    const bool editable = selected && selected != &root;  // the selection may have just been deleted
    const std::size_t position = editable ? indexInParent(*selected) : 0;
    ImGui::BeginDisabled(!editable || position + 1 >= (editable ? selected->parent()->childCount() : 0));
    if (ImGui::ArrowButton("##Raise", ImGuiDir_Up)) {
        selected->parent()->moveChild(position, position + 1);  // children are ordered bottom to top
        invalidateComposite(root);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Move up");
    ImGui::SameLine();
    ImGui::BeginDisabled(!editable || position == 0);
    if (ImGui::ArrowButton("##Lower", ImGuiDir_Down)) {
        selected->parent()->moveChild(position, position - 1);
        invalidateComposite(root);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Move down");
}

void LayersPanel::drawLayerRow(PanelContext& ctx, LayerNode& layer, int depth) {
    const LayerStyle style = layerStyle(layer.type());
    ImGui::PushID(&layer);
    ImGui::Indent(static_cast<float>(depth) * ImGui::GetStyle().IndentSpacing);

    bool visible = layer.visible();
    if (ImGui::Checkbox("##Visible", &visible)) {
        layer.setVisible(visible);
        invalidateComposite(ctx.app.document());
    }
    ImGui::SetItemTooltip("Visibility");
    ImGui::SameLine();

    // Type chip.
    const ImVec2 chipPos = ImGui::GetCursorScreenPos();
    const float chipWidth = ImGui::CalcTextSize("VEC").x + 8.0f;
    const float frame = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(chipWidth, frame));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float inset = frame * 0.18f;
    drawList->AddRectFilled(ImVec2(chipPos.x, chipPos.y + inset), ImVec2(chipPos.x + chipWidth, chipPos.y + frame - inset),
                            ImGui::GetColorU32(style.color), 3.0f);
    const ImVec2 chipText = ImGui::CalcTextSize(style.chip);
    drawList->AddText(ImVec2(chipPos.x + (chipWidth - chipText.x) * 0.5f, chipPos.y + (frame - chipText.y) * 0.5f),
                      IM_COL32(20, 20, 22, 255), style.chip);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", style.description);
    ImGui::SameLine();

    // Name, opacity and mask badge.
    char suffix[48];
    std::snprintf(suffix, sizeof suffix, "%s%3.0f%%", layer.mask() ? "[mask] " : "", layer.opacity() * 100.0f);
    const float suffixWidth = ImGui::CalcTextSize(suffix).x;
    ImGui::AlignTextToFramePadding();
    if (ImGui::Selectable(layer.name().c_str(), ctx.canvas.selectedLayer == &layer, ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(std::max(ImGui::GetContentRegionAvail().x - suffixWidth - 8.0f, 20.0f), 0.0f))) {
        ctx.canvas.selectedLayer = &layer;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", suffix);

    ImGui::Unindent(static_cast<float>(depth) * ImGui::GetStyle().IndentSpacing);
    ImGui::PopID();

    if (layer.isGroup()) {
        for (std::size_t i = layer.childCount(); i-- > 0;) drawLayerRow(ctx, layer.child(i), depth + 1);
    }
}

void LayersPanel::drawAddMenu(PanelContext& ctx) {
    if (!ImGui::BeginPopup("##AddLayer")) return;
    const std::uint32_t width = ctx.app.canvasWidth();
    const std::uint32_t height = ctx.app.canvasHeight();
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    ImGui::SeparatorText("Raster");
    if (ImGui::MenuItem("Empty Raster Layer")) {
        addLayer(ctx, LayerNode::createRaster(nextName("Layer"), width, height));
    }
    if (ImGui::MenuItem("Test Chart")) {
        auto layer = LayerNode::createRaster(nextName("Test Chart"), width, height);
        paintTestChart(*layer->raster());
        addLayer(ctx, std::move(layer));
    }
    ImGui::SeparatorText("Adjustment (applies to the layers below)");
    auto adjustment = [&](const char* label, const char* type, std::vector<std::byte> params) {
        if (ImGui::MenuItem(label)) addLayer(ctx, LayerNode::createAdjustment(nextName(label), {type, std::move(params)}));
    };
    adjustment("Exposure", "exposure", ExposureNode::pack({}));
    adjustment("White Balance", "white_balance", packParams(WhiteBalanceParams{}));
    {
        ToneCurveParams curves;
        curves.curves[0] = curvePreset(CurvePreset::MEDIUM_CONTRAST);
        adjustment("Curves", "tone_curve", packParams(curves));
    }
    adjustment("Hue / Saturation (HSL)", "hsl", packParams(HslParams{}));
    adjustment("Color Grading", "color_grading", packParams(ColorGradingParams{}));
    ImGui::SeparatorText("Vector");
    if (ImGui::MenuItem("Rectangle")) {
        addLayer(ctx, LayerNode::createVector(nextName("Rectangle"), rectangleShape(w / 2, h / 2, w / 3, h / 4)));
    }
    if (ImGui::MenuItem("Ellipse")) {
        addLayer(ctx, LayerNode::createVector(nextName("Ellipse"), ellipseShape(w / 2, h / 2, w / 5, h / 5)));
    }
    ImGui::SeparatorText("Other");
    const AssetRecord* asset = ctx.library.selected();
    if (ImGui::MenuItem("Smart Object from Selected Photo", nullptr, false, asset != nullptr)) {
        addLayer(ctx, LayerNode::createSmartObject(asset->fileName, SmartObjectContent{asset->id, nullptr}));
        std::vector<EditNodeRecord> stack;
        try {
            stack = ctx.app.assets().loadEditStack(asset->id);
        } catch (const std::exception&) {
            // no edits: the photo as decoded
        }
        ctx.canvas.loadSmartObject(*ctx.canvas.selectedLayer, asset->filePath, std::move(stack),
                                   std::max(ctx.app.canvasWidth(), ctx.app.canvasHeight()));
    }
    if (asset) ImGui::SetItemTooltip("%s, placed with its exposure, colour and curve edits", asset->fileName.c_str());
    if (ImGui::MenuItem("Group")) addLayer(ctx, LayerNode::createGroup(nextName("Group")));
    ImGui::EndPopup();
}

// -----------------------------------------------------------------------------
// Properties
// -----------------------------------------------------------------------------

void PropertiesPanel::draw(PanelContext& ctx) {
    LayerNode& root = ctx.app.document();
    ctx.canvas.validate(root);
    if (!ctx.canvas.selectedLayer) {
        ImGui::TextDisabled("Select a layer in the Layers panel to edit its properties.");
        return;
    }
    LayerNode& layer = *ctx.canvas.selectedLayer;
    const LayerStyle style = layerStyle(layer.type());
    ImGui::TextDisabled("%s", style.description);

    if (!ImGui::BeginTable("##LayerProperties", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    auto row = [](const char* label) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };

    row("Name");
    char name[128];
    std::snprintf(name, sizeof name, "%s", layer.name().c_str());
    if (ImGui::InputText("##Name", name, sizeof name)) layer.setName(name);

    if (&layer != &root) {
        row("Mask");
        if (!layer.mask()) {
            if (ImGui::SmallButton("Add Mask")) {
                layer.addMask(ctx.app.canvasWidth(), ctx.app.canvasHeight());
                invalidateComposite(root);
            }
        } else {
            bool enabled = layer.maskEnabled();
            if (ImGui::Checkbox("Enabled", &enabled)) {
                layer.setMaskEnabled(enabled);
                invalidateComposite(root);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                layer.removeMask();
                invalidateComposite(root);
            }
        }
        // Develop masks (Masking panel) double as layer masks.
        ImGui::SameLine();
        if (ImGui::SmallButton("From Mask...")) ImGui::OpenPopup("##FromDevelopMask");
        ImGui::SetItemTooltip("Replace the layer mask with a mask from the Masking panel");
        if (ImGui::BeginPopup("##FromDevelopMask")) {
            ctx.masking.masks.sync(ctx.app, ctx.frame.frameIndex);
            const std::vector<LocalAdjustment>& masks = ctx.masking.masks.values().masks;
            if (masks.empty()) ImGui::TextDisabled("No masks yet: add one in the Masking panel");
            for (std::size_t i = 0; i < masks.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::MenuItem(masks[i].name.c_str())) {
                    applyMaskToLayer(masks[i], layer, root, ctx.app.canvasWidth(), ctx.app.canvasHeight());
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }

    row("Content");
    if (const SparseRasterLayer* raster = layer.raster()) {
        ImGui::Text("%zu of %u tiles, %.1f MB", raster->tileCount(), raster->tilesX() * raster->tilesY(),
                    static_cast<double>(raster->residentBytes()) / (1024.0 * 1024.0));
        ImGui::SetItemTooltip("%ux%u FP16, %ux%u tiles allocated on first write", raster->width(), raster->height(),
                              TILE_SIZE, TILE_SIZE);
    } else if (const AdjustmentContent* adjustment = layer.adjustment()) {
        ImGui::Text("%s", adjustment->nodeType.c_str());
    } else if (VectorContent* shape = layer.vectorShape()) {
        ImGui::Text("%zu path verbs", shape->verbs.size());
        bool edited = false;
        row("Fill");
        edited |= ImGui::ColorEdit4("##Fill", shape->fillColor.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        row("Stroke");
        edited |= ImGui::ColorEdit4("##Stroke", shape->strokeColor.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        edited |= ImGui::DragFloat("##StrokeWidth", &shape->strokeWidth, 0.25f, 0.0f, 200.0f, "%.1f px");
        if (edited) invalidateComposite(root);
    } else if (const SmartObjectContent* object = layer.smartObject()) {
        const AssetRecord* source = ctx.library.findAsset(object->sourceAssetId);
        ImGui::TextWrapped("%s", source ? source->fileName.c_str() : object->sourceAssetId.c_str());
        if (ctx.canvas.loadingSmartObject(&layer)) {
            ImGui::TextDisabled("loading...");
        } else if (object->pixels) {
            ImGui::TextDisabled("%u x %u", object->pixels->width(), object->pixels->height());
        } else {
            ImGui::TextDisabled("not loaded");
        }
    } else if (layer.isGroup()) {
        ImGui::Text("%zu layer%s, isolated", layer.childCount(), layer.childCount() == 1 ? "" : "s");
    }
    ImGui::EndTable();

    if (AdjustmentContent* adjustment = layer.adjustment()) {
        ImGui::SeparatorText("Adjustment");
        if (editAdjustment(*adjustment)) invalidateComposite(root);
    }
    if (layer.contentBounds()) drawTransform(layer);
}

// Position, size and rotation of the layer's content (about its centre).
void PropertiesPanel::drawTransform(LayerNode& layer) {
    ImGui::SeparatorText("Transform");
    LayerTransform t = layer.transform();
    if (t.isIdentity()) {
        // Scale and rotate about the content's centre.
        const std::array<float, 4> bounds = *layer.contentBounds();
        t.pivotX = 0.5f * (bounds[0] + bounds[2]);
        t.pivotY = 0.5f * (bounds[1] + bounds[3]);
    }
    bool changed = false;
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(width * 0.62f);
    changed |= ImGui::DragFloat2("Position", &t.translateX, 1.0f, -100000.0f, 100000.0f, "%.0f px");
    ImGui::SetItemTooltip("Offset from the original place, in canvas pixels");
    float scale[2] = {t.scaleX * 100.0f, t.scaleY * 100.0f};
    ImGui::SetNextItemWidth(width * 0.62f);
    if (ImGui::DragFloat2("Size", scale, 0.5f, -1000.0f, 1000.0f, "%.1f %%")) {
        if (linkScale_) {
            // Keep the aspect ratio: follow whichever value moved.
            const bool xMoved = scale[0] != t.scaleX * 100.0f;
            const float ratio = xMoved ? scale[0] / (t.scaleX * 100.0f) : scale[1] / (t.scaleY * 100.0f);
            scale[0] = t.scaleX * 100.0f * ratio;
            scale[1] = t.scaleY * 100.0f * ratio;
        }
        if (std::fabs(scale[0]) >= 0.1f && std::fabs(scale[1]) >= 0.1f) {
            t.scaleX = scale[0] / 100.0f;
            t.scaleY = scale[1] / 100.0f;
            changed = true;
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Link", &linkScale_);
    ImGui::SetNextItemWidth(width * 0.62f);
    changed |= ImGui::SliderFloat("Angle", &t.rotation, -180.0f, 180.0f, "%.1f deg");
    if (ImGui::Button("Flip H")) {
        t.scaleX = -t.scaleX;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Flip V")) {
        t.scaleY = -t.scaleY;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        t = LayerTransform{};
        changed = true;
    }
    if (changed) layer.setTransform(t);
}

}  // namespace darkhouse::ui
