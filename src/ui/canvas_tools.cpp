// Canvas tools: what the viewport does with the mouse and keyboard in the
// Canvas workspace (see ui/canvas_tools.hpp).

#include "ui/canvas_tools.hpp"

#include "ui/canvas_state.hpp"
#include "ui/masking.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace darkhouse::ui {
namespace {

constexpr std::size_t kMaxUndo = 32;
constexpr float kHandleRadius = 8.0f;  // screen pixels

glm::vec2 toCanvas(const CanvasView& view, glm::vec2 screen) { return (screen - view.imageMin) / view.scale; }
glm::vec2 toScreen(const CanvasView& view, glm::vec2 canvas) { return view.imageMin + canvas * view.scale; }
ImVec2 im(glm::vec2 v) { return ImVec2(v.x, v.y); }
glm::vec2 apply(const Affine& m, glm::vec2 p) {
    const std::array<float, 2> q = applyAffine(m, p.x, p.y);
    return {q[0], q[1]};
}

void pushUndo(CanvasToolState& tools, CanvasUndo entry) {
    tools.undo.push_back(std::move(entry));
    while (tools.undo.size() > kMaxUndo) tools.undo.pop_front();
}

// The transform to edit: identity transforms get their pivot at the
// content's centre, so scaling and rotating happen about the middle.
LayerTransform editableTransform(const LayerNode& layer) {
    LayerTransform t = layer.transform();
    if (t.isIdentity()) {
        if (const std::optional<std::array<float, 4>> b = layer.contentBounds()) {
            t.pivotX = 0.5f * ((*b)[0] + (*b)[2]);
            t.pivotY = 0.5f * ((*b)[1] + (*b)[3]);
        }
    }
    return t;
}

// The canvas corners of a layer's transformed content, in order.
std::optional<std::array<glm::vec2, 4>> layerCorners(const LayerNode& layer) {
    const std::optional<std::array<float, 4>> b = layer.contentBounds();
    if (!b) return std::nullopt;
    const Affine m = layer.transform().matrix();
    return std::array<glm::vec2, 4>{apply(m, {(*b)[0], (*b)[1]}), apply(m, {(*b)[2], (*b)[1]}), apply(m, {(*b)[2], (*b)[3]}),
                                    apply(m, {(*b)[0], (*b)[3]})};
}

bool insideQuad(const std::array<glm::vec2, 4>& q, glm::vec2 p) {
    bool inside = false;
    for (std::size_t i = 0, j = 3; i < 4; j = i++) {
        if ((q[i].y > p.y) != (q[j].y > p.y) && p.x < (q[j].x - q[i].x) * (p.y - q[i].y) / (q[j].y - q[i].y) + q[i].x) inside = !inside;
    }
    return inside;
}

// What the brush paints: the layer's pixels (in layer coordinates) or its
// mask (in canvas coordinates).
struct PaintSurface {
    SparseRasterLayer* raster = nullptr;
    Affine toSurface = kIdentityAffine;  // canvas -> surface
    float scale = 1.0f;                  // surface pixels per canvas pixel
};

PaintSurface paintSurface(LayerNode& layer, PaintTarget target) {
    PaintSurface surface;
    if (target == PaintTarget::MASK) {
        surface.raster = layer.mask();
        return surface;
    }
    surface.raster = layer.raster();
    if (!layer.transform().isIdentity()) {
        const Affine m = layer.transform().matrix();
        if (const std::optional<Affine> inverse = invertAffine(m)) {
            surface.toSurface = *inverse;
            surface.scale = 1.0f / std::sqrt(std::fabs(m[0] * m[3] - m[1] * m[2]));
        } else {
            surface.raster = nullptr;
        }
    }
    return surface;
}

void setStatus(CanvasToolState& tools, const char* message) { tools.status = message; }

// Dabs from the last one to `to`, a quarter of the radius apart.
template <class Dab>
void strokeTo(CanvasToolState& tools, glm::vec2 to, Dab&& dab) {
    const float spacing = std::max(1.0f, tools.size * 0.25f);
    glm::vec2 delta = to - tools.lastDab;
    float distance = glm::length(delta);
    while (distance >= spacing) {
        tools.lastDab += delta / distance * spacing;
        dab(tools.lastDab);
        delta = to - tools.lastDab;
        distance = glm::length(delta);
    }
}

void finishPath(PanelContext& ctx, bool closed) {
    CanvasToolState& tools = ctx.canvas.tools;
    if (tools.penPoints.size() < (closed ? 3u : 2u)) {
        tools.penPoints.clear();
        return;
    }
    VectorContent shape;
    for (std::size_t i = 0; i < tools.penPoints.size(); ++i) {
        shape.verbs.push_back(i == 0 ? PathVerb::MOVE_TO : PathVerb::LINE_TO);
        shape.points.push_back(tools.penPoints[i].x);
        shape.points.push_back(tools.penPoints[i].y);
    }
    if (closed) {
        shape.verbs.push_back(PathVerb::CLOSE);
        shape.fillColor = tools.color;
        shape.strokeColor = {0.0f, 0.0f, 0.0f, 0.0f};
    } else {
        shape.fillColor = {0.0f, 0.0f, 0.0f, 0.0f};
        shape.strokeColor = tools.color;
        shape.strokeWidth = tools.penStrokeWidth;
    }
    static int pathCounter = 0;
    ctx.canvas.insertLayer(ctx.app.document(),
                           LayerNode::createVector("Path " + std::to_string(++pathCounter), std::move(shape)));
    tools.penPoints.clear();
    setStatus(tools, closed ? "Closed path added as a filled shape" : "Open path added as a stroked line");
}

}  // namespace

const char* toolName(CanvasTool tool) noexcept {
    switch (tool) {
    case CanvasTool::NONE: return "Hand";
    case CanvasTool::MOVE: return "Move";
    case CanvasTool::BRUSH: return "Brush";
    case CanvasTool::ERASER: return "Eraser";
    case CanvasTool::CLONE: return "Clone Stamp";
    case CanvasTool::PEN: return "Pen";
    case CanvasTool::DIRECT_SELECT: return "Direct Selection";
    case CanvasTool::EYEDROPPER: return "Eyedropper";
    }
    return "?";
}

const char* toolShortcut(CanvasTool tool) noexcept {
    switch (tool) {
    case CanvasTool::NONE: return "H";
    case CanvasTool::MOVE: return "V";
    case CanvasTool::BRUSH: return "B";
    case CanvasTool::ERASER: return "E";
    case CanvasTool::CLONE: return "S";
    case CanvasTool::PEN: return "P";
    case CanvasTool::DIRECT_SELECT: return "A";
    case CanvasTool::EYEDROPPER: return "I";
    }
    return "";
}

void undoCanvasEdit(PanelContext& ctx) {
    CanvasToolState& tools = ctx.canvas.tools;
    LayerNode& root = ctx.app.document();
    while (!tools.undo.empty()) {
        CanvasUndo entry = std::move(tools.undo.back());
        tools.undo.pop_back();
        if (!containsLayer(root, entry.layer)) continue;  // the layer is gone: nothing to undo there
        LayerNode* layer = entry.layer;
        if (entry.pixels) {
            // Only if the painted raster is still the layer's (a mask may have been replaced).
            if (entry.pixels->layer() != layer->raster() && entry.pixels->layer() != layer->mask()) continue;
            entry.pixels->restore();
        }
        if (entry.transform) layer->setTransform(*entry.transform);
        if (entry.path && layer->vectorShape()) {
            *layer->vectorShape() = *entry.path;
            layer->markCompositeDirty();
        }
        tools.status = "Undid " + entry.label;
        return;
    }
    tools.status = "Nothing to undo";
}

bool canvasViewportInput(PanelContext& ctx, const CanvasView& view, bool hovered, bool active) {
    CanvasToolState& tools = ctx.canvas.tools;
    LayerNode& root = ctx.app.document();
    ctx.canvas.validate(root);
    if (tools.tool == CanvasTool::NONE) return false;
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse = io.MousePos;
    const glm::vec2 point = toCanvas(view, mouse);
    const bool pressed = active && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool down = active && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    LayerNode* layer = ctx.canvas.selectedLayer != &root ? ctx.canvas.selectedLayer : nullptr;

    if (!down && tools.dragging) {
        tools.dragging = false;
        tools.dragHandle = -1;
        return true;
    }

    switch (tools.tool) {
    case CanvasTool::MOVE: {
        if (!layer || !layer->contentBounds()) {
            if (pressed) setStatus(tools, "Select a pixel, vector or smart-object layer to move it");
            break;
        }
        const std::optional<std::array<glm::vec2, 4>> corners = layerCorners(*layer);
        if (pressed && corners) {
            tools.dragging = true;
            tools.dragStart = point;
            tools.dragTransform = editableTransform(*layer);
            tools.dragHandle = insideQuad(*corners, point) ? 4 : 5;
            for (int i = 0; i < 4; ++i) {
                if (glm::length(toScreen(view, (*corners)[static_cast<std::size_t>(i)]) - mouse) < kHandleRadius) tools.dragHandle = i;
            }
            pushUndo(tools, {layer, nullptr, layer->transform(), std::nullopt, "transform"});
        } else if (down && tools.dragging) {
            LayerTransform t = tools.dragTransform;
            const glm::vec2 pivot = apply(tools.dragTransform.matrix(), {t.pivotX, t.pivotY});
            if (tools.dragHandle == 4) {
                t.translateX += point.x - tools.dragStart.x;
                t.translateY += point.y - tools.dragStart.y;
            } else if (tools.dragHandle >= 0 && tools.dragHandle < 4) {
                const float before = glm::length(tools.dragStart - pivot), after = glm::length(point - pivot);
                if (before > 1e-3f && after > 1e-3f) {
                    const float factor = after / before;
                    t.scaleX *= factor;
                    t.scaleY *= factor;
                }
            } else {
                const glm::vec2 a = tools.dragStart - pivot, b = point - pivot;
                float degrees = t.rotation + (std::atan2(b.y, b.x) - std::atan2(a.y, a.x)) * 180.0f / 3.14159265f;
                if (io.KeyShift) degrees = std::round(degrees / 15.0f) * 15.0f;  // Shift: 15 degree steps
                t.rotation = std::remainder(degrees, 360.0f);
            }
            layer->setTransform(t);
        }
        return hovered;
    }
    case CanvasTool::BRUSH:
    case CanvasTool::ERASER:
    case CanvasTool::CLONE: {
        if (tools.tool == CanvasTool::CLONE && pressed && io.KeyAlt) {
            tools.cloneSource = point;
            tools.cloneOffset.reset();
            setStatus(tools, "Clone source set; paint to copy it");
            return true;
        }
        if (!layer) {
            if (pressed) setStatus(tools, "Select a pixel layer to paint on");
            return hovered;
        }
        const PaintSurface surface = paintSurface(*layer, tools.target);
        if (!surface.raster) {
            if (pressed) {
                setStatus(tools, tools.target == PaintTarget::MASK ? "The selected layer has no mask: add one in Properties"
                                                                   : "The selected layer has no pixels to paint (pick a pixel layer, or paint its mask)");
            }
            return hovered;
        }
        if (tools.tool == CanvasTool::CLONE && !tools.cloneSource) {
            if (pressed) setStatus(tools, "Alt+click to set the clone source first");
            return hovered;
        }
        const BrushTip tip{tools.size * surface.scale, tools.hardness / 100.0f, tools.flow / 100.0f};
        std::shared_ptr<TileSnapshot> snapshot;
        auto dab = [&](glm::vec2 at) {
            const glm::vec2 p = apply(surface.toSurface, at);
            snapshot->capture(dabTiles(*surface.raster, p.x, p.y, tip.radius));
            if (tools.tool == CanvasTool::CLONE) {
                const glm::vec2 source = apply(surface.toSurface, at + *tools.cloneOffset);
                cloneDab(*surface.raster, *surface.raster, p.x, p.y, source.x - p.x, source.y - p.y, tip);
            } else {
                paintDab(*surface.raster, p.x, p.y, tip, tools.color, tools.tool == CanvasTool::ERASER ? DabMode::ERASE : DabMode::PAINT);
            }
        };
        if (pressed) {
            tools.dragging = true;
            tools.lastDab = point;
            if (tools.tool == CanvasTool::CLONE && (!tools.cloneAligned || !tools.cloneOffset)) {
                tools.cloneOffset = *tools.cloneSource - point;
            }
            pushUndo(tools, {layer, std::make_shared<TileSnapshot>(*surface.raster), std::nullopt, std::nullopt,
                             tools.tool == CanvasTool::BRUSH ? "brush stroke" : tools.tool == CanvasTool::ERASER ? "eraser stroke" : "clone stroke"});
            snapshot = tools.undo.back().pixels;
            dab(point);
        } else if (down && tools.dragging && !tools.undo.empty() && tools.undo.back().pixels) {
            snapshot = tools.undo.back().pixels;
            strokeTo(tools, point, dab);
        }
        return hovered;
    }
    case CanvasTool::EYEDROPPER:
        if (pressed && point.x >= 0.0f && point.y >= 0.0f && point.x < static_cast<float>(ctx.app.canvasWidth()) &&
            point.y < static_cast<float>(ctx.app.canvasHeight())) {
            const auto x = static_cast<std::uint32_t>(point.x), y = static_cast<std::uint32_t>(point.y);
            const CompositedTile tile = compositeTileCPU(root, SparseRasterLayer::tileKeyFor(x, y), ctx.app.canvasWidth(),
                                                         ctx.app.canvasHeight());
            const float* p = &tile.rgba[(std::size_t{y % TILE_SIZE} * tile.width + x % TILE_SIZE) * 4];
            tools.color = {p[0], p[1], p[2], 1.0f};
            char message[96];
            std::snprintf(message, sizeof message, "Picked %.2f %.2f %.2f (linear) at %u, %u", p[0], p[1], p[2], x, y);
            tools.status = message;
        }
        return hovered;
    case CanvasTool::PEN:
        if (pressed) {
            if (tools.penPoints.size() >= 3 && glm::length(toScreen(view, tools.penPoints.front()) - mouse) < kHandleRadius) {
                finishPath(ctx, true);
            } else {
                tools.penPoints.push_back(point);
                setStatus(tools, "Click to add corners; the first point closes; Enter keeps it open; Esc cancels");
            }
        }
        return hovered;
    case CanvasTool::DIRECT_SELECT: {
        VectorContent* shape = layer ? layer->vectorShape() : nullptr;
        if (!shape) {
            if (pressed) setStatus(tools, "Select a vector layer (Paths or Layers panel) to edit its points");
            return hovered;
        }
        const Affine m = layer->transform().matrix();
        if (pressed) {
            tools.dragHandle = -1;
            float best = kHandleRadius;
            for (std::size_t i = 0; i + 1 < shape->points.size(); i += 2) {
                const float d = glm::length(toScreen(view, apply(m, {shape->points[i], shape->points[i + 1]})) - mouse);
                if (d < best) {
                    best = d;
                    tools.dragHandle = static_cast<int>(i / 2);
                }
            }
            if (tools.dragHandle >= 0) {
                tools.dragging = true;
                pushUndo(tools, {layer, nullptr, std::nullopt, *shape, "path edit"});
            }
        } else if (down && tools.dragging && tools.dragHandle >= 0) {
            if (const std::optional<Affine> inverse = invertAffine(m)) {
                const glm::vec2 p = apply(*inverse, point);
                const auto i = static_cast<std::size_t>(tools.dragHandle) * 2;
                if (i + 1 < shape->points.size()) {
                    shape->points[i] = p.x;
                    shape->points[i + 1] = p.y;
                    layer->markCompositeDirty();
                }
            }
        }
        return hovered;
    }
    case CanvasTool::NONE: break;
    }
    return hovered;
}

void drawCanvasToolOverlay(PanelContext& ctx, const CanvasView& view, bool hovered) {
    CanvasToolState& tools = ctx.canvas.tools;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const glm::vec2 mouse = ImGui::GetIO().MousePos;
    LayerNode* layer = ctx.canvas.selectedLayer;

    // The selected vector layer's path, with its points, for the path tools.
    if (layer && layer->vectorShape() && (tools.tool == CanvasTool::DIRECT_SELECT || tools.tool == CanvasTool::PEN)) {
        const VectorContent& shape = *layer->vectorShape();
        const Affine m = layer->transform().matrix();
        for (const Polyline& line : flattenPath(shape, m)) {
            std::vector<ImVec2> points;
            for (const auto& p : line.points) points.push_back(im(toScreen(view, {p[0], p[1]})));
            drawList->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(90, 170, 255, 230), 1.5f,
                                  line.closed ? ImDrawFlags_Closed : ImDrawFlags_None);
        }
        for (std::size_t i = 0; i + 1 < shape.points.size(); i += 2) {
            const glm::vec2 p = toScreen(view, apply(m, {shape.points[i], shape.points[i + 1]}));
            const bool hot = static_cast<int>(i / 2) == tools.dragHandle && tools.dragging;
            drawList->AddRectFilled(im(p - glm::vec2(3.5f)), im(p + glm::vec2(3.5f)), hot ? IM_COL32(90, 170, 255, 255) : IM_COL32(255, 255, 255, 240));
            drawList->AddRect(im(p - glm::vec2(3.5f)), im(p + glm::vec2(3.5f)), IM_COL32(20, 60, 120, 255));
        }
    }

    switch (tools.tool) {
    case CanvasTool::BRUSH:
    case CanvasTool::ERASER:
    case CanvasTool::CLONE:
        if (hovered) {
            const float radius = tools.size * view.scale;
            drawList->AddCircle(im(mouse), radius, IM_COL32(0, 0, 0, 160), 48, 3.0f);
            drawList->AddCircle(im(mouse), radius, tools.tool == CanvasTool::ERASER ? IM_COL32(255, 140, 140, 230) : IM_COL32(255, 255, 255, 230),
                                48, 1.5f);
            if (tools.hardness < 99.0f) {
                drawList->AddCircle(im(mouse), radius * tools.hardness / 100.0f, IM_COL32(255, 255, 255, 90), 48, 1.0f);
            }
        }
        if (tools.tool == CanvasTool::CLONE) {
            // Where the stamp copies from: the source, or the mouse plus the offset.
            std::optional<glm::vec2> source;
            if (tools.cloneOffset && hovered) source = toCanvas(view, mouse) + *tools.cloneOffset;
            else if (tools.cloneSource) source = tools.cloneSource;
            if (source) {
                const glm::vec2 s = toScreen(view, *source);
                for (const auto& [a, b] : {std::pair{glm::vec2(-8, 0), glm::vec2(8, 0)}, std::pair{glm::vec2(0, -8), glm::vec2(0, 8)}}) {
                    drawList->AddLine(im(s + a), im(s + b), IM_COL32(0, 0, 0, 180), 3.0f);
                    drawList->AddLine(im(s + a), im(s + b), IM_COL32(255, 255, 255, 240), 1.5f);
                }
            }
        }
        break;
    case CanvasTool::PEN:
        if (!tools.penPoints.empty()) {
            std::vector<ImVec2> points;
            for (const glm::vec2& p : tools.penPoints) points.push_back(im(toScreen(view, p)));
            if (hovered) points.push_back(im(mouse));
            drawList->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(0, 0, 0, 160), 3.0f, ImDrawFlags_None);
            drawList->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(90, 170, 255, 255), 1.5f, ImDrawFlags_None);
            for (std::size_t i = 0; i < tools.penPoints.size(); ++i) {
                const glm::vec2 p = toScreen(view, tools.penPoints[i]);
                const bool closes = i == 0 && tools.penPoints.size() >= 3 && glm::length(p - mouse) < kHandleRadius;
                drawList->AddRectFilled(im(p - glm::vec2(3.5f)), im(p + glm::vec2(3.5f)), closes ? IM_COL32(90, 170, 255, 255) : IM_COL32(255, 255, 255, 240));
            }
        }
        break;
    case CanvasTool::EYEDROPPER:
        if (hovered) {
            drawList->AddCircle(im(mouse), 7.0f, IM_COL32(0, 0, 0, 180), 16, 3.0f);
            drawList->AddCircle(im(mouse), 7.0f, IM_COL32(255, 255, 255, 240), 16, 1.5f);
        }
        break;
    case CanvasTool::NONE:
    case CanvasTool::MOVE:
    case CanvasTool::DIRECT_SELECT: break;
    }
}

void canvasToolShortcuts(PanelContext& ctx) {
    if (ctx.frame.mode != AppMode::CANVAS) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    CanvasToolState& tools = ctx.canvas.tools;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) undoCanvasEdit(ctx);
    if (io.KeyCtrl || io.KeyAlt || io.KeySuper) return;
    constexpr std::array<std::pair<ImGuiKey, CanvasTool>, 8> kKeys{{{ImGuiKey_H, CanvasTool::NONE},
                                                                    {ImGuiKey_V, CanvasTool::MOVE},
                                                                    {ImGuiKey_B, CanvasTool::BRUSH},
                                                                    {ImGuiKey_E, CanvasTool::ERASER},
                                                                    {ImGuiKey_S, CanvasTool::CLONE},
                                                                    {ImGuiKey_P, CanvasTool::PEN},
                                                                    {ImGuiKey_A, CanvasTool::DIRECT_SELECT},
                                                                    {ImGuiKey_I, CanvasTool::EYEDROPPER}}};
    for (const auto& [key, tool] : kKeys) {
        if (ImGui::IsKeyPressed(key, false)) {
            tools.tool = tool;
            ctx.masking.tool = MaskTool::NONE;  // one tool drives the viewport at a time
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) tools.size = std::max(1.0f, tools.size / 1.2f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) tools.size = std::min(1000.0f, tools.size * 1.2f);
    if (tools.tool == CanvasTool::PEN && !tools.penPoints.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) finishPath(ctx, false);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            tools.penPoints.clear();
            setStatus(tools, "Path cancelled");
        }
    }
}

}  // namespace darkhouse::ui
