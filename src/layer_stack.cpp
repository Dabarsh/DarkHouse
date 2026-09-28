#include "layer_stack.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace darkhouse {
namespace {

float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

// Clips [origin, origin + extent) against [0, limit). The math is 64-bit so
// huge extents cannot overflow. Returns the exclusive end.
std::uint32_t clippedEnd(std::uint32_t origin, std::uint32_t extent, std::uint32_t limit) noexcept {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(std::uint64_t{origin} + extent, limit));
}

bool lessRowMajor(const TileKey& a, const TileKey& b) noexcept {
    return a.ty != b.ty ? a.ty < b.ty : a.tx < b.tx;
}

// Source-over with a separable blend mode (W3C Compositing Level 1 section 5.2)
// on straight-alpha RGBA. `sourceAlpha` already includes opacity and mask.
void blendPixel(float* dst, const float* src, float sourceAlpha, BlendMode mode) noexcept {
    if (!(sourceAlpha > 0.0f)) return;
    const float backdropAlpha = dst[3];
    const float outAlpha = sourceAlpha + backdropAlpha * (1.0f - sourceAlpha);
    for (int c = 0; c < 3; ++c) {
        const float cb = dst[c];
        const float cs = src[c];
        const float mixed = (1.0f - backdropAlpha) * cs + backdropAlpha * blendChannel(mode, cb, cs);
        const float premultiplied = sourceAlpha * mixed + backdropAlpha * cb * (1.0f - sourceAlpha);
        dst[c] = outAlpha > 0.0f ? premultiplied / outAlpha : 0.0f;
    }
    dst[3] = outAlpha;
}

// Reads `layer`'s tile `key` into a tw x th RGBA float buffer. Returns false if
// the tile is unallocated and transparent, so the caller can skip it.
bool sampleRasterTile(const SparseRasterLayer& layer, TileKey key, std::uint32_t tw, std::uint32_t th,
                      std::vector<float>& out) {
    const PixelTile* tile = layer.getTile(key);
    if (!tile && layer.defaultValue() == 0.0f) return false;
    out.assign(std::size_t{tw} * th * 4, layer.defaultValue());
    if (!tile) return true;
    const std::uint32_t channels = tile->channels;
    for (std::uint32_t y = 0; y < std::min(th, tile->height); ++y) {
        for (std::uint32_t x = 0; x < std::min(tw, tile->width); ++x) {
            const std::uint16_t* texel = tile->data.data() + tile->index(x, y);
            float* pixel = out.data() + (std::size_t{y} * tw + x) * 4;
            for (std::uint32_t c = 0; c < std::min<std::uint32_t>(channels, 4); ++c) pixel[c] = halfToFloat(texel[c]);
            if (channels < 4) pixel[3] = 1.0f;  // no alpha channel means opaque
        }
    }
    return true;
}

float sampleMask(const SparseRasterLayer& mask, const PixelTile* tile, std::uint32_t x, std::uint32_t y) {
    if (!tile || x >= tile->width || y >= tile->height) return mask.defaultValue();
    return clamp01(halfToFloat(tile->data[tile->index(x, y)]));
}

void compositeInto(const LayerNode& node, TileKey key, std::uint32_t tw, std::uint32_t th, std::vector<float>& dst) {
    if (!node.visible() || node.opacity() <= 0.0f) return;

    std::vector<float> source;
    if (node.isGroup()) {
        // Isolated group: children are composited onto transparency first.
        source.assign(dst.size(), 0.0f);
        for (std::size_t i = 0; i < node.childCount(); ++i) compositeInto(node.child(i), key, tw, th, source);
    } else if (const SparseRasterLayer* raster = node.raster()) {
        if (!sampleRasterTile(*raster, key, tw, th, source)) return;
    } else {
        return;  // GPU-only content
    }

    const SparseRasterLayer* mask = node.maskEnabled() ? node.mask() : nullptr;
    const PixelTile* maskTile = mask ? mask->getTile(key) : nullptr;
    for (std::uint32_t y = 0; y < th; ++y) {
        for (std::uint32_t x = 0; x < tw; ++x) {
            const std::size_t i = (std::size_t{y} * tw + x) * 4;
            float alpha = clamp01(source[i + 3]) * node.opacity();
            if (mask) alpha *= sampleMask(*mask, maskTile, x, y);
            blendPixel(&dst[i], &source[i], alpha, node.blendMode());
        }
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// SparseRasterLayer
// -----------------------------------------------------------------------------

SparseRasterLayer::SparseRasterLayer(std::uint32_t width, std::uint32_t height, std::uint32_t channels,
                                     float defaultValue)
    : width_(width),
      height_(height),
      channels_(channels),
      defaultValue_(defaultValue),
      defaultHalf_(floatToHalf(defaultValue)) {
    if (width == 0 || height == 0) throw std::invalid_argument("SparseRasterLayer: zero-sized layer");
    if (channels == 0 || channels > 4) throw std::invalid_argument("SparseRasterLayer: channels must be 1..4");
}

const PixelTile* SparseRasterLayer::getTile(TileKey key) const noexcept {
    const auto it = tiles_.find(key);
    return it == tiles_.end() ? nullptr : &it->second;
}

PixelTile* SparseRasterLayer::getTile(TileKey key) noexcept {
    const auto it = tiles_.find(key);
    return it == tiles_.end() ? nullptr : &it->second;
}

PixelTile& SparseRasterLayer::getOrCreateTile(TileKey key) {
    if (key.tx >= tilesX() || key.ty >= tilesY()) throw std::out_of_range("SparseRasterLayer: tile outside layer");
    if (PixelTile* existing = getTile(key)) return *existing;
    const std::uint32_t tileWidth = std::min(TILE_SIZE, width_ - key.tx * TILE_SIZE);
    const std::uint32_t tileHeight = std::min(TILE_SIZE, height_ - key.ty * TILE_SIZE);
    return tiles_.emplace(key, PixelTile(tileWidth, tileHeight, channels_, defaultHalf_)).first->second;
}

void SparseRasterLayer::releaseTile(TileKey key) {
    if (tiles_.erase(key) > 0) dirty_.insert(key);
}

void SparseRasterLayer::markTileDirty(TileKey key, PixelTile& tile) {
    tile.dirty = true;
    dirty_.insert(key);
}

bool SparseRasterLayer::writePixel(std::uint32_t x, std::uint32_t y, std::span<const float> values) {
    if (values.size() != channels_) throw std::invalid_argument("writePixel: value count must equal channels()");
    if (x >= width_ || y >= height_) return false;
    const TileKey key = tileKeyFor(x, y);
    PixelTile& tile = getOrCreateTile(key);
    std::uint16_t* texel = tile.data.data() + tile.index(x % TILE_SIZE, y % TILE_SIZE);
    for (std::uint32_t c = 0; c < channels_; ++c) texel[c] = floatToHalf(values[c]);
    markTileDirty(key, tile);
    return true;
}

void SparseRasterLayer::readPixel(std::uint32_t x, std::uint32_t y, std::span<float> out) const {
    if (out.size() != channels_) throw std::invalid_argument("readPixel: output size must equal channels()");
    const PixelTile* tile = (x < width_ && y < height_) ? getTile(tileKeyFor(x, y)) : nullptr;
    if (!tile) {
        std::fill(out.begin(), out.end(), defaultValue_);
        return;
    }
    const std::uint16_t* texel = tile->data.data() + tile->index(x % TILE_SIZE, y % TILE_SIZE);
    for (std::uint32_t c = 0; c < channels_; ++c) out[c] = halfToFloat(texel[c]);
}

void SparseRasterLayer::writeRegion(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                                    std::span<const float> source, std::size_t srcRowStride) {
    if (w == 0 || h == 0) return;
    const std::size_t rowFloats = std::size_t{w} * channels_;
    if (srcRowStride == 0) srcRowStride = rowFloats;
    if (srcRowStride < rowFloats || source.size() < (std::size_t{h} - 1) * srcRowStride + rowFloats) {
        throw std::invalid_argument("writeRegion: source buffer is smaller than the region");
    }
    if (x >= width_ || y >= height_) return;
    const std::uint32_t x1 = clippedEnd(x, w, width_);
    const std::uint32_t y1 = clippedEnd(y, h, height_);

    for (std::uint32_t ty = y / TILE_SIZE; ty <= (y1 - 1) / TILE_SIZE; ++ty) {
        for (std::uint32_t tx = x / TILE_SIZE; tx <= (x1 - 1) / TILE_SIZE; ++tx) {
            const TileKey key{tx, ty};
            PixelTile& tile = getOrCreateTile(key);
            const std::uint32_t tileX0 = tx * TILE_SIZE;
            const std::uint32_t tileY0 = ty * TILE_SIZE;
            const std::uint32_t cx0 = std::max(x, tileX0);
            const std::uint32_t cx1 = std::min(x1, tileX0 + tile.width);
            const std::uint32_t cy0 = std::max(y, tileY0);
            const std::uint32_t cy1 = std::min(y1, tileY0 + tile.height);
            const std::size_t count = std::size_t{cx1 - cx0} * channels_;
            for (std::uint32_t py = cy0; py < cy1; ++py) {
                const float* src = source.data() + std::size_t{py - y} * srcRowStride + std::size_t{cx0 - x} * channels_;
                std::uint16_t* dst = tile.data.data() + tile.index(cx0 - tileX0, py - tileY0);
                for (std::size_t i = 0; i < count; ++i) dst[i] = floatToHalf(src[i]);
            }
            markTileDirty(key, tile);
        }
    }
}

void SparseRasterLayer::markDirtyRegion(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    if (w == 0 || h == 0 || x >= width_ || y >= height_) return;
    const std::uint32_t x1 = clippedEnd(x, w, width_);
    const std::uint32_t y1 = clippedEnd(y, h, height_);
    for (std::uint32_t ty = y / TILE_SIZE; ty <= (y1 - 1) / TILE_SIZE; ++ty) {
        for (std::uint32_t tx = x / TILE_SIZE; tx <= (x1 - 1) / TILE_SIZE; ++tx) {
            const TileKey key{tx, ty};
            if (PixelTile* tile = getTile(key)) tile->dirty = true;
            dirty_.insert(key);
        }
    }
}

std::vector<TileKey> SparseRasterLayer::takeDirtyTiles() {
    std::vector<TileKey> keys(dirty_.begin(), dirty_.end());
    std::sort(keys.begin(), keys.end(), lessRowMajor);
    for (const TileKey& key : keys) {
        if (PixelTile* tile = getTile(key)) tile->dirty = false;
    }
    dirty_.clear();
    return keys;
}

std::size_t SparseRasterLayer::residentBytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& entry : tiles_) bytes += entry.second.byteSize();
    return bytes;
}

// -----------------------------------------------------------------------------
// LayerNode
// -----------------------------------------------------------------------------

LayerNode::LayerNode(std::string name, LayerType type, Content content)
    : name_(std::move(name)), type_(type), content_(std::move(content)) {}

std::unique_ptr<LayerNode> LayerNode::createGroup(std::string name) {
    return std::unique_ptr<LayerNode>(new LayerNode(std::move(name), LayerType::GROUP, std::monostate{}));
}

std::unique_ptr<LayerNode> LayerNode::createRaster(std::string name, std::uint32_t width, std::uint32_t height) {
    return std::unique_ptr<LayerNode>(
        new LayerNode(std::move(name), LayerType::RASTER_PIXEL, SparseRasterLayer(width, height, 4, 0.0f)));
}

std::unique_ptr<LayerNode> LayerNode::createAdjustment(std::string name, AdjustmentContent adjustment) {
    return std::unique_ptr<LayerNode>(
        new LayerNode(std::move(name), LayerType::PARAMETRIC_ADJUSTMENT, std::move(adjustment)));
}

std::unique_ptr<LayerNode> LayerNode::createVector(std::string name, VectorContent shape) {
    return std::unique_ptr<LayerNode>(new LayerNode(std::move(name), LayerType::VECTOR_SHAPE, std::move(shape)));
}

std::unique_ptr<LayerNode> LayerNode::createSmartObject(std::string name, SmartObjectContent object) {
    return std::unique_ptr<LayerNode>(new LayerNode(std::move(name), LayerType::SMART_OBJECT, std::move(object)));
}

void LayerNode::setOpacity(float opacity) noexcept { opacity_ = std::isfinite(opacity) ? clamp01(opacity) : 1.0f; }

SparseRasterLayer& LayerNode::addMask(std::uint32_t width, std::uint32_t height) {
    mask_ = std::make_unique<SparseRasterLayer>(width, height, 1, 1.0f);
    maskEnabled_ = true;
    mask_->markDirtyRegion(0, 0, width, height);  // the GPU copy must switch to "reveal all"
    return *mask_;
}

LayerNode& LayerNode::addChild(std::unique_ptr<LayerNode>&& child) {
    return insertChild(children_.size(), std::move(child));
}

LayerNode& LayerNode::insertChild(std::size_t index, std::unique_ptr<LayerNode>&& child) {
    if (!child) throw std::invalid_argument("LayerNode::insertChild: child is null");
    if (!isGroup()) throw std::logic_error("LayerNode '" + name_ + "' is not a group and cannot have children");
    if (child->parent_ != nullptr) throw std::logic_error("LayerNode '" + child->name_ + "' already has a parent");
    if (child.get() == this || child->isAncestorOf(*this)) {
        throw std::logic_error("LayerNode: adding '" + child->name_ + "' would create a cycle");
    }
    if (index > children_.size()) throw std::out_of_range("LayerNode::insertChild: index out of range");
    child->parent_ = this;
    auto it = children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
    return **it;
}

std::unique_ptr<LayerNode> LayerNode::removeChild(const LayerNode& child) {
    const auto it = std::find_if(children_.begin(), children_.end(),
                                 [&](const std::unique_ptr<LayerNode>& c) { return c.get() == &child; });
    if (it == children_.end()) throw std::invalid_argument("LayerNode::removeChild: not a child of '" + name_ + "'");
    std::unique_ptr<LayerNode> removed = std::move(*it);
    children_.erase(it);
    removed->parent_ = nullptr;
    return removed;
}

void LayerNode::moveChild(std::size_t from, std::size_t to) {
    if (from >= children_.size() || to >= children_.size()) {
        throw std::out_of_range("LayerNode::moveChild: index out of range");
    }
    if (from == to) return;
    auto node = std::move(children_[from]);
    children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(from));
    children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(to), std::move(node));
}

LayerNode& LayerNode::child(std::size_t index) {
    if (index >= children_.size()) throw std::out_of_range("LayerNode::child: index out of range");
    return *children_[index];
}

const LayerNode& LayerNode::child(std::size_t index) const {
    if (index >= children_.size()) throw std::out_of_range("LayerNode::child: index out of range");
    return *children_[index];
}

bool LayerNode::isAncestorOf(const LayerNode& other) const noexcept {
    for (const LayerNode* p = other.parent_; p != nullptr; p = p->parent_) {
        if (p == this) return true;
    }
    return false;
}

std::vector<TileKey> takeDirtyTiles(LayerNode& root) {
    std::unordered_set<TileKey, TileKeyHash> merged;
    root.visit([&](LayerNode& node, std::size_t) {
        if (SparseRasterLayer* raster = node.raster()) {
            for (const TileKey& key : raster->takeDirtyTiles()) merged.insert(key);
        }
        if (SparseRasterLayer* mask = node.mask()) {
            for (const TileKey& key : mask->takeDirtyTiles()) merged.insert(key);
        }
    });
    std::vector<TileKey> keys(merged.begin(), merged.end());
    std::sort(keys.begin(), keys.end(), lessRowMajor);
    return keys;
}

// -----------------------------------------------------------------------------
// Compositing
// -----------------------------------------------------------------------------

float blendChannel(BlendMode mode, float backdrop, float source) noexcept {
    switch (mode) {
    case BlendMode::NORMAL:
        return source;
    case BlendMode::MULTIPLY:
        return backdrop * source;
    case BlendMode::SCREEN: {
        const float cb = clamp01(backdrop), cs = clamp01(source);
        return cb + cs - cb * cs;
    }
    case BlendMode::OVERLAY: {  // HardLight with the layers swapped
        const float cb = clamp01(backdrop), cs = clamp01(source);
        return cb <= 0.5f ? 2.0f * cb * cs : 1.0f - 2.0f * (1.0f - cb) * (1.0f - cs);
    }
    case BlendMode::COLOR_DODGE: {
        const float cb = clamp01(backdrop), cs = clamp01(source);
        if (cb <= 0.0f) return 0.0f;
        if (cs >= 1.0f) return 1.0f;
        return std::min(1.0f, cb / (1.0f - cs));
    }
    }
    return source;
}

CompositedTile compositeTileCPU(const LayerNode& root, TileKey key, std::uint32_t canvasWidth,
                                std::uint32_t canvasHeight) {
    if (canvasWidth == 0 || canvasHeight == 0 || key.tx >= (canvasWidth + TILE_SIZE - 1) / TILE_SIZE ||
        key.ty >= (canvasHeight + TILE_SIZE - 1) / TILE_SIZE) {
        throw std::out_of_range("compositeTileCPU: tile outside the canvas");
    }
    CompositedTile out;
    out.width = std::min(TILE_SIZE, canvasWidth - key.tx * TILE_SIZE);
    out.height = std::min(TILE_SIZE, canvasHeight - key.ty * TILE_SIZE);
    out.rgba.assign(std::size_t{out.width} * out.height * 4, 0.0f);
    compositeInto(root, key, out.width, out.height, out.rgba);
    return out;
}

}  // namespace darkhouse
