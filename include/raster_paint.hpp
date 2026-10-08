// DarkHouse — painting into raster layers (the Canvas workspace's brush,
// eraser and clone stamp), and per-stroke undo.
//
// A stroke is a series of round dabs in layer pixels. A dab's coverage is
// full inside its hard core (hardness x radius) and falls smoothly to zero at
// the radius; flow scales every dab. Colour layers composite the brush colour
// over their pixels (straight alpha) or, erasing, fade their alpha; one-channel
// layer masks move towards the brush's grey level, or erasing, back towards
// "reveal" (1), like painting the background colour. The clone stamp copies
// the pixels at an offset under the same coverage.
#pragma once

#include "layer_stack.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace darkhouse {

struct BrushTip {
    float radius = 20.0f;   // layer pixels
    float hardness = 0.8f;  // 0 = soft from the centre .. 1 = hard edge (1 px anti-aliased)
    float flow = 1.0f;      // strength of each dab, 0..1
};

enum class DabMode : std::uint8_t { PAINT, ERASE };

// Coverage (0..1, before flow) at `distance` from a dab's centre.
[[nodiscard]] float dabCoverage(const BrushTip& tip, float distance) noexcept;

// Tiles a dab of `radius` at (x, y) can change.
[[nodiscard]] std::vector<TileKey> dabTiles(const SparseRasterLayer& layer, float x, float y, float radius);

// One dab of `color` (straight linear RGBA; its alpha scales the coverage).
void paintDab(SparseRasterLayer& layer, float x, float y, const BrushTip& tip, const std::array<float, 4>& color,
              DabMode mode);

// One dab of the clone stamp: the pixels of `source` at (x + dx, y + dy)
// copied to (x, y). `source` may be `layer` itself; the dab reads before it
// writes. Both must have the same channel count.
void cloneDab(SparseRasterLayer& layer, const SparseRasterLayer& source, float x, float y, float dx, float dy,
              const BrushTip& tip);

// The tiles a stroke is about to change, as they were before it.
class TileSnapshot {
public:
    explicit TileSnapshot(SparseRasterLayer& layer) noexcept : layer_(&layer) {}

    // Saves the tiles not saved yet.
    void capture(std::span<const TileKey> keys);
    // Puts every saved tile back (tiles that did not exist are released).
    void restore();

    [[nodiscard]] const SparseRasterLayer* layer() const noexcept { return layer_; }
    [[nodiscard]] bool empty() const noexcept { return saved_.empty(); }

private:
    SparseRasterLayer* layer_;
    std::unordered_map<TileKey, std::optional<std::vector<std::uint16_t>>, TileKeyHash> saved_;
};

}  // namespace darkhouse
