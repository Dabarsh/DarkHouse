// DarkHouse — masking tools shared by the Masking panel and the viewport.
//
// The Masking panel edits the photo's masks (the local_adjust develop node);
// the viewport paints brush strokes and drags gradients for the component
// selected there. Both go through one MaskingState, owned by the shell, so a
// stroke painted on the canvas and a slider moved in the panel edit the same
// data and are sent the same way: live while dragging, saved on release.
#pragma once

#include "develop_stack.hpp"
#include "mask_engine.hpp"
#include "ui/panel.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace darkhouse::ui {

enum class MaskTool : std::uint8_t {
    NONE,         // the viewport pans and zooms
    BRUSH,        // left-drag paints into the selected brush component (Alt erases)
    DRAW_LINEAR,  // left-drag sets the selected linear gradient's start and end
    DRAW_RADIAL,  // left-drag sets the selected radial gradient's centre and radii (Shift: circle)
    PICK_COLOR,   // a click picks the selected colour range's hue from the photo
};

// The masks of the open photo, bound to its local_adjust node.
class MaskStackBinding {
public:
    // Once per frame (later calls in the same frame are ignored): loads the
    // node's masks unless an edit is in progress.
    void sync(const DarkHouseApp& app, std::uint64_t frame);

    [[nodiscard]] LocalAdjustments& values() noexcept { return values_; }
    [[nodiscard]] bool present() const noexcept { return index_.has_value(); }

    // Sends the current values: live while `editing`, saved when it ends.
    // The first edit inserts the local_adjust node into the develop stack.
    void commit(PanelContext& ctx, bool editing);

private:
    LocalAdjustments values_;
    std::optional<std::uint32_t> index_;
    std::uint64_t syncedFrame_ = ~std::uint64_t{0};
    bool editing_ = false;
};

struct MaskingState {
    MaskStackBinding masks;
    int selectedMask = -1;
    int selectedComponent = -1;
    MaskTool tool = MaskTool::NONE;
    bool showOverlay = true;  // red overlay of the selected mask while the Masking panel is visible
    bool panelVisible = false;  // set by the Masking panel each frame it draws; the shell resets it
    int visibleFrames = 0;      // consecutive frames the panel has been visible (capped at 2)

    // Brush options.
    float brushSize = 60.0f;  // radius in canvas pixels
    float brushFeather = 50.0f;
    float brushFlow = 80.0f;
    bool brushErase = false;

    // Viewport stroke state.
    bool stroking = false;
    glm::vec2 lastDab{0.0f};     // canvas pixels
    glm::vec2 dragStart{0.0f};   // canvas pixels, for gradients
    int overlaySent = -2;        // last overlay index posted to the engine (-2: none yet)

    [[nodiscard]] LocalAdjustment* mask() noexcept;
    [[nodiscard]] MaskComponent* component() noexcept;
    void clampSelection() noexcept;
};

// Viewport hooks. `imageMin` is the canvas origin on screen, `scale` screen
// pixels per canvas pixel, `canvasSize` the canvas in pixels.
struct CanvasView {
    glm::vec2 imageMin{0.0f};
    float scale = 1.0f;
    glm::vec2 canvasSize{1.0f};
};

// Handles left-button input for the active tool. Returns true when it used
// the input (the viewport must then not pan with the left button).
bool maskingViewportInput(PanelContext& ctx, const CanvasView& view, bool hovered, bool active);
// Draws the brush cursor and the selected gradient's guides.
void drawMaskingOverlay(PanelContext& ctx, const CanvasView& view, bool hovered);
// Keeps the engine's mask overlay in step with the selection and tool.
void syncMaskOverlay(PanelContext& ctx);

}  // namespace darkhouse::ui
