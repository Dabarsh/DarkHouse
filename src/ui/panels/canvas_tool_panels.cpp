// Canvas & Compositing panels for tools, channels and paths.

#include "ui/panels.hpp"

#include "tone_curve.hpp"
#include "ui/canvas_state.hpp"
#include "ui/canvas_tools.hpp"
#include "ui/masking.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace darkhouse::ui {
namespace {

constexpr std::array<CanvasTool, 8> kTools{CanvasTool::NONE,  CanvasTool::MOVE, CanvasTool::BRUSH,         CanvasTool::ERASER,
                                           CanvasTool::CLONE, CanvasTool::PEN,  CanvasTool::DIRECT_SELECT, CanvasTool::EYEDROPPER};

const char* toolHint(CanvasTool tool) {
    switch (tool) {
    case CanvasTool::NONE: return "Drag to pan, wheel to zoom, double-click to toggle fit / 100 %.";
    case CanvasTool::MOVE: return "Drag inside the selected layer to move it, a corner to scale it, outside it to rotate (Shift: 15 degree steps).";
    case CanvasTool::BRUSH: return "Paint the selected pixel layer, or its mask (white reveals, black hides). [ and ] change the size.";
    case CanvasTool::ERASER: return "Erase the selected layer's pixels, or reveal its mask again.";
    case CanvasTool::CLONE: return "Alt+click a source, then paint copies of it onto the selected layer.";
    case CanvasTool::PEN: return "Click to place corners. Click the first point to close a filled shape; Enter keeps an open, stroked line; Esc cancels.";
    case CanvasTool::DIRECT_SELECT: return "Drag the points of the selected vector layer's path.";
    case CanvasTool::EYEDROPPER: return "Click the canvas to take the brush colour.";
    }
    return "";
}

// A colour edited as display (sRGB) values and stored linear.
bool linearColorEdit(const char* label, std::array<float, 4>& linear) {
    float display[4] = {encodeTransfer(linear[0]), encodeTransfer(linear[1]), encodeTransfer(linear[2]), linear[3]};
    if (!ImGui::ColorEdit4(label, display, ImGuiColorEditFlags_AlphaBar)) return false;
    linear = {decodeTransfer(display[0]), decodeTransfer(display[1]), decodeTransfer(display[2]), display[3]};
    return true;
}

bool accentButton(const char* label, bool on, ImVec2 size) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHovered);
    }
    const bool clicked = ImGui::Button(label, size);
    if (on) ImGui::PopStyleColor(2);
    return clicked;
}

// Every vector layer of the document, top first.
void collectPaths(LayerNode& node, std::vector<LayerNode*>& out) {
    for (std::size_t i = node.childCount(); i-- > 0;) {
        LayerNode& child = node.child(i);
        if (child.vectorShape()) out.push_back(&child);
        if (child.isGroup()) collectPaths(child, out);
    }
}

std::size_t indexInParent(const LayerNode& layer) {
    const LayerNode* parent = layer.parent();
    for (std::size_t i = 0; parent && i < parent->childCount(); ++i) {
        if (&parent->child(i) == &layer) return i;
    }
    return 0;
}

// The path's coverage (fill, closing open subpaths) written into `target`'s
// mask: the path decides what shows of the layer.
void pathToMask(const LayerNode& pathLayer, LayerNode& target, std::uint32_t width, std::uint32_t height) {
    VectorContent shape = *pathLayer.vectorShape();
    shape.fillColor = {1.0f, 1.0f, 1.0f, 1.0f};
    shape.strokeColor[3] = 0.0f;
    auto probe = LayerNode::createGroup("Path coverage");
    LayerNode& copy = probe->addChild(LayerNode::createVector("Path", shape));
    copy.setTransform(pathLayer.transform());
    SparseRasterLayer& mask = target.mask() && target.mask()->width() == width && target.mask()->height() == height
                                  ? *target.mask()
                                  : target.addMask(width, height);
    for (std::uint32_t ty = 0; ty < (height + TILE_SIZE - 1) / TILE_SIZE; ++ty) {
        for (std::uint32_t tx = 0; tx < (width + TILE_SIZE - 1) / TILE_SIZE; ++tx) {
            const CompositedTile tile = compositeTileCPU(*probe, {tx, ty}, width, height);
            std::vector<float> coverage(std::size_t{tile.width} * tile.height);
            for (std::size_t i = 0; i < coverage.size(); ++i) coverage[i] = tile.rgba[i * 4 + 3];
            mask.writeRegion(tx * TILE_SIZE, ty * TILE_SIZE, tile.width, tile.height, coverage);
        }
    }
    target.setMaskEnabled(true);
}

}  // namespace

// -----------------------------------------------------------------------------
// Tools
// -----------------------------------------------------------------------------

void ToolsPanel::draw(PanelContext& ctx) {
    CanvasToolState& tools = ctx.canvas.tools;
    LayerNode& root = ctx.app.document();
    ctx.canvas.validate(root);
    LayerNode* layer = ctx.canvas.selectedLayer != &root ? ctx.canvas.selectedLayer : nullptr;

    // Tool buttons, two per row.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float buttonWidth = std::max((ImGui::GetContentRegionAvail().x - spacing) * 0.5f, 40.0f);
    for (std::size_t i = 0; i < kTools.size(); ++i) {
        if (i % 2 == 1) ImGui::SameLine();
        const CanvasTool tool = kTools[i];
        char label[48];
        std::snprintf(label, sizeof label, "%s##tool%zu", toolName(tool), i);
        if (accentButton(label, tools.tool == tool, ImVec2(buttonWidth, 0.0f))) {
            tools.tool = tool;
            ctx.masking.tool = MaskTool::NONE;  // one tool drives the viewport at a time
        }
        ImGui::SetItemTooltip("%s (%s)\n%s", toolName(tool), toolShortcut(tool), toolHint(tool));
    }

    ImGui::SeparatorText(toolName(tools.tool));
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("%s", toolHint(tools.tool));
    ImGui::PopTextWrapPos();

    const float sliderWidth = ImGui::GetContentRegionAvail().x;
    switch (tools.tool) {
    case CanvasTool::BRUSH:
    case CanvasTool::ERASER:
    case CanvasTool::CLONE: {
        compactSlider("##size", tools.size, 1.0f, 1000.0f, 30.0f, "Size %.0f px", sliderWidth, ImGuiSliderFlags_Logarithmic);
        compactSlider("##hardness", tools.hardness, 0.0f, 100.0f, 70.0f, "Hardness %.0f %%", sliderWidth);
        compactSlider("##flow", tools.flow, 1.0f, 100.0f, 100.0f, "Flow %.0f %%", sliderWidth);
        if (tools.tool == CanvasTool::BRUSH) {
            ImGui::SetNextItemWidth(sliderWidth);
            linearColorEdit("##color", tools.color);
        }
        const bool hasMask = layer && layer->mask();
        if (!hasMask) tools.target = PaintTarget::LAYER;
        ImGui::TextDisabled("Paint");
        ImGui::SameLine();
        if (ImGui::RadioButton("Layer", tools.target == PaintTarget::LAYER)) tools.target = PaintTarget::LAYER;
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasMask);
        if (ImGui::RadioButton("Mask", tools.target == PaintTarget::MASK)) tools.target = PaintTarget::MASK;
        ImGui::EndDisabled();
        if (!hasMask) ImGui::SetItemTooltip("The selected layer has no mask (Properties > Add Mask)");
        if (tools.tool == CanvasTool::CLONE) {
            ImGui::Checkbox("Aligned", &tools.cloneAligned);
            ImGui::SetItemTooltip("Keep the source offset from one stroke to the next");
            if (tools.cloneSource) {
                ImGui::TextDisabled("Source %.0f, %.0f", tools.cloneSource->x, tools.cloneSource->y);
            } else {
                ImGui::TextDisabled("No source yet: Alt+click the canvas");
            }
        }
        if (layer) {
            ImGui::TextDisabled("On: %s%s", layer->name().c_str(), tools.target == PaintTarget::MASK ? " (mask)" : "");
        } else {
            ImGui::TextDisabled("Select a layer to paint on");
        }
        break;
    }
    case CanvasTool::MOVE:
        if (layer && layer->contentBounds()) {
            const LayerTransform& t = layer->transform();
            ImGui::TextDisabled("%s: %+.0f, %+.0f px, %.0f %% x %.0f %%, %.1f deg", layer->name().c_str(), t.translateX,
                                t.translateY, t.scaleX * 100.0f, t.scaleY * 100.0f, t.rotation);
            if (ImGui::Button("Reset Transform")) {
                tools.undo.push_back({layer, nullptr, layer->transform(), std::nullopt, "transform"});
                layer->setTransform(LayerTransform{});
            }
        } else {
            ImGui::TextDisabled("Select a pixel, vector or smart-object layer");
        }
        break;
    case CanvasTool::PEN:
        ImGui::SetNextItemWidth(sliderWidth);
        linearColorEdit("##penColor", tools.color);
        compactSlider("##penWidth", tools.penStrokeWidth, 0.5f, 100.0f, 4.0f, "Stroke %.1f px", sliderWidth);
        ImGui::TextDisabled("%zu point%s placed", tools.penPoints.size(), tools.penPoints.size() == 1 ? "" : "s");
        ImGui::BeginDisabled(tools.penPoints.empty());
        if (ImGui::Button("Cancel Path")) tools.penPoints.clear();
        ImGui::EndDisabled();
        break;
    case CanvasTool::EYEDROPPER: {
        float display[4] = {encodeTransfer(tools.color[0]), encodeTransfer(tools.color[1]), encodeTransfer(tools.color[2]), 1.0f};
        ImGui::ColorButton("##picked", ImVec4(display[0], display[1], display[2], 1.0f), ImGuiColorEditFlags_NoTooltip,
                           ImVec2(sliderWidth, ImGui::GetFrameHeight()));
        break;
    }
    case CanvasTool::NONE:
    case CanvasTool::DIRECT_SELECT: break;
    }

    ImGui::Separator();
    ImGui::BeginDisabled(tools.undo.empty());
    char undoLabel[96];
    std::snprintf(undoLabel, sizeof undoLabel, "Undo %s", tools.undo.empty() ? "" : tools.undo.back().label.c_str());
    if (ImGui::Button(undoLabel)) undoCanvasEdit(ctx);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Z (%zu step%s)", tools.undo.size(), tools.undo.size() == 1 ? "" : "s");
    if (!tools.status.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("%s", tools.status.c_str());
        ImGui::PopTextWrapPos();
    }
}

// -----------------------------------------------------------------------------
// Channels
// -----------------------------------------------------------------------------

void ChannelsPanel::draw(PanelContext& ctx) {
    CanvasState& canvas = ctx.canvas;
    LayerNode& root = ctx.app.document();
    canvas.validate(root);
    struct Row {
        const char* name;
        DisplayChannel channel;
        ImU32 color;
    };
    constexpr std::array<Row, 5> kRows{{{"RGB", DisplayChannel::COLOR, IM_COL32(230, 230, 230, 255)},
                                        {"Red", DisplayChannel::RED, IM_COL32(235, 80, 70, 255)},
                                        {"Green", DisplayChannel::GREEN, IM_COL32(90, 200, 90, 255)},
                                        {"Blue", DisplayChannel::BLUE, IM_COL32(80, 140, 240, 255)},
                                        {"Alpha", DisplayChannel::ALPHA, IM_COL32(170, 170, 170, 255)}}};
    for (const Row& row : kRows) {
        const bool selected = !canvas.maskChannel && canvas.channel == row.channel;
        ImGui::PushStyleColor(ImGuiCol_Text, row.color);
        if (ImGui::Selectable(row.name, selected)) {
            canvas.channel = row.channel;
            canvas.maskChannel = nullptr;
        }
        ImGui::PopStyleColor();
        if (row.channel == DisplayChannel::ALPHA) ImGui::SetItemTooltip("The composite's transparency: white is opaque");
    }
    LayerNode* layer = canvas.selectedLayer != &root ? canvas.selectedLayer : nullptr;
    if (layer && layer->mask()) {
        ImGui::Separator();
        const std::string label = layer->name() + " Mask";
        if (ImGui::Selectable(label.c_str(), canvas.maskChannel == layer)) canvas.maskChannel = layer;
        ImGui::SetItemTooltip("Show the layer mask: white reveals the layer, black hides it. The brush and eraser paint it with Paint: Mask.");
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("Channels of the developed canvas, shown as grey. The view returns to RGB outside this workspace.");
    ImGui::PopTextWrapPos();
}

// -----------------------------------------------------------------------------
// Paths
// -----------------------------------------------------------------------------

void PathsPanel::draw(PanelContext& ctx) {
    CanvasState& canvas = ctx.canvas;
    CanvasToolState& tools = canvas.tools;
    LayerNode& root = ctx.app.document();
    canvas.validate(root);

    if (ImGui::Button("New Path")) {
        tools.tool = CanvasTool::PEN;
        ctx.masking.tool = MaskTool::NONE;
    }
    ImGui::SetItemTooltip("Pen tool (P): click corners on the canvas");
    ImGui::SameLine();
    LayerNode* selected = canvas.selectedLayer && canvas.selectedLayer->vectorShape() ? canvas.selectedLayer : nullptr;
    ImGui::BeginDisabled(!selected);
    if (ImGui::Button("Edit Points")) {
        tools.tool = CanvasTool::DIRECT_SELECT;
        ctx.masking.tool = MaskTool::NONE;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && selected) {
        LayerNode* parent = selected->parent();
        parent->removeChild(*selected);
        canvas.selectedLayer = nullptr;
        selected = nullptr;
    }
    ImGui::EndDisabled();

    std::vector<LayerNode*> paths;
    collectPaths(root, paths);
    if (ImGui::BeginChild("##PathList", ImVec2(0.0f, std::max(ImGui::GetFrameHeightWithSpacing() * 4.0f,
                                                              ImGui::GetContentRegionAvail().y * 0.4f)),
                          ImGuiChildFlags_Borders)) {
        if (paths.empty()) ImGui::TextDisabled("No paths yet: draw one with New Path.");
        for (LayerNode* path : paths) {
            const VectorContent& shape = *path->vectorShape();
            std::size_t closed = 0;
            for (PathVerb verb : shape.verbs) closed += verb == PathVerb::CLOSE ? 1 : 0;
            char label[160];
            std::snprintf(label, sizeof label, "%s  (%zu points, %s)###path%p", path->name().c_str(), shape.points.size() / 2,
                          closed ? "closed" : "open", static_cast<const void*>(path));
            if (ImGui::Selectable(label, canvas.selectedLayer == path)) canvas.selectedLayer = path;
        }
    }
    ImGui::EndChild();

    if (!selected) return;
    VectorContent& shape = *selected->vectorShape();
    bool edited = false;
    bool fill = shape.fillColor[3] > 0.0f;
    if (ImGui::Checkbox("Fill", &fill)) {
        shape.fillColor[3] = fill ? 1.0f : 0.0f;
        edited = true;
    }
    ImGui::SameLine();
    edited |= linearColorEdit("##fill", shape.fillColor);
    bool stroke = shape.strokeColor[3] > 0.0f && shape.strokeWidth > 0.0f;
    if (ImGui::Checkbox("Stroke", &stroke)) {
        shape.strokeColor[3] = stroke ? 1.0f : 0.0f;
        if (stroke && shape.strokeWidth <= 0.0f) shape.strokeWidth = 3.0f;
        edited = true;
    }
    ImGui::SameLine();
    edited |= linearColorEdit("##stroke", shape.strokeColor);
    edited |= compactSlider("##width", shape.strokeWidth, 0.0f, 200.0f, 3.0f, "Width %.1f px", ImGui::GetContentRegionAvail().x).changed;
    if (edited) invalidateComposite(root);

    // The layer just below the path gets the path as its mask.
    LayerNode* parent = selected->parent();
    const std::size_t position = indexInParent(*selected);
    LayerNode* below = parent && position > 0 ? &parent->child(position - 1) : nullptr;
    ImGui::BeginDisabled(!below || below->isGroup() || below->adjustment());
    if (ImGui::Button("Mask Layer Below from Path") && below) {
        pathToMask(*selected, *below, ctx.app.canvasWidth(), ctx.app.canvasHeight());
        tools.status = "Masked " + below->name() + " with " + selected->name();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(below ? "Replace %s's mask with this path's shape (hide the path layer to see the result)"
                                : "Needs a pixel, vector or smart-object layer just below the path",
                          below ? below->name().c_str() : "");
}

}  // namespace darkhouse::ui
