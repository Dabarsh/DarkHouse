// DarkHouse — state and helpers shared by the Canvas & Compositing panels.
//
// The Layers panel, the Properties panel and the viewport's canvas tools all
// work on the selected layer of the open document; CanvasState, owned by the
// shell, is where that selection lives.
#pragma once

#include "asset_manager.hpp"
#include "layer_stack.hpp"
#include "mask_engine.hpp"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <vector>

namespace darkhouse::ui {

// Blend modes in menu order; a group ends after each entry marked here.
inline constexpr std::array<BlendMode, kBlendModeCount> kBlendModes{
    BlendMode::NORMAL,     BlendMode::DARKEN,     BlendMode::MULTIPLY,   BlendMode::COLOR_BURN,
    BlendMode::LIGHTEN,    BlendMode::SCREEN,     BlendMode::COLOR_DODGE, BlendMode::OVERLAY,
    BlendMode::SOFT_LIGHT, BlendMode::HARD_LIGHT, BlendMode::DIFFERENCE, BlendMode::EXCLUSION,
    BlendMode::HUE,        BlendMode::SATURATION, BlendMode::COLOR,      BlendMode::LUMINOSITY};
[[nodiscard]] bool endsBlendGroup(BlendMode mode) noexcept;
[[nodiscard]] const char* blendModeName(BlendMode mode) noexcept;
// Blend mode combo; returns true when the mode changed.
bool blendModeCombo(const char* id, BlendMode& mode);

// Recomposites the whole document on the next frame: for content edited
// through the mutable accessors (vector paths and colours). Layer property
// setters do this themselves.
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

// The selected layer's transformed content box over the canvas (Canvas
// workspace): `imageMin` is the canvas origin on screen, `scale` screen
// pixels per canvas pixel.
void drawLayerOutline(const LayerNode& layer, ImDrawList* drawList, ImVec2 imageMin, float scale);

struct CanvasState {
    LayerNode* selectedLayer = nullptr;

    // Drops the selection when the layer is no longer in `root` (deleted, or
    // the document was replaced by a newly opened photo).
    void validate(const LayerNode& root);

    // Smart objects: `layer`'s source photo is decoded on a worker thread,
    // with the asset's point adjustments (exposure, white balance, tone
    // curve, colour mixer, colour grading) applied, at most `maxDimension`
    // pixels on its long edge. pollSmartObjects() (every frame) hands the
    // pixels to layers still in the document and places them centred at up
    // to half the canvas.
    void loadSmartObject(LayerNode& layer, const std::filesystem::path& file, std::vector<EditNodeRecord> editStack,
                         std::uint32_t maxDimension);
    void pollSmartObjects(LayerNode& root, std::uint32_t canvasWidth, std::uint32_t canvasHeight);
    [[nodiscard]] bool loadingSmartObject(const LayerNode* layer) const noexcept;

private:
    struct PendingSmartObject {
        LayerNode* layer;
        std::future<std::shared_ptr<const SparseRasterLayer>> pixels;
    };
    std::vector<PendingSmartObject> pendingSmartObjects_;
};

}  // namespace darkhouse::ui
