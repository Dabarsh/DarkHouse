// Right dock: unified layer stack and develop adjustments.

#include "ui/panels.hpp"

#include "denoise_node.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <glm/common.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace darkhouse::ui {
namespace {

using theme::u32;

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

std::size_t layerCount(const LayerNode& root) {
    std::size_t count = 0;
    root.visit([&](const LayerNode& node, std::size_t) {
        if (&node != &root) ++count;
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

// Inspector form row: secondary label, then the widget filling the rest.
void propertyLabel(const char* label) {
    const float start = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kTextSecondary, "%s", label);
    ImGui::SameLine(start + ImGui::GetFontSize() * 4.2f);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// Header of a section whose GPU node does not exist yet: says so on the row itself.
bool previewSectionHeader(const char* title, bool& open) {
    const float width = pillWidth("Preview");
    const bool isOpen = sectionHeader(title, open, width);
    alignRight(width);
    pill("Preview", theme::kWarning);
    ImGui::SetItemTooltip("Interface preview: this adjustment does not change the image yet.");
    return isOpen;
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
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();
    const std::size_t count = layerCount(root);

    // Header row: the document, then add, delete and reorder.
    {
        char caption[192];
        std::snprintf(caption, sizeof caption, "%s  \xC2\xB7  %zu layer%s", root.name().c_str(), count,
                      count == 1 ? "" : "s");
        const SmallText small;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::kTextSecondary, "%s", caption);
    }
    const bool canEdit = selected_ && selected_ != &root;
    const bool canDelete = canEdit && rasterCount(root, selected_) > 0;
    const std::size_t position = canEdit ? indexInParent(*selected_) : 0;
    const std::size_t siblings = canEdit ? selected_->parent()->childCount() : 0;
    alignRight(4.0f * iconButtonWidth() + 3.0f * 2.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * s, style.ItemSpacing.y));
    if (iconButton("##Add", Icon::PLUS, "Add layer")) ImGui::OpenPopup("##AddLayer");
    drawAddMenu(ctx);
    ImGui::SameLine();
    if (iconButton("##Delete", Icon::TRASH, "Delete layer", false, canDelete)) {
        LayerNode* parent = selected_->parent();
        parent->removeChild(*selected_);  // destroyed here
        selected_ = parent == &root ? nullptr : parent;
        invalidateComposite(root);
    }
    if (canEdit && !canDelete && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("The document keeps at least one raster layer");
    }
    ImGui::SameLine();
    if (iconButton("##Raise", Icon::ARROW_UP, "Move up", false, canEdit && position + 1 < siblings)) {
        selected_->parent()->moveChild(position, position + 1);  // children are ordered bottom to top
        invalidateComposite(root);
    }
    ImGui::SameLine();
    if (iconButton("##Lower", Icon::ARROW_DOWN, "Move down", false, canEdit && position > 0)) {
        selected_->parent()->moveChild(position, position - 1);
        invalidateComposite(root);
    }
    ImGui::PopStyleVar();

    // Stack, top to bottom: as tall as its rows, between three rows and half the panel.
    const float rowHeight = ImGui::GetFrameHeight();
    const float maxHeight = std::max(ImGui::GetContentRegionAvail().y * 0.5f, rowHeight * 3.0f);
    const float listHeight = std::clamp(rowHeight * static_cast<float>(count), rowHeight * 3.0f, maxHeight);
    if (ImGui::BeginChild("##LayerList", ImVec2(0.0f, listHeight))) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));  // rows abut
        for (std::size_t i = root.childCount(); i-- > 0;) drawLayerRow(ctx, root.child(i), 0);
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    if (selected_) {
        drawProperties(ctx, *selected_);
    } else {
        const SmallText small;
        ImGui::TextColored(theme::kTextSecondary, "Select a layer to edit its properties.");
    }
}

void LayersPanel::drawLayerRow(PanelContext& ctx, LayerNode& layer, int depth) {
    const LayerStyle look = layerStyle(layer.type());
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();
    const float height = ImGui::GetFrameHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float indent = static_cast<float>(depth) * style.IndentSpacing;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImGui::PushID(&layer);

    // The whole row selects; the visibility toggle sits over it.
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##Row", ImVec2(width, height))) selected_ = &layer;
    const bool hovered = ImGui::IsItemHovered();
    if (selected_ == &layer) {
        drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), u32(theme::kSelected), style.FrameRounding);
    } else if (hovered) {
        drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), u32(theme::kFillControl), style.FrameRounding);
    }

    ImGui::SetCursorScreenPos(ImVec2(pos.x + 2.0f * s + indent, pos.y));
    const bool visible = layer.visible();
    if (iconButton("##Visible", visible ? Icon::EYE : Icon::EYE_OFF, visible ? "Hide layer" : "Show layer")) {
        layer.setVisible(!visible);
        invalidateComposite(ctx.app.document());
    }
    const float afterEye = ImGui::GetItemRectMax().x + 4.0f * s;
    ImGui::PopID();

    float nameX = afterEye;
    float trailing = pos.x + width - 8.0f * s;
    {
        const SmallText small;
        // Type chip: tinted, so the stack is not a column of saturated blocks.
        const ImVec2 chipText = ImGui::CalcTextSize(look.chip);
        const float chipWidth = ImGui::CalcTextSize("VEC").x + 8.0f * s;
        const ImVec2 chipMin(afterEye, std::floor(pos.y + (height - chipText.y - 2.0f * s) * 0.5f));
        const ImVec2 chipMax(chipMin.x + chipWidth, chipMin.y + chipText.y + 2.0f * s);
        drawList->AddRectFilled(chipMin, chipMax, u32(look.color, 0.19f), 4.0f * s);
        drawList->AddText(ImVec2(chipMin.x + (chipWidth - chipText.x) * 0.5f, chipMin.y + 1.0f * s), u32(look.color),
                          look.chip);
        if (hovered && ImGui::IsMouseHoveringRect(chipMin, chipMax)) {
            ImGui::SetTooltip("%s%s", look.description,
                              look.cpuComposited ? "" : "\nNot drawn yet: GPU compositing of this layer type is a later milestone.");
        }
        nameX = chipMax.x + 8.0f * s;

        // Trailing: opacity, and a pill when the layer is masked.
        char opacity[16];
        std::snprintf(opacity, sizeof opacity, "%.0f%%", static_cast<double>(layer.opacity()) * 100.0);
        const ImVec2 opacitySize = ImGui::CalcTextSize(opacity);
        trailing -= ImGui::CalcTextSize("100%").x;
        drawList->AddText(ImVec2(pos.x + width - 8.0f * s - opacitySize.x, pos.y + (height - opacitySize.y) * 0.5f),
                          u32(theme::kTextSecondary), opacity);
        if (layer.mask()) {
            const ImVec2 maskText = ImGui::CalcTextSize("Mask");
            const float maskWidth = maskText.x + 10.0f * s;
            const ImVec2 maskMin(trailing - 8.0f * s - maskWidth, std::floor(pos.y + (height - maskText.y - 2.0f * s) * 0.5f));
            const ImVec2 maskMax(maskMin.x + maskWidth, maskMin.y + maskText.y + 2.0f * s);
            const ImVec4& tint = layer.maskEnabled() ? theme::kAccentBright : theme::kTextTertiary;
            drawList->AddRectFilled(maskMin, maskMax, u32(tint, 0.16f), (maskMax.y - maskMin.y) * 0.5f);
            drawList->AddText(ImVec2(maskMin.x + 5.0f * s, maskMin.y + 1.0f * s), u32(tint), "Mask");
            trailing = maskMin.x;
        }
    }
    drawList->PushClipRect(ImVec2(nameX, pos.y), ImVec2(std::max(trailing - 6.0f * s, nameX), pos.y + height), true);
    drawList->AddText(ImVec2(nameX, pos.y + style.FramePadding.y), u32(visible ? theme::kText : theme::kTextTertiary),
                      layer.name().c_str());
    drawList->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height));  // the next row starts below this one

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

void LayersPanel::drawProperties(PanelContext& ctx, LayerNode& layer) {
    LayerNode& root = ctx.app.document();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();

    // The selected layer's properties, as one inset group under the stack.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kFillGroup);
    const bool visible = ImGui::BeginChild("##LayerProperties", ImVec2(0.0f, 0.0f),
                                           ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                                               ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    if (!visible) {
        ImGui::EndChild();
        return;
    }

    propertyLabel("Name");
    char name[128];
    std::snprintf(name, sizeof name, "%s", layer.name().c_str());
    if (ImGui::InputText("##Name", name, sizeof name)) layer.setName(name);

    if (&layer != &root) {
        // Blend mode and opacity share a row, as in most layer inspectors.
        propertyLabel("Blend");
        const float opacityWidth = ImGui::GetFontSize() * 4.4f;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - opacityWidth - style.ItemInnerSpacing.x, 1.0f));
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
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        float opacity = layer.opacity() * 100.0f;
        ImGui::SetNextItemWidth(opacityWidth);
        if (ImGui::DragFloat("##Opacity", &opacity, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            layer.setOpacity(opacity / 100.0f);
            invalidateComposite(root);
        }
        ImGui::SetItemTooltip("Opacity: drag, or double-click to type");

        propertyLabel("Mask");
        if (!layer.mask()) {
            if (ImGui::Button("Add Mask")) {
                layer.addMask(ctx.app.canvasWidth(), ctx.app.canvasHeight());
                invalidateComposite(root);
            }
        } else {
            bool enabled = layer.maskEnabled();
            if (miniCheckbox("##MaskEnabled", enabled, "Enabled")) {
                layer.setMaskEnabled(enabled);
                invalidateComposite(root);
            }
            ImGui::SameLine(0.0f, 12.0f * s);
            if (ImGui::Button("Remove")) {
                layer.removeMask();
                invalidateComposite(root);
            }
        }
    }

    if (VectorContent* shape = layer.vectorShape()) {
        propertyLabel("Fill");
        ImGui::ColorEdit4("##Fill", shape->fillColor.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        propertyLabel("Stroke");
        ImGui::ColorEdit4("##Stroke", shape->strokeColor.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##StrokeWidth", &shape->strokeWidth, 0.25f, 0.0f, 200.0f, "%.1f px");
    }

    // What the layer holds, as a footnote.
    {
        const SmallText small;
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);
        if (const SparseRasterLayer* raster = layer.raster()) {
            ImGui::Text("%zu of %u tiles  \xC2\xB7  %.1f MB", raster->tileCount(), raster->tilesX() * raster->tilesY(),
                        static_cast<double>(raster->residentBytes()) / (1024.0 * 1024.0));
            ImGui::SetItemTooltip("%ux%u FP16, %ux%u tiles allocated on first write", raster->width(), raster->height(),
                                  TILE_SIZE, TILE_SIZE);
        } else if (const AdjustmentContent* adjustment = layer.adjustment()) {
            ImGui::Text("%s node  \xC2\xB7  %zu parameter bytes", adjustment->nodeType.c_str(),
                        adjustment->serializedParams.size());
        } else if (const VectorContent* shape = layer.vectorShape()) {
            ImGui::Text("%zu path verbs", shape->verbs.size());
        } else if (const SmartObjectContent* object = layer.smartObject()) {
            const AssetRecord* source = ctx.library.findAsset(object->sourceAssetId);
            ImGui::TextWrapped("%s", source ? source->fileName.c_str() : object->sourceAssetId.c_str());
        } else if (layer.isGroup()) {
            ImGui::Text("%zu layer%s, isolated", layer.childCount(), layer.childCount() == 1 ? "" : "s");
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

// -----------------------------------------------------------------------------
// Adjustments
// -----------------------------------------------------------------------------

void AdjustmentsPanel::draw(PanelContext& ctx) {
    drawTone(ctx);
    drawNoiseReduction(ctx);
    drawColor();
    drawHsl();
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

    // Reset lives in the header, lit only while there is something to reset.
    bool changed = false;
    bool released = false;
    const bool open = sectionHeader("Tone", toneOpen_, iconButtonWidth());
    const bool modified = tone_.exposureEV != 0.0f || tone_.contrast != 0.0f || tone_.highlights != 0.0f ||
                          tone_.shadows != 0.0f;
    alignRight(iconButtonWidth());
    if (iconButton("##ResetTone", Icon::RESET, "Reset Tone", false, node && modified)) {
        tone_ = ExposureParams{};
        changed = released = true;
    }

    if (open && !node) {
        const SmallText small;
        ImGui::TextColored(theme::kTextSecondary, "The develop stack has no exposure node.");
    }
    if (open && node) {
        float contrast = tone_.contrast * 100.0f;
        float highlights = tone_.highlights * 100.0f;
        float shadows = tone_.shadows * 100.0f;
        SliderResult results[4];
        results[0] = adjustmentSlider("Exposure", tone_.exposureEV, -5.0f, 5.0f, 0.0f, "%+.2f EV");
        results[1] = adjustmentSlider("Contrast", contrast, -100.0f, 100.0f, 0.0f, "%+.0f");
        results[2] = adjustmentSlider("Highlights", highlights, -100.0f, 100.0f, 0.0f, "%+.0f");
        results[3] = adjustmentSlider("Shadows", shadows, -100.0f, 100.0f, 0.0f, "%+.0f");
        tone_.contrast = contrast / 100.0f;
        tone_.highlights = highlights / 100.0f;
        tone_.shadows = shadows / 100.0f;
        bool sliderChanged = false;
        bool sliderReleased = false;
        for (const SliderResult& result : results) {
            sliderChanged |= result.changed;
            sliderReleased |= result.released;
        }
        toneEditing_ = ImGui::IsAnyItemActive() && (sliderChanged || toneEditing_) && !sliderReleased;
        changed |= sliderChanged;
        released |= sliderReleased;
    } else {
        toneEditing_ = false;
    }
    if (node && (changed || released)) {
        // Live while dragging; written to the catalog when the drag ends.
        ctx.app.postEvent(SetDevelopParamsEvent{*node, ExposureNode::pack(tone_), released});
    }
}

void AdjustmentsPanel::drawNoiseReduction(PanelContext& ctx) {
    const std::vector<EditNodeRecord>& stack = ctx.app.developStack();
    std::optional<std::uint32_t> node;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (stack[i].nodeType == DenoiseNode::kTypeName && stack[i].serializedParams.size() == sizeof(DenoiseParams)) {
            node = static_cast<std::uint32_t>(i);
            if (!noiseEditing_) std::memcpy(&noise_, stack[i].serializedParams.data(), sizeof noise_);
            break;
        }
    }
    const float s = theme::scale();

    // Header: reset, then the switch that adds or removes the node.
    bool changed = false;
    bool released = false;
    const float checkbox = std::floor(ImGui::GetFontSize() * 0.93f);
    const float trailing = iconButtonWidth() + 6.0f * s + checkbox + 4.0f * s;
    const bool open = sectionHeader("Noise Reduction", noiseOpen_, trailing);
    const DenoiseParams defaults;
    const bool modified = noise_.luminance != defaults.luminance || noise_.chrominance != defaults.chrominance ||
                          noise_.detail != defaults.detail || noise_.noiseLevel != defaults.noiseLevel;
    alignRight(trailing);
    if (iconButton("##ResetNoise", Icon::RESET, "Reset Noise Reduction", false, node && modified)) {
        noise_ = DenoiseParams{};
        changed = released = true;
    }
    ImGui::SameLine(0.0f, 6.0f * s);
    bool enabled = node.has_value();
    if (miniCheckbox("##EnableNoise", enabled)) {
        std::vector<EditNodeRecord> next = stack;
        if (enabled) {
            // First in the stack: the noise model assumes scene-linear light, before any tone change.
            next.insert(next.begin(), EditNodeRecord{0, std::string(DenoiseNode::kTypeName), DenoiseNode::pack(noise_)});
        } else {
            next.erase(next.begin() + *node);
        }
        ctx.app.postEvent(SetDevelopStackEvent{std::move(next), true});
    }
    ImGui::SetItemTooltip("%s", node ? "Noise reduction is on (multiscale, GPU)" : "Turn noise reduction on");
    if (!node) {
        noiseEditing_ = false;
        if (open) {
            const SmallText small;
            ImGui::TextColored(theme::kTextSecondary, "Off. Tick the box to denoise this photo.");
        }
        return;
    }

    const auto* denoise = dynamic_cast<const DenoiseNode*>(ctx.app.developNode(*node));
    const DenoiseStatistics stats = denoise ? denoise->statistics() : DenoiseStatistics{};
    if (open) {
        float luminance = noise_.luminance * 100.0f;
        float colour = noise_.chrominance * 100.0f;
        float detail = noise_.detail * 100.0f;
        SliderResult results[4];
        results[0] = adjustmentSlider("Luminance", luminance, 0.0f, 100.0f, 50.0f, "%.0f");
        results[1] = adjustmentSlider("Color", colour, 0.0f, 100.0f, 50.0f, "%.0f");
        results[2] = adjustmentSlider("Detail", detail, 0.0f, 100.0f, 20.0f, "%.0f");
        noise_.luminance = luminance / 100.0f;
        noise_.chrominance = colour / 100.0f;
        noise_.detail = detail / 100.0f;

        // Noise level: measured from the image on every render, or set by hand
        // (sigma after the square-root variance stabilisation, see denoise.hpp).
        constexpr float kMinLevel = 0.001f;
        constexpr float kMaxLevel = 0.25f;
        bool automatic = noise_.noiseLevel <= 0.0f;
        if (miniCheckbox("##AutoNoise", automatic, "Auto noise level")) {
            // Manual starts from the current estimate, so the image does not jump.
            const float measured = stats.sigma[0] > 0.0f ? stats.sigma[0] : 0.02f;
            noise_.noiseLevel = automatic ? 0.0f : std::clamp(measured, kMinLevel, kMaxLevel);
            results[3].changed = results[3].released = true;
        }
        if (automatic && stats.sigma[0] > 0.0f) {
            char measured[64];
            std::snprintf(measured, sizeof measured, "\xCF\x83 %.4f", static_cast<double>(stats.sigma[0]));
            const SmallText small;
            alignRight(ImGui::CalcTextSize(measured).x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(theme::kTextSecondary, "%s", measured);
            ImGui::SetItemTooltip("Measured noise sigma: luma %.4f, color %.4f / %.4f.\n"
                                  "Estimated from the finest wavelet band (median absolute deviation)\n"
                                  "in the square-root domain.",
                                  static_cast<double>(stats.sigma[0]), static_cast<double>(stats.sigma[1]),
                                  static_cast<double>(stats.sigma[2]));
        }
        if (!automatic) {
            const SliderResult level = adjustmentSlider("Noise level", noise_.noiseLevel, kMinLevel, kMaxLevel, 0.02f,
                                                        "%.4f", 0, 0, ImGuiSliderFlags_Logarithmic);
            results[3].changed |= level.changed;
            results[3].released |= level.released;
        }
        if (stats.levels > 0) {
            std::string line = std::to_string(stats.levels) + (stats.levels == 1 ? " detail band" : " detail bands");
            for (const auto& timing : ctx.app.developTimings()) {
                if (timing.id != *node) continue;
                char gpuTime[48];
                std::snprintf(gpuTime, sizeof gpuTime, "  \xC2\xB7  %.1f ms on the GPU", timing.milliseconds);
                line += gpuTime;
            }
            const SmallText small;
            ImGui::TextColored(theme::kTextSecondary, "%s", line.c_str());
        }

        bool sliderChanged = false;
        bool sliderReleased = false;
        for (const SliderResult& result : results) {
            sliderChanged |= result.changed;
            sliderReleased |= result.released;
        }
        noiseEditing_ = ImGui::IsAnyItemActive() && (sliderChanged || noiseEditing_) && !sliderReleased;
        changed |= sliderChanged;
        released |= sliderReleased;
    } else {
        noiseEditing_ = false;
    }
    if (changed || released) {
        ctx.app.postEvent(SetDevelopParamsEvent{*node, DenoiseNode::pack(noise_), released});
    }
}

void AdjustmentsPanel::drawColor() {
    if (!previewSectionHeader("White Balance & Presence", colorOpen_)) return;
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
    if (!previewSectionHeader("HSL / Color", hslOpen_)) return;
    segmented("##HslMode", hslMode_, {"Hue", "Saturation", "Luminance"}, std::floor(ImGui::GetContentRegionAvail().x / 3.0f));
    std::array<float, 8>& values = hslMode_ == 0 ? hue_ : hslMode_ == 1 ? hslSaturation_ : luminance_;
    ImGui::PushID(hslMode_);
    for (std::size_t i = 0; i < kBands.size(); ++i) {
        const float h = kBands[i].hue;
        ImU32 left = 0;
        ImU32 right = 0;
        switch (hslMode_) {
        case 0: left = hsv(h - 1.0f / 12.0f, 0.75f, 0.85f); right = hsv(h + 1.0f / 12.0f, 0.75f, 0.85f); break;
        case 1: left = hsv(h, 0.0f, 0.55f); right = hsv(h, 1.0f, 0.9f); break;
        default: left = hsv(h, 0.8f, 0.15f); right = hsv(h, 0.35f, 1.0f); break;
        }
        adjustmentSlider(kBands[i].name, values[i], -100.0f, 100.0f, 0.0f, "%+.0f", left, right);
    }
    ImGui::PopID();
    if (ImGui::Button("Reset")) values.fill(0.0f);
}

}  // namespace darkhouse::ui
