#include "ui/canvas_state.hpp"

#include <algorithm>

namespace darkhouse::ui {

const char* blendModeName(BlendMode mode) noexcept {
    switch (mode) {
    case BlendMode::NORMAL: return "Normal";
    case BlendMode::MULTIPLY: return "Multiply";
    case BlendMode::SCREEN: return "Screen";
    case BlendMode::OVERLAY: return "Overlay";
    case BlendMode::COLOR_DODGE: return "Color Dodge";
    }
    return "?";
}

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

std::vector<float> compositeCanvas(const LayerNode& root, std::uint32_t width, std::uint32_t height) {
    std::vector<float> rgba(std::size_t{width} * height * 4, 0.0f);
    const std::uint32_t tilesX = (width + TILE_SIZE - 1) / TILE_SIZE;
    const std::uint32_t tilesY = (height + TILE_SIZE - 1) / TILE_SIZE;
    for (std::uint32_t ty = 0; ty < tilesY; ++ty) {
        for (std::uint32_t tx = 0; tx < tilesX; ++tx) {
            const CompositedTile tile = compositeTileCPU(root, {tx, ty}, width, height);
            for (std::uint32_t row = 0; row < tile.height; ++row) {
                const std::size_t dst = (std::size_t{ty * TILE_SIZE + row} * width + tx * TILE_SIZE) * 4;
                std::copy_n(tile.rgba.begin() + static_cast<std::ptrdiff_t>(std::size_t{row} * tile.width * 4),
                            std::size_t{tile.width} * 4, rgba.begin() + static_cast<std::ptrdiff_t>(dst));
            }
        }
    }
    return rgba;
}

void applyMaskToLayer(const LocalAdjustment& mask, LayerNode& layer, LayerNode& root, std::uint32_t canvasWidth,
                      std::uint32_t canvasHeight) {
    SparseRasterLayer* target = layer.mask();
    if (!target || target->width() != canvasWidth || target->height() != canvasHeight) {
        target = &layer.addMask(canvasWidth, canvasHeight);
    }
    // Ranges are measured on the composite as it is now (before develop).
    const std::vector<float> source =
        readsImage(mask) ? compositeCanvas(root, canvasWidth, canvasHeight) : std::vector<float>{};
    writeLayerMask(mask, *target, source);
    layer.setMaskEnabled(true);
    invalidateComposite(root);
}

void CanvasState::validate(const LayerNode& root) {
    if (selectedLayer && !containsLayer(root, selectedLayer)) selectedLayer = nullptr;
}

}  // namespace darkhouse::ui
