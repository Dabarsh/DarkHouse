// Canvas & Compositing panels: the unified layer stack and the selected
// layer's properties.

#include "ui/panels.hpp"

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

}  // namespace

// -----------------------------------------------------------------------------
// Layers
// -----------------------------------------------------------------------------

std::string LayersPanel::nextName(const char* base) { return std::string(base) + " " + std::to_string(nameCounter_++); }

void LayersPanel::addLayer(PanelContext& ctx, std::unique_ptr<LayerNode> layer) {
    LayerNode& root = ctx.app.document();
    LayerNode*& selected = ctx.canvas.selectedLayer;
    // Above the selection, in the same group; inside a selected group, on top.
    LayerNode* parent = &root;
    std::size_t position = root.childCount();
    if (selected && selected != &root) {
        if (selected->isGroup()) {
            parent = selected;
            position = selected->childCount();
        } else {
            parent = selected->parent();
            position = indexInParent(*selected) + 1;
        }
    }
    LayerNode& added = parent->insertChild(position, std::move(layer));
    selected = &added;
    invalidateComposite(root);
}

void LayersPanel::draw(PanelContext& ctx) {
    LayerNode& root = ctx.app.document();
    ctx.canvas.validate(root);
    LayerNode*& selected = ctx.canvas.selectedLayer;
    const bool canEdit = selected && selected != &root;

    // Blend mode and opacity of the selected layer, above the stack.
    ImGui::BeginDisabled(!canEdit);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    if (ImGui::BeginCombo("##Blend", canEdit ? blendModeName(selected->blendMode()) : "Normal")) {
        for (BlendMode mode : kBlendModes) {
            if (ImGui::Selectable(blendModeName(mode), selected->blendMode() == mode)) {
                selected->setBlendMode(mode);
                invalidateComposite(root);
            }
        }
        ImGui::EndCombo();
    }
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

}  // namespace darkhouse::ui
