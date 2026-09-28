// Right dock: unified layer stack and develop adjustments.

#include "ui/panels.hpp"
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
    bool cpuComposited;  // rendered by today's CPU compositor
};

LayerStyle layerStyle(LayerType type) {
    switch (type) {
    case LayerType::PARAMETRIC_ADJUSTMENT:
        return {"ADJ", theme::kLayerParametric, "Parametric adjustment (GPU operator on everything below)", false};
    case LayerType::RASTER_PIXEL: return {"PX", theme::kLayerRaster, "Raster pixels (sparse FP16 tiles)", true};
    case LayerType::VECTOR_SHAPE: return {"VEC", theme::kLayerVector, "Vector shape (resolution independent)", false};
    case LayerType::SMART_OBJECT: return {"OBJ", theme::kLayerSmart, "Smart object (another catalog asset)", false};
    case LayerType::GROUP: return {"GRP", theme::kLayerGroup, "Group (composited in isolation)", true};
    }
    return {"?", theme::kLayerGroup, "", false};
}

const char* blendModeName(BlendMode mode) {
    switch (mode) {
    case BlendMode::NORMAL: return "Normal";
    case BlendMode::MULTIPLY: return "Multiply";
    case BlendMode::SCREEN: return "Screen";
    case BlendMode::OVERLAY: return "Overlay";
    case BlendMode::COLOR_DODGE: return "Color Dodge";
    }
    return "?";
}

// Re-dirties every raster tile and mask under `root`. Property changes
// (visibility, opacity, blend, masks, order) do not touch pixels, so without
// this the engine would keep showing the old composite.
void invalidateComposite(LayerNode& root) {
    root.visit([](LayerNode& node, std::size_t) {
        if (SparseRasterLayer* raster = node.raster()) raster->markDirtyRegion(0, 0, raster->width(), raster->height());
        if (SparseRasterLayer* mask = node.mask()) mask->markDirtyRegion(0, 0, mask->width(), mask->height());
    });
}

bool containsLayer(const LayerNode& root, const LayerNode* target) {
    bool found = false;
    root.visit([&](const LayerNode& node, std::size_t) { found = found || &node == target; });
    return found;
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

// --- Adjustment helpers ----------------------------------------------------------

ImU32 hsv(float h, float s, float v) {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(h - std::floor(h), s, v, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, 1.0f));
}

struct HslBand {
    const char* name;
    float hue;  // 0..1
};
constexpr std::array<HslBand, 8> kBands{{{"Red", 0.0f / 360.0f},
                                         {"Orange", 30.0f / 360.0f},
                                         {"Yellow", 55.0f / 360.0f},
                                         {"Green", 120.0f / 360.0f},
                                         {"Aqua", 180.0f / 360.0f},
                                         {"Blue", 225.0f / 360.0f},
                                         {"Purple", 270.0f / 360.0f},
                                         {"Magenta", 310.0f / 360.0f}}};

}  // namespace

// -----------------------------------------------------------------------------
// Layers
// -----------------------------------------------------------------------------

std::string LayersPanel::nextName(const char* base) { return std::string(base) + " " + std::to_string(nameCounter_++); }

void LayersPanel::addLayer(PanelContext& ctx, std::unique_ptr<LayerNode> layer) {
    LayerNode& root = ctx.app.document();
    // Above the selection, in the same group; inside a selected group, on top.
    LayerNode* parent = &root;
    std::size_t position = root.childCount();
    if (selected_ && selected_ != &root) {
        if (selected_->isGroup()) {
            parent = selected_;
            position = selected_->childCount();
        } else {
            parent = selected_->parent();
            position = indexInParent(*selected_) + 1;
        }
    }
    LayerNode& added = parent->insertChild(position, std::move(layer));
    selected_ = &added;
    invalidateComposite(root);
}

void LayersPanel::draw(PanelContext& ctx) {
    LayerNode& root = ctx.app.document();
    if (selected_ && !containsLayer(root, selected_)) selected_ = nullptr;

    // Toolbar: add, delete, reorder.
    if (ImGui::Button("+ Add")) ImGui::OpenPopup("##AddLayer");
    drawAddMenu(ctx);
    ImGui::SameLine();
    const bool canEdit = selected_ && selected_ != &root;
    const bool canDelete = canEdit && rasterCount(root, selected_) > 0;
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button("Delete")) {
        LayerNode* parent = selected_->parent();
        parent->removeChild(*selected_);  // destroyed here
        selected_ = parent == &root ? nullptr : parent;
        invalidateComposite(root);
    }
    ImGui::EndDisabled();
    if (canEdit && !canDelete) ImGui::SetItemTooltip("The document keeps at least one raster layer");
    ImGui::SameLine();
    const std::size_t position = canEdit ? indexInParent(*selected_) : 0;
    ImGui::BeginDisabled(!canEdit || position + 1 >= (canEdit ? selected_->parent()->childCount() : 0));
    if (ImGui::ArrowButton("##Raise", ImGuiDir_Up)) {
        selected_->parent()->moveChild(position, position + 1);  // children are ordered bottom to top
        invalidateComposite(root);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Move up");
    ImGui::SameLine();
    ImGui::BeginDisabled(!canEdit || position == 0);
    if (ImGui::ArrowButton("##Lower", ImGuiDir_Down)) {
        selected_->parent()->moveChild(position, position - 1);
        invalidateComposite(root);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Move down");

    // Stack, top to bottom.
    const float listHeight = std::max(ImGui::GetContentRegionAvail().y * 0.5f, ImGui::GetFrameHeightWithSpacing() * 4.0f);
    if (ImGui::BeginChild("##LayerList", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders)) {
        ImGui::TextDisabled("%s", root.name().c_str());
        for (std::size_t i = root.childCount(); i-- > 0;) drawLayerRow(ctx, root.child(i), 0);
    }
    ImGui::EndChild();

    if (selected_) {
        drawProperties(ctx, *selected_);
    } else {
        ImGui::TextDisabled("Select a layer to edit its properties.");
    }
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
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s%s", style.description,
                          style.cpuComposited ? "" : "\nNot drawn yet: GPU compositing of this layer type is a later milestone.");
    }
    ImGui::SameLine();

    // Name, opacity and mask badge.
    char suffix[48];
    std::snprintf(suffix, sizeof suffix, "%s%3.0f%%", layer.mask() ? "[mask] " : "", layer.opacity() * 100.0f);
    const float suffixWidth = ImGui::CalcTextSize(suffix).x;
    ImGui::AlignTextToFramePadding();
    if (ImGui::Selectable(layer.name().c_str(), selected_ == &layer, ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(std::max(ImGui::GetContentRegionAvail().x - suffixWidth - 8.0f, 20.0f), 0.0f))) {
        selected_ = &layer;
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
    const AppConfig& config = ctx.app.config();
    const float w = static_cast<float>(config.canvasWidth);
    const float h = static_cast<float>(config.canvasHeight);

    ImGui::SeparatorText("Raster");
    if (ImGui::MenuItem("Empty Raster Layer")) {
        addLayer(ctx, LayerNode::createRaster(nextName("Layer"), config.canvasWidth, config.canvasHeight));
    }
    if (ImGui::MenuItem("Test Chart")) {
        auto layer = LayerNode::createRaster(nextName("Test Chart"), config.canvasWidth, config.canvasHeight);
        paintTestChart(*layer->raster());
        addLayer(ctx, std::move(layer));
    }
    ImGui::SeparatorText("Parametric");
    if (ImGui::MenuItem("Exposure Adjustment")) {
        addLayer(ctx, LayerNode::createAdjustment(nextName("Exposure"),
                                                  {std::string(ExposureNode::kTypeName), ExposureNode::pack({})}));
    }
    if (ImGui::MenuItem("HSL Adjustment")) {
        addLayer(ctx, LayerNode::createAdjustment(nextName("HSL"),
                                                  {"hsl", std::vector<std::byte>(24 * sizeof(float), std::byte{0})}));
    }
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
        addLayer(ctx, LayerNode::createSmartObject(asset->fileName, SmartObjectContent{asset->id, {1, 0, 0, 1, 0, 0}}));
    }
    if (ImGui::MenuItem("Group")) addLayer(ctx, LayerNode::createGroup(nextName("Group")));
    ImGui::EndPopup();
}

void LayersPanel::drawProperties(PanelContext& ctx, LayerNode& layer) {
    LayerNode& root = ctx.app.document();
    ImGui::SeparatorText("Properties");
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
        row("Blend");
        if (ImGui::BeginCombo("##Blend", blendModeName(layer.blendMode()))) {
            for (BlendMode mode : {BlendMode::NORMAL, BlendMode::MULTIPLY, BlendMode::SCREEN, BlendMode::OVERLAY,
                                   BlendMode::COLOR_DODGE}) {
                if (ImGui::Selectable(blendModeName(mode), layer.blendMode() == mode)) {
                    layer.setBlendMode(mode);
                    invalidateComposite(root);
                }
            }
            ImGui::EndCombo();
        }
        row("Opacity");
        float opacity = layer.opacity() * 100.0f;
        if (ImGui::SliderFloat("##Opacity", &opacity, 0.0f, 100.0f, "%.0f%%")) {
            layer.setOpacity(opacity / 100.0f);
            invalidateComposite(root);
        }
        row("Mask");
        if (!layer.mask()) {
            if (ImGui::SmallButton("Add Mask")) {
                layer.addMask(ctx.app.config().canvasWidth, ctx.app.config().canvasHeight);
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
    }

    row("Content");
    if (const SparseRasterLayer* raster = layer.raster()) {
        ImGui::Text("%zu of %u tiles, %.1f MB", raster->tileCount(), raster->tilesX() * raster->tilesY(),
                    static_cast<double>(raster->residentBytes()) / (1024.0 * 1024.0));
        ImGui::SetItemTooltip("%ux%u FP16, %ux%u tiles allocated on first write", raster->width(), raster->height(),
                              TILE_SIZE, TILE_SIZE);
    } else if (const AdjustmentContent* adjustment = layer.adjustment()) {
        ImGui::Text("%s node, %zu parameter bytes", adjustment->nodeType.c_str(), adjustment->serializedParams.size());
    } else if (VectorContent* shape = layer.vectorShape()) {
        ImGui::Text("%zu path verbs", shape->verbs.size());
        row("Fill");
        ImGui::ColorEdit4("##Fill", shape->fillColor.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        row("Stroke");
        ImGui::ColorEdit4("##Stroke", shape->strokeColor.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##StrokeWidth", &shape->strokeWidth, 0.25f, 0.0f, 200.0f, "%.1f px");
    } else if (const SmartObjectContent* object = layer.smartObject()) {
        const AssetRecord* source = ctx.library.findAsset(object->sourceAssetId);
        ImGui::TextWrapped("%s", source ? source->fileName.c_str() : object->sourceAssetId.c_str());
    } else if (layer.isGroup()) {
        ImGui::Text("%zu layer%s, isolated", layer.childCount(), layer.childCount() == 1 ? "" : "s");
    }
    ImGui::EndTable();
}

// -----------------------------------------------------------------------------
// Adjustments
// -----------------------------------------------------------------------------

void AdjustmentsPanel::draw(PanelContext& ctx) {
    const AssetRecord* asset = ctx.library.findAsset(ctx.app.activeAssetId());
    ImGui::TextDisabled("Develop  |  %s", asset ? asset->fileName.c_str() : "document (no photo open)");
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Tone", ImGuiTreeNodeFlags_DefaultOpen)) drawTone(ctx);
    if (ImGui::CollapsingHeader("White Balance & Presence", ImGuiTreeNodeFlags_DefaultOpen)) drawColor();
    if (ImGui::CollapsingHeader("HSL / Color", ImGuiTreeNodeFlags_DefaultOpen)) drawHsl();

    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("Tone runs live on the GPU develop graph and is saved with the photo. White balance, "
                        "presence and HSL are interface previews; their GPU nodes are not implemented yet.");
    ImGui::PopTextWrapPos();
}

void AdjustmentsPanel::drawTone(PanelContext& ctx) {
    // Find the exposure node of the develop stack currently on the canvas.
    const std::vector<EditNodeRecord>& stack = ctx.app.developStack();
    std::optional<std::uint32_t> node;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (stack[i].nodeType == ExposureNode::kTypeName && stack[i].serializedParams.size() == sizeof(ExposureParams)) {
            node = static_cast<std::uint32_t>(i);
            if (!toneEditing_) std::memcpy(&tone_, stack[i].serializedParams.data(), sizeof tone_);
            break;
        }
    }
    if (!node) {
        ImGui::TextDisabled("The develop stack has no exposure node.");
        return;
    }

    float contrast = tone_.contrast * 100.0f;
    float highlights = tone_.highlights * 100.0f;
    float shadows = tone_.shadows * 100.0f;
    SliderResult results[4];
    results[0] = adjustmentSlider("Exposure", tone_.exposureEV, -5.0f, 5.0f, 0.0f, "%+.2f EV",
                                  IM_COL32(20, 20, 20, 255), IM_COL32(235, 235, 235, 255));
    results[1] = adjustmentSlider("Contrast", contrast, -100.0f, 100.0f, 0.0f, "%+.0f");
    results[2] = adjustmentSlider("Highlights", highlights, -100.0f, 100.0f, 0.0f, "%+.0f");
    results[3] = adjustmentSlider("Shadows", shadows, -100.0f, 100.0f, 0.0f, "%+.0f");
    tone_.contrast = contrast / 100.0f;
    tone_.highlights = highlights / 100.0f;
    tone_.shadows = shadows / 100.0f;

    bool changed = false;
    bool released = false;
    for (const SliderResult& result : results) {
        changed |= result.changed;
        released |= result.released;
    }
    toneEditing_ = ImGui::IsAnyItemActive() && (changed || toneEditing_) && !released;
    if (ImGui::SmallButton("Reset Tone")) {
        tone_ = ExposureParams{};
        changed = released = true;
    }
    if (changed || released) {
        // Live while dragging; written to the catalog when the drag ends.
        ctx.app.postEvent(SetDevelopParamsEvent{*node, ExposureNode::pack(tone_), released});
    }
}

void AdjustmentsPanel::drawColor() {
    adjustmentSlider("Temperature", temperature_, 2000.0f, 12000.0f, 5500.0f, "%.0f K", IM_COL32(70, 120, 230, 255),
                     IM_COL32(240, 200, 70, 255));
    adjustmentSlider("Tint", tint_, -150.0f, 150.0f, 0.0f, "%+.0f", IM_COL32(60, 190, 80, 255),
                     IM_COL32(210, 70, 200, 255));
    adjustmentSlider("Vibrance", vibrance_, -100.0f, 100.0f, 0.0f, "%+.0f", IM_COL32(128, 128, 128, 255),
                     IM_COL32(230, 110, 60, 255));
    adjustmentSlider("Saturation", saturation_, -100.0f, 100.0f, 0.0f, "%+.0f", IM_COL32(128, 128, 128, 255),
                     IM_COL32(230, 60, 60, 255));
}

void AdjustmentsPanel::drawHsl() {
    if (!ImGui::BeginTabBar("##HslMode")) return;
    auto bandSliders = [&](std::array<float, 8>& values, int mode) {
        for (std::size_t i = 0; i < kBands.size(); ++i) {
            const float h = kBands[i].hue;
            ImU32 left = 0;
            ImU32 right = 0;
            switch (mode) {
            case 0: left = hsv(h - 1.0f / 12.0f, 0.75f, 0.85f); right = hsv(h + 1.0f / 12.0f, 0.75f, 0.85f); break;
            case 1: left = hsv(h, 0.0f, 0.55f); right = hsv(h, 1.0f, 0.9f); break;
            default: left = hsv(h, 0.8f, 0.15f); right = hsv(h, 0.35f, 1.0f); break;
            }
            adjustmentSlider(kBands[i].name, values[i], -100.0f, 100.0f, 0.0f, "%+.0f", left, right);
        }
        if (ImGui::SmallButton("Reset")) values.fill(0.0f);
    };
    if (ImGui::BeginTabItem("Hue")) {
        bandSliders(hue_, 0);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Saturation")) {
        bandSliders(hslSaturation_, 1);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Luminance")) {
        bandSliders(luminance_, 2);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

}  // namespace darkhouse::ui
