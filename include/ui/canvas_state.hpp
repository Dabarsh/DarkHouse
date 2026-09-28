// DarkHouse — state and helpers shared by the Canvas & Compositing panels.
//
// The Layers panel, the Properties panel and the viewport's canvas tools all
// work on the selected layer of the open document; CanvasState, owned by the
// shell, is where that selection lives.
#pragma once

#include "layer_stack.hpp"
#include "mask_engine.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace darkhouse::ui {

inline constexpr std::array<BlendMode, 5> kBlendModes{BlendMode::NORMAL, BlendMode::MULTIPLY, BlendMode::SCREEN,
                                                      BlendMode::OVERLAY, BlendMode::COLOR_DODGE};
[[nodiscard]] const char* blendModeName(BlendMode mode) noexcept;

// Re-dirties every raster tile and mask under `root`. Property changes
// (visibility, opacity, blend, masks, order) do not touch pixels, so without
// this the engine would keep showing the old composite.
void invalidateComposite(LayerNode& root);
[[nodiscard]] bool containsLayer(const LayerNode& root, const LayerNode* target);

// The whole document composited on the CPU (linear RGBA floats, row-major),
// for tools that measure colours over the canvas.
[[nodiscard]] std::vector<float> compositeCanvas(const LayerNode& root, std::uint32_t width, std::uint32_t height);

// Writes a develop mask (a Masking panel mask) into `layer`'s layer mask,
// creating one the size of the canvas if needed: the same shapes select
// pixels in Develop and hide them in the layer stack.
void applyMaskToLayer(const LocalAdjustment& mask, LayerNode& layer, LayerNode& root, std::uint32_t canvasWidth,
                      std::uint32_t canvasHeight);

struct CanvasState {
    LayerNode* selectedLayer = nullptr;

    // Drops the selection when the layer is no longer in `root` (deleted, or
    // the document was replaced by a newly opened photo).
    void validate(const LayerNode& root);
};

}  // namespace darkhouse::ui
