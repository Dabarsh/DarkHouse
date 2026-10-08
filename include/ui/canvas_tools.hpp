// DarkHouse — the Canvas workspace's tools (the Tools panel and the viewport).
//
//   Move (V)             drag inside the selected layer to move it, a corner
//                        to scale it (Shift: free aspect), outside to rotate
//   Brush (B)            paint the selected pixel layer or its layer mask
//   Eraser (E)           erase its pixels, or reveal its mask again
//   Clone Stamp (S)      Alt+click a source, then paint copies of it
//   Pen (P)              click corners of a new path; click the first point
//                        to close it (a filled shape), Enter to keep it open
//                        (a stroked line), Esc to cancel
//   Direct Selection (A) drag the anchor and control points of the selected
//                        vector layer's path
//   Eyedropper (I)       click to take the brush colour from the canvas
//
// Ctrl+Z undoes the last stroke, transform or path edit (up to 32).
#pragma once

#include "layer_stack.hpp"
#include "raster_paint.hpp"
#include "render_pipeline.hpp"
#include "ui/panel.hpp"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

namespace darkhouse::ui {

struct CanvasView;  // ui/masking.hpp

enum class CanvasTool : std::uint8_t { NONE, MOVE, BRUSH, ERASER, CLONE, PEN, DIRECT_SELECT, EYEDROPPER };
[[nodiscard]] const char* toolName(CanvasTool tool) noexcept;
[[nodiscard]] const char* toolShortcut(CanvasTool tool) noexcept;

enum class PaintTarget : std::uint8_t { LAYER, MASK };

// One undoable edit of a layer.
struct CanvasUndo {
    LayerNode* layer = nullptr;                     // checked against the document before use
    std::shared_ptr<TileSnapshot> pixels;           // a stroke: the tiles before it
    std::optional<LayerTransform> transform;        // a move / transform drag: the transform before it
    std::optional<VectorContent> path;              // a path edit: the path before it
    std::string label;
};

struct CanvasToolState {
    CanvasTool tool = CanvasTool::NONE;  // NONE: the left button pans the view

    // Brush, eraser and clone stamp.
    float size = 30.0f;  // radius, canvas pixels
    float hardness = 70.0f;  // %
    float flow = 100.0f;     // %
    std::array<float, 4> color{0.9f, 0.9f, 0.9f, 1.0f};  // straight linear RGBA
    PaintTarget target = PaintTarget::LAYER;
    bool cloneAligned = true;
    std::optional<glm::vec2> cloneSource;  // canvas pixels (Alt+click)
    std::optional<glm::vec2> cloneOffset;  // source - destination, set by the first stroke

    // Pen.
    std::vector<glm::vec2> penPoints;  // canvas pixels
    float penStrokeWidth = 4.0f;

    // Interaction in progress.
    bool dragging = false;
    glm::vec2 lastDab{0.0f};
    glm::vec2 dragStart{0.0f};
    int dragHandle = -1;  // move tool: 0-3 corners, 4 inside, 5 outside; direct selection: point index
    LayerTransform dragTransform;

    std::deque<CanvasUndo> undo;
    std::string status;  // last message for the Tools panel
};

// Viewport hooks for the Canvas workspace. Return / behave like the masking
// tools' (ui/masking.hpp): true when the tool used the left button.
bool canvasViewportInput(PanelContext& ctx, const CanvasView& view, bool hovered, bool active);
void drawCanvasToolOverlay(PanelContext& ctx, const CanvasView& view, bool hovered);
// Keyboard: tool shortcuts, [ ] brush size, Ctrl+Z, Enter / Esc for the pen.
void canvasToolShortcuts(PanelContext& ctx);
void undoCanvasEdit(PanelContext& ctx);

}  // namespace darkhouse::ui
