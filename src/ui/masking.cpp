// Masking tools: the binding to the local_adjust node, and what the viewport
// does for them (brush strokes, gradient drags, colour picking, guides).

#include "ui/masking.hpp"

#include "local_adjust_node.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace darkhouse::ui {
namespace {

constexpr const char* kNodeType = "local_adjust";

glm::vec2 toScreen(const CanvasView& view, glm::vec2 canvasPixels) { return view.imageMin + canvasPixels * view.scale; }
glm::vec2 toCanvas(const CanvasView& view, glm::vec2 screen) { return (screen - view.imageMin) / view.scale; }
ImVec2 im(glm::vec2 v) { return ImVec2(v.x, v.y); }

// Outline with a dark halo, readable on any photo.
void guideLine(ImDrawList* drawList, glm::vec2 a, glm::vec2 b, float thickness = 1.5f) {
    drawList->AddLine(im(a), im(b), IM_COL32(0, 0, 0, 160), thickness + 2.0f);
    drawList->AddLine(im(a), im(b), IM_COL32(255, 255, 255, 230), thickness);
}

void guideHandle(ImDrawList* drawList, glm::vec2 p) {
    drawList->AddCircleFilled(im(p), 5.0f, IM_COL32(0, 0, 0, 160));
    drawList->AddCircleFilled(im(p), 3.5f, IM_COL32(255, 255, 255, 240));
}

void guideEllipse(ImDrawList* drawList, glm::vec2 centre, glm::vec2 radii, float angleDegrees) {
    constexpr int kSegments = 64;
    const float angle = angleDegrees * 3.14159265f / 180.0f;
    const float cs = std::cos(angle), sn = std::sin(angle);
    ImVec2 points[kSegments];
    for (int i = 0; i < kSegments; ++i) {
        const float t = static_cast<float>(i) / kSegments * 6.2831853f;
        const glm::vec2 local(radii.x * std::cos(t), radii.y * std::sin(t));
        // Inverse of the shader's rotation: canvas = R^T * local.
        points[i] = im(centre + glm::vec2(cs * local.x - sn * local.y, sn * local.x + cs * local.y));
    }
    drawList->AddPolyline(points, kSegments, IM_COL32(0, 0, 0, 160), 3.5f, ImDrawFlags_Closed);
    drawList->AddPolyline(points, kSegments, IM_COL32(255, 255, 255, 230), 1.5f, ImDrawFlags_Closed);
}

// The bottom-most raster layer: the photo, for picking colours.
const SparseRasterLayer* photoLayer(DarkHouseApp& app) {
    LayerNode& document = app.document();
    for (std::size_t i = 0; i < document.childCount(); ++i) {
        if (const SparseRasterLayer* raster = document.child(i).raster()) return raster;
    }
    return nullptr;
}

}  // namespace

// -----------------------------------------------------------------------------
// MaskStackBinding / MaskingState
// -----------------------------------------------------------------------------

void MaskStackBinding::sync(const DarkHouseApp& app, std::uint64_t frame) {
    if (frame == syncedFrame_) return;
    syncedFrame_ = frame;
    if (!ImGui::IsAnyItemActive()) editing_ = false;  // a drag ended without a final change
    const std::vector<EditNodeRecord>& stack = app.developStack();
    const std::optional<std::size_t> found = findDevelopNode(stack, kNodeType);
    index_.reset();
    if (found) index_ = static_cast<std::uint32_t>(*found);
    if (editing_) return;
    values_ = LocalAdjustments{};
    if (!found) return;
    try {
        values_ = deserializeLocalAdjustments(stack[*found].serializedParams);
    } catch (const std::exception&) {
        values_ = LocalAdjustments{};  // unreadable masks: start over rather than fail every frame
    }
}

void MaskStackBinding::commit(PanelContext& ctx, bool editing) {
    editing_ = editing;
    std::vector<std::byte> bytes = serialize(values_);
    if (index_) {
        if (values_.masks.empty() && !editing) {
            // No masks left: take the node out rather than run an empty pass.
            std::vector<EditNodeRecord> stack = ctx.app.developStack();
            stack.erase(stack.begin() + *index_);
            ctx.app.postEvent(SetDevelopStackEvent{std::move(stack), true});
            return;
        }
        ctx.app.postEvent(SetDevelopParamsEvent{*index_, std::move(bytes), !editing, kNodeType});
    } else if (!values_.masks.empty()) {
        ctx.app.postEvent(SetDevelopStackEvent{
            withDevelopNode(ctx.app.developStack(), EditNodeRecord{0, kNodeType, std::move(bytes)}), true});
    }
}

LocalAdjustment* MaskingState::mask() noexcept {
    std::vector<LocalAdjustment>& masksList = masks.values().masks;
    return selectedMask >= 0 && selectedMask < static_cast<int>(masksList.size()) ? &masksList[static_cast<std::size_t>(selectedMask)]
                                                                                  : nullptr;
}

MaskComponent* MaskingState::component() noexcept {
    LocalAdjustment* current = mask();
    if (!current || selectedComponent < 0 || selectedComponent >= static_cast<int>(current->components.size())) {
        return nullptr;
    }
    return &current->components[static_cast<std::size_t>(selectedComponent)];
}

void MaskingState::clampSelection() noexcept {
    const int maskCount = static_cast<int>(masks.values().masks.size());
    if (selectedMask >= maskCount) selectedMask = maskCount - 1;
    if (selectedMask < 0 && maskCount > 0) selectedMask = 0;
    const LocalAdjustment* current = mask();
    const int componentCount = current ? static_cast<int>(current->components.size()) : 0;
    if (selectedComponent >= componentCount) selectedComponent = componentCount - 1;
    if (selectedComponent < 0 && componentCount > 0) selectedComponent = 0;

    // A tool only makes sense on a component of its kind.
    const MaskComponent* c = component();
    const MaskShape wanted = tool == MaskTool::BRUSH         ? MaskShape::BRUSH
                             : tool == MaskTool::DRAW_LINEAR ? MaskShape::LINEAR_GRADIENT
                             : tool == MaskTool::DRAW_RADIAL ? MaskShape::RADIAL_GRADIENT
                                                             : MaskShape::COLOR_RANGE;
    if (tool != MaskTool::NONE && (!c || c->shape != wanted)) {
        tool = MaskTool::NONE;
        stroking = false;
    }
}

// -----------------------------------------------------------------------------
// Viewport
// -----------------------------------------------------------------------------

bool maskingViewportInput(PanelContext& ctx, const CanvasView& view, bool hovered, bool active) {
    MaskingState& m = ctx.masking;
    m.masks.sync(ctx.app, ctx.frame.frameIndex);
    m.clampSelection();
    MaskComponent* c = m.component();
    if (m.tool == MaskTool::NONE || !c) return false;

    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 canvas = toCanvas(view, io.MousePos);
    const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const float longEdge = std::max(view.canvasSize.x, view.canvasSize.y);

    if (active && leftDown) {
        switch (m.tool) {
        case MaskTool::BRUSH: {
            auto addDab = [&](glm::vec2 at) {
                c->dabs.push_back({at.x / view.canvasSize.x, at.y / view.canvasSize.y, m.brushSize / longEdge,
                                   m.brushFeather / 100.0f, m.brushFlow / 100.0f, m.brushErase || io.KeyAlt});
            };
            const std::size_t before = c->dabs.size();
            if (!m.stroking) {
                m.stroking = true;
                m.lastDab = canvas;
                addDab(canvas);
            } else {
                // Evenly spaced dabs along the mouse path, a quarter radius apart.
                const float spacing = std::max(1.0f, m.brushSize * 0.25f);
                glm::vec2 delta = canvas - m.lastDab;
                float distance = glm::length(delta);
                while (distance >= spacing) {
                    m.lastDab += delta / glm::length(delta) * spacing;
                    addDab(m.lastDab);
                    delta = canvas - m.lastDab;
                    distance = glm::length(delta);
                }
            }
            if (c->dabs.size() != before) m.masks.commit(ctx, true);
            break;
        }
        case MaskTool::DRAW_LINEAR:
            if (!m.stroking) {
                m.stroking = true;
                m.dragStart = canvas;
            }
            c->start = {m.dragStart.x / view.canvasSize.x, m.dragStart.y / view.canvasSize.y};
            c->end = {canvas.x / view.canvasSize.x, canvas.y / view.canvasSize.y};
            m.masks.commit(ctx, true);
            break;
        case MaskTool::DRAW_RADIAL: {
            if (!m.stroking) {
                m.stroking = true;
                m.dragStart = canvas;
            }
            glm::vec2 radii = glm::max(glm::abs(canvas - m.dragStart), glm::vec2(2.0f));
            if (io.KeyShift) radii = glm::vec2(std::max(radii.x, radii.y));
            c->start = {m.dragStart.x / view.canvasSize.x, m.dragStart.y / view.canvasSize.y};
            c->size = {radii.x / view.canvasSize.x, radii.y / view.canvasSize.y};
            c->angle = 0.0f;
            m.masks.commit(ctx, true);
            break;
        }
        case MaskTool::PICK_COLOR:
            if (!m.stroking) {
                m.stroking = true;
                if (const SparseRasterLayer* photo = photoLayer(ctx.app);
                    photo && canvas.x >= 0.0f && canvas.y >= 0.0f && canvas.x < photo->width() && canvas.y < photo->height()) {
                    float rgba[4];
                    photo->readPixel(static_cast<std::uint32_t>(canvas.x), static_cast<std::uint32_t>(canvas.y), rgba);
                    const Rgb lab = linearSrgbToOklab({std::max(rgba[0], 0.0f), std::max(rgba[1], 0.0f), std::max(rgba[2], 0.0f)});
                    const float chroma = std::hypot(lab[1], lab[2]);
                    if (chroma > 0.0f) {
                        float hue = std::atan2(lab[2], lab[1]) * 180.0f / 3.14159265f;
                        c->hue = hue < 0.0f ? hue + 360.0f : hue;
                        c->low = std::clamp(chroma * 0.5f, 0.0f, 0.2f);  // keep colours at least half as vivid
                        m.masks.commit(ctx, false);
                    }
                }
            }
            break;
        case MaskTool::NONE: break;
        }
        return true;
    }
    if (m.stroking && !leftDown) {
        m.stroking = false;
        if (m.tool == MaskTool::PICK_COLOR) {
            m.tool = MaskTool::NONE;  // one pick per click of the eyedropper
        } else {
            m.masks.commit(ctx, false);  // the stroke or drag is done: save it
        }
        return true;
    }
    return hovered;  // with a tool active, the left button never pans
}

void drawMaskingOverlay(PanelContext& ctx, const CanvasView& view, bool hovered) {
    MaskingState& m = ctx.masking;
    const MaskComponent* c = m.component();
    if (m.tool == MaskTool::NONE || !c) return;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const glm::vec2 size = view.canvasSize;

    switch (m.tool) {
    case MaskTool::BRUSH:
        if (hovered) {
            const glm::vec2 mouse = ImGui::GetIO().MousePos;
            const float radius = m.brushSize * view.scale;
            const bool erase = m.brushErase || ImGui::GetIO().KeyAlt;
            drawList->AddCircle(im(mouse), radius, IM_COL32(0, 0, 0, 160), 48, 3.0f);
            drawList->AddCircle(im(mouse), radius, erase ? IM_COL32(255, 120, 120, 230) : IM_COL32(255, 255, 255, 230), 48, 1.5f);
            if (m.brushFeather > 1.0f) {
                drawList->AddCircle(im(mouse), radius * (1.0f - m.brushFeather / 100.0f), IM_COL32(255, 255, 255, 110), 48, 1.0f);
            }
            if (erase) drawList->AddLine(im(mouse - glm::vec2(4.0f, 0.0f)), im(mouse + glm::vec2(4.0f, 0.0f)), IM_COL32(255, 120, 120, 255), 2.0f);
        }
        break;
    case MaskTool::DRAW_LINEAR: {
        // Full effect beyond the start line, none beyond the end line.
        const glm::vec2 a = toScreen(view, glm::vec2(c->start[0], c->start[1]) * size);
        const glm::vec2 b = toScreen(view, glm::vec2(c->end[0], c->end[1]) * size);
        const glm::vec2 along = b - a;
        const float length = glm::length(along);
        if (length > 1.0f) {
            const glm::vec2 across = glm::vec2(-along.y, along.x) / length * std::max(view.canvasSize.x, view.canvasSize.y) * view.scale;
            guideLine(drawList, a - across, a + across);
            guideLine(drawList, b - across, b + across, 1.0f);
            guideLine(drawList, a, b, 1.0f);
        }
        guideHandle(drawList, a);
        guideHandle(drawList, b);
        break;
    }
    case MaskTool::DRAW_RADIAL: {
        const glm::vec2 centre = toScreen(view, glm::vec2(c->start[0], c->start[1]) * size);
        const glm::vec2 radii = glm::vec2(c->size[0], c->size[1]) * size * view.scale;
        guideEllipse(drawList, centre, radii, c->angle);
        guideEllipse(drawList, centre, radii * (1.0f - c->feather), c->angle);
        guideHandle(drawList, centre);
        break;
    }
    case MaskTool::PICK_COLOR:
        if (hovered) {
            const glm::vec2 mouse = ImGui::GetIO().MousePos;
            guideLine(drawList, mouse - glm::vec2(9.0f, 0.0f), mouse + glm::vec2(9.0f, 0.0f));
            guideLine(drawList, mouse - glm::vec2(0.0f, 9.0f), mouse + glm::vec2(0.0f, 9.0f));
        }
        break;
    case MaskTool::NONE: break;
    }
}

void syncMaskOverlay(PanelContext& ctx) {
    MaskingState& m = ctx.masking;
    if (!m.panelVisible) {
        // The panel is closed or behind another tab: its tools and overlay go with it.
        m.tool = MaskTool::NONE;
        m.stroking = false;
        m.visibleFrames = 0;
    } else if (m.visibleFrames < 2) {
        ++m.visibleFrames;
    }
    // Two frames in a row, so a tab that shows for a single frame while a dock
    // is rebuilt does not flash the overlay (and re-render the photo twice).
    const int wanted = m.visibleFrames >= 2 && m.showOverlay && m.mask() ? m.selectedMask : -1;
    if (wanted != m.overlaySent) {
        ctx.app.postEvent(SetMaskOverlayEvent{wanted});
        m.overlaySent = wanted;
    }
}

}  // namespace darkhouse::ui
