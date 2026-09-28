// DarkHouse — layer stack and sparse tiled raster storage.
//
// Large pixel surfaces are stored as sparse 512x512 FP16 tiles. A tile is only
// allocated once something is painted into it, so a mostly empty 20k x 20k
// layer costs almost nothing. Every write records its tile as dirty, and the
// renderer re-uploads only those tiles to the GPU.
//
// Documents are trees of LayerNode. Groups hold ordered children, and any node
// can carry a non-destructive single-channel mask.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace darkhouse {

inline constexpr std::uint32_t TILE_SIZE = 512;

// -----------------------------------------------------------------------------
// IEEE 754 binary16 <-> binary32. Round-to-nearest-even, and Inf, NaN and
// subnormals are preserved (after F. Giesen's reference conversions).
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::uint16_t floatToHalf(float value) noexcept {
    std::uint32_t f;
    std::memcpy(&f, &value, sizeof f);
    const std::uint32_t sign = f & 0x80000000u;
    f ^= sign;

    std::uint32_t half;
    if (f >= 0x47800000u) {  // >= 65536 after rounding, or Inf/NaN
        half = f > 0x7F800000u ? 0x7E00u : 0x7C00u;
    } else if (f < 0x38800000u) {  // below the smallest normal half: subnormal or zero
        // Adding 0.5f puts the 10 mantissa bits at the bottom of the float,
        // and the FPU does the round-to-nearest-even.
        constexpr std::uint32_t kMagicBits = 126u << 23;
        float magic;
        std::memcpy(&magic, &kMagicBits, sizeof magic);
        float magnitude;
        std::memcpy(&magnitude, &f, sizeof magnitude);
        magnitude += magic;
        std::memcpy(&f, &magnitude, sizeof f);
        half = f - kMagicBits;
    } else {
        const std::uint32_t mantissaOdd = (f >> 13) & 1u;
        f -= 112u << 23;            // rebias exponent 127 -> 15
        f += 0xFFFu + mantissaOdd;  // round to nearest even
        half = f >> 13;
    }
    return static_cast<std::uint16_t>(half | (sign >> 16));
}

[[nodiscard]] inline float halfToFloat(std::uint16_t half) noexcept {
    constexpr std::uint32_t kShiftedExponent = 0x7C00u << 13;
    std::uint32_t bits = (static_cast<std::uint32_t>(half) & 0x7FFFu) << 13;
    const std::uint32_t exponent = bits & kShiftedExponent;
    bits += (127u - 15u) << 23;

    float out;
    if (exponent == kShiftedExponent) {  // Inf / NaN
        bits += (128u - 16u) << 23;
        std::memcpy(&out, &bits, sizeof out);
    } else if (exponent == 0) {  // zero / subnormal: renormalize through the FPU
        bits += 1u << 23;
        std::memcpy(&out, &bits, sizeof out);
        constexpr std::uint32_t kMagicBits = 113u << 23;
        float magic;
        std::memcpy(&magic, &kMagicBits, sizeof magic);
        out -= magic;
    } else {
        std::memcpy(&out, &bits, sizeof out);
    }
    std::uint32_t result;
    std::memcpy(&result, &out, sizeof result);
    result |= (static_cast<std::uint32_t>(half) & 0x8000u) << 16;
    std::memcpy(&out, &result, sizeof out);
    return out;
}

// -----------------------------------------------------------------------------
// Tiles
// -----------------------------------------------------------------------------

struct TileKey {
    std::uint32_t tx = 0;  // tile column (pixel x / TILE_SIZE)
    std::uint32_t ty = 0;  // tile row    (pixel y / TILE_SIZE)

    bool operator==(const TileKey&) const = default;
};

struct TileKeyHash {
    std::size_t operator()(const TileKey& key) const noexcept {
        // splitmix64 finalizer. std::hash on integers is often the identity,
        // which clusters neighbouring tiles into the same buckets.
        std::uint64_t x = (static_cast<std::uint64_t>(key.tx) << 32) | key.ty;
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x);
    }
};

struct PixelTile {
    PixelTile(std::uint32_t tileWidth, std::uint32_t tileHeight, std::uint32_t channelCount, std::uint16_t fill)
        : width(tileWidth),
          height(tileHeight),
          channels(channelCount),
          data(std::size_t{tileWidth} * tileHeight * channelCount, fill) {}

    std::uint32_t width;     // <= TILE_SIZE; tiles on the right/bottom edge are clipped to the layer
    std::uint32_t height;
    std::uint32_t channels;  // 4 for colour layers, 1 for masks
    std::vector<std::uint16_t> data;  // binary16, row-major, interleaved, tightly packed
    bool dirty = false;               // changed since the last GPU upload

    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const noexcept {
        return (std::size_t{y} * width + x) * channels;
    }
    [[nodiscard]] std::size_t byteSize() const noexcept { return data.size() * sizeof(std::uint16_t); }
};

// A bounded, sparsely allocated FP16 surface. Unallocated tiles read as
// `defaultValue` in every channel: 0 (transparent) for colour, 1 (reveal all)
// for masks. Writes outside the layer are clipped. Not thread-safe.
class SparseRasterLayer {
public:
    SparseRasterLayer(std::uint32_t width, std::uint32_t height, std::uint32_t channels = 4,
                      float defaultValue = 0.0f);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] float defaultValue() const noexcept { return defaultValue_; }
    [[nodiscard]] std::uint32_t tilesX() const noexcept { return (width_ + TILE_SIZE - 1) / TILE_SIZE; }
    [[nodiscard]] std::uint32_t tilesY() const noexcept { return (height_ + TILE_SIZE - 1) / TILE_SIZE; }
    [[nodiscard]] static TileKey tileKeyFor(std::uint32_t x, std::uint32_t y) noexcept {
        return {x / TILE_SIZE, y / TILE_SIZE};
    }

    // Writes one pixel (`values.size()` must equal channels()). Returns false if
    // (x, y) is outside the layer.
    bool writePixel(std::uint32_t x, std::uint32_t y, std::span<const float> values);
    // Fills `out` with the pixel at (x, y), or with defaultValue() outside the layer.
    void readPixel(std::uint32_t x, std::uint32_t y, std::span<float> out) const;

    // Copies a w x h block of interleaved floats with its top-left corner at
    // (x, y), clipped to the layer. `srcRowStride` is in floats; 0 means tightly
    // packed. The tile-at-a-time loop keeps this fast for brushes and imports.
    void writeRegion(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                     std::span<const float> source, std::size_t srcRowStride = 0);

    [[nodiscard]] const PixelTile* getTile(TileKey key) const noexcept;
    [[nodiscard]] PixelTile* getTile(TileKey key) noexcept;
    PixelTile& getOrCreateTile(TileKey key);
    // Frees a tile (it reads as defaultValue() again) and marks it dirty so the
    // GPU copy gets cleared.
    void releaseTile(TileKey key);

    // Marks every tile touching the rectangle as needing a GPU upload, for
    // example after a filter rewrites pixels in place. This includes tiles that
    // are not allocated.
    void markDirtyRegion(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);
    [[nodiscard]] bool hasDirtyTiles() const noexcept { return !dirty_.empty(); }
    // Returns the dirty tiles in row-major order and clears their dirty state.
    // A key with no allocated tile means "upload defaultValue()".
    [[nodiscard]] std::vector<TileKey> takeDirtyTiles();

    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }
    [[nodiscard]] std::size_t residentBytes() const noexcept;
    [[nodiscard]] const std::unordered_map<TileKey, PixelTile, TileKeyHash>& tiles() const noexcept { return tiles_; }

private:
    void markTileDirty(TileKey key, PixelTile& tile);

    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t channels_;
    float defaultValue_;
    std::uint16_t defaultHalf_;
    std::unordered_map<TileKey, PixelTile, TileKeyHash> tiles_;
    std::unordered_set<TileKey, TileKeyHash> dirty_;
};

// -----------------------------------------------------------------------------
// Layer tree
// -----------------------------------------------------------------------------

enum class LayerType : std::uint8_t {
    PARAMETRIC_ADJUSTMENT,  // GPU operator applied to everything beneath it
    RASTER_PIXEL,           // sparse FP16 pixels
    VECTOR_SHAPE,           // resolution-independent path
    SMART_OBJECT,           // non-destructive reference to another catalog asset
    GROUP,                  // container that composites its children in isolation
};

enum class BlendMode : std::uint8_t { NORMAL, MULTIPLY, SCREEN, OVERLAY, COLOR_DODGE };

// PARAMETRIC_ADJUSTMENT payload. Uses the same (type, params) pair as an
// edit_nodes row, so it maps straight onto createComputeNode().
struct AdjustmentContent {
    std::string nodeType;
    std::vector<std::byte> serializedParams;
};

enum class PathVerb : std::uint8_t { MOVE_TO, LINE_TO, CUBIC_TO, CLOSE };

// VECTOR_SHAPE payload. Coordinates are in canvas pixels.
struct VectorContent {
    std::vector<PathVerb> verbs;
    std::vector<float> points;  // x,y pairs: MOVE_TO/LINE_TO use 1 pair, CUBIC_TO 3, CLOSE 0
    std::array<float, 4> fillColor{0.0f, 0.0f, 0.0f, 1.0f};    // straight-alpha linear RGBA
    std::array<float, 4> strokeColor{0.0f, 0.0f, 0.0f, 0.0f};
    float strokeWidth = 0.0f;
};

// SMART_OBJECT payload: renders another asset through its own develop stack.
struct SmartObjectContent {
    std::string sourceAssetId;
    std::array<float, 6> transform{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};  // affine [a b c d tx ty]
};

class LayerNode {
public:
    using Content = std::variant<std::monostate, SparseRasterLayer, AdjustmentContent, VectorContent, SmartObjectContent>;

    [[nodiscard]] static std::unique_ptr<LayerNode> createGroup(std::string name);
    [[nodiscard]] static std::unique_ptr<LayerNode> createRaster(std::string name, std::uint32_t width,
                                                                 std::uint32_t height);
    [[nodiscard]] static std::unique_ptr<LayerNode> createAdjustment(std::string name, AdjustmentContent adjustment);
    [[nodiscard]] static std::unique_ptr<LayerNode> createVector(std::string name, VectorContent shape);
    [[nodiscard]] static std::unique_ptr<LayerNode> createSmartObject(std::string name, SmartObjectContent object);

    LayerNode(const LayerNode&) = delete;
    LayerNode& operator=(const LayerNode&) = delete;

    // Identity
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] LayerType type() const noexcept { return type_; }
    [[nodiscard]] bool isGroup() const noexcept { return type_ == LayerType::GROUP; }

    // Compositing
    [[nodiscard]] BlendMode blendMode() const noexcept { return blendMode_; }
    void setBlendMode(BlendMode mode) noexcept { blendMode_ = mode; }
    [[nodiscard]] float opacity() const noexcept { return opacity_; }
    void setOpacity(float opacity) noexcept;  // clamped to [0, 1]
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    void setVisible(bool visible) noexcept { visible_ = visible; }

    // Content. Each accessor returns nullptr when the node is of another type.
    [[nodiscard]] SparseRasterLayer* raster() noexcept { return std::get_if<SparseRasterLayer>(&content_); }
    [[nodiscard]] const SparseRasterLayer* raster() const noexcept { return std::get_if<SparseRasterLayer>(&content_); }
    [[nodiscard]] AdjustmentContent* adjustment() noexcept { return std::get_if<AdjustmentContent>(&content_); }
    [[nodiscard]] const AdjustmentContent* adjustment() const noexcept {
        return std::get_if<AdjustmentContent>(&content_);
    }
    [[nodiscard]] VectorContent* vectorShape() noexcept { return std::get_if<VectorContent>(&content_); }
    [[nodiscard]] const VectorContent* vectorShape() const noexcept { return std::get_if<VectorContent>(&content_); }
    [[nodiscard]] SmartObjectContent* smartObject() noexcept { return std::get_if<SmartObjectContent>(&content_); }
    [[nodiscard]] const SmartObjectContent* smartObject() const noexcept {
        return std::get_if<SmartObjectContent>(&content_);
    }

    // Non-destructive layer mask: one channel where 1 reveals and 0 hides.
    // A new mask reveals everything. Disabling a mask keeps its pixels.
    SparseRasterLayer& addMask(std::uint32_t width, std::uint32_t height);  // replaces any existing mask
    [[nodiscard]] SparseRasterLayer* mask() noexcept { return mask_.get(); }
    [[nodiscard]] const SparseRasterLayer* mask() const noexcept { return mask_.get(); }
    void removeMask() noexcept { mask_.reset(); }
    [[nodiscard]] bool maskEnabled() const noexcept { return maskEnabled_ && mask_ != nullptr; }
    void setMaskEnabled(bool enabled) noexcept { maskEnabled_ = enabled; }

    // Hierarchy. Only GROUP nodes have children; they are ordered bottom to top.
    // Children are taken by rvalue reference, so ownership moves only on
    // success. If the call throws, the caller still owns the node.
    LayerNode& addChild(std::unique_ptr<LayerNode>&& child);
    LayerNode& insertChild(std::size_t index, std::unique_ptr<LayerNode>&& child);
    std::unique_ptr<LayerNode> removeChild(const LayerNode& child);
    void moveChild(std::size_t from, std::size_t to);
    [[nodiscard]] std::size_t childCount() const noexcept { return children_.size(); }
    [[nodiscard]] LayerNode& child(std::size_t index);
    [[nodiscard]] const LayerNode& child(std::size_t index) const;
    [[nodiscard]] LayerNode* parent() const noexcept { return parent_; }
    [[nodiscard]] bool isAncestorOf(const LayerNode& other) const noexcept;

    // Pre-order traversal: parent before children, children bottom to top.
    // visitor(node, depth).
    template <class Visitor>
    void visit(Visitor&& visitor, std::size_t depth = 0) {
        visitor(*this, depth);
        for (auto& c : children_) c->visit(visitor, depth + 1);
    }
    template <class Visitor>
    void visit(Visitor&& visitor, std::size_t depth = 0) const {
        visitor(*this, depth);
        for (const auto& c : children_) static_cast<const LayerNode&>(*c).visit(visitor, depth + 1);
    }

private:
    LayerNode(std::string name, LayerType type, Content content);

    std::string name_;
    LayerType type_;
    Content content_;
    BlendMode blendMode_ = BlendMode::NORMAL;
    float opacity_ = 1.0f;
    bool visible_ = true;
    std::unique_ptr<SparseRasterLayer> mask_;
    bool maskEnabled_ = true;
    LayerNode* parent_ = nullptr;
    std::vector<std::unique_ptr<LayerNode>> children_;
};

// Takes the dirty tiles of every raster layer and mask under `root`, merged and
// sorted row-major. These are the composite tiles that need re-uploading.
[[nodiscard]] std::vector<TileKey> takeDirtyTiles(LayerNode& root);

// -----------------------------------------------------------------------------
// CPU reference compositor
// -----------------------------------------------------------------------------

// Separable blend function B(Cb, Cs) from the W3C Compositing and Blending
// Level 1 spec. SCREEN, OVERLAY and COLOR_DODGE are display-referred, so their
// inputs are clamped to [0, 1]. NORMAL and MULTIPLY accept HDR values.
[[nodiscard]] float blendChannel(BlendMode mode, float backdrop, float source) noexcept;

struct CompositedTile {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba;  // straight alpha, linear light, width * height * 4
};

// Composites the raster content under `root` for one canvas tile: groups are
// isolated, masks, opacity and blend modes are honoured, and invisible layers
// are skipped. Adjustment, vector and smart-object layers only render on the GPU
// and are skipped here. Used for thumbnails, tests and GPU parity checks.
[[nodiscard]] CompositedTile compositeTileCPU(const LayerNode& root, TileKey key, std::uint32_t canvasWidth,
                                              std::uint32_t canvasHeight);

}  // namespace darkhouse
