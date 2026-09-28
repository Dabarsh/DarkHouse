// Layer stack tests: bulk FP16 conversion (F16C or scalar) against the scalar
// reference, the pass-through composite against the full compositor, binary16
// region writes, the parallel loop, blend modes, layer transforms, vector
// shapes, adjustment layers, smart objects and dirty-tile tracking.

#include "color_adjust.hpp"
#include "layer_stack.hpp"
#include "parallel.hpp"
#include "render_pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

using namespace darkhouse;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::cout << "FAIL line " << __LINE__ << ": " #condition << '\n';              \
        }                                                                                  \
    } while (false)

std::uint32_t bitsOf(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

float fromBits(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

bool isHalfNaN(std::uint16_t h) { return (h & 0x7FFFu) > 0x7C00u; }

void testHalvesToFloats() {
    std::vector<std::uint16_t> all(65536);
    std::iota(all.begin(), all.end(), std::uint16_t{0});
    std::vector<float> bulk(all.size());
    halvesToFloats(all.data(), bulk.data(), all.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const float reference = halfToFloat(all[i]);
        const bool same = isHalfNaN(all[i]) ? std::isnan(bulk[i]) : bitsOf(bulk[i]) == bitsOf(reference);
        mismatches += same ? 0 : 1;
    }
    CHECK(mismatches == 0);
}

void testFloatsToHalves() {
    std::vector<float> values;
    // Every half and the midpoints between neighbours (ties round to even).
    for (std::uint32_t h = 0; h < 0x7C00; ++h) {
        for (std::uint32_t sign : {0u, 0x8000u}) {
            const float a = halfToFloat(static_cast<std::uint16_t>(h | sign));
            const float b = halfToFloat(static_cast<std::uint16_t>((h + 1) | sign));
            values.push_back(a);
            values.push_back(a + (b - a) * 0.5f);
            values.push_back(std::nextafter(a + (b - a) * 0.5f, 0.0f));
        }
    }
    // Overflow boundary, infinities, float subnormals.
    for (float v : {65504.0f, 65519.99f, 65520.0f, 65536.0f, 1e10f, INFINITY, -INFINITY, 1e-40f, -1e-40f, 5.96e-8f,
                    2.98e-8f, 2.99e-8f}) {
        values.push_back(v);
    }
    // A sweep over float bit patterns.
    for (std::uint64_t bits = 0; bits <= 0xFFFFFFFFull; bits += 4099) values.push_back(fromBits(static_cast<std::uint32_t>(bits)));
    values.push_back(1.0f);  // odd count: exercises the scalar tail

    std::vector<std::uint16_t> bulk(values.size());
    floatsToHalves(values.data(), bulk.data(), values.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint16_t reference = floatToHalf(values[i]);
        const bool same = std::isnan(values[i]) ? isHalfNaN(bulk[i]) : bulk[i] == reference;
        if (!same && mismatches++ < 5) {
            std::cout << "  float " << values[i] << " (0x" << std::hex << bitsOf(values[i]) << "): bulk 0x" << bulk[i]
                      << ", scalar 0x" << reference << std::dec << '\n';
        }
    }
    CHECK(mismatches == 0);
}

// Random binary16 RGBA with the awkward cases: alpha 0, -0, negative, above
// one, NaN; colour -0 and NaN. No infinities (see compositeTileHalf).
std::vector<std::uint16_t> awkwardPixels(std::size_t pixels, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> colour(-0.25f, 4.0f);
    std::uniform_int_distribution<int> pick(0, 15);
    std::vector<std::uint16_t> out(pixels * 4);
    for (std::size_t p = 0; p < pixels; ++p) {
        std::uint16_t* px = &out[p * 4];
        for (int c = 0; c < 3; ++c) px[c] = floatToHalf(colour(rng));
        if (pick(rng) == 0) px[0] = 0x8000;  // -0
        if (pick(rng) == 0) px[1] = 0x7E01;  // NaN
        switch (pick(rng)) {
        case 0: px[3] = 0x0000; break;
        case 1: px[3] = 0x8000; break;
        case 2: px[3] = floatToHalf(-0.5f); break;
        case 3: px[3] = floatToHalf(1.75f); break;
        case 4: px[3] = 0x7E00; break;
        case 5: px[3] = 0x0001; break;  // smallest subnormal
        default: px[3] = floatToHalf(std::uniform_real_distribution<float>(0.0f, 1.0f)(rng)); break;
        }
    }
    return out;
}

// compositeTileHalf must equal floatsToHalves(compositeTileCPU) (NaN: any NaN).
std::size_t compareComposites(const LayerNode& root, std::uint32_t width, std::uint32_t height) {
    std::size_t mismatches = 0;
    for (std::uint32_t ty = 0; ty < (height + TILE_SIZE - 1) / TILE_SIZE; ++ty) {
        for (std::uint32_t tx = 0; tx < (width + TILE_SIZE - 1) / TILE_SIZE; ++tx) {
            const CompositedTile reference = compositeTileCPU(root, {tx, ty}, width, height);
            const CompositedTileHalf fast = compositeTileHalf(root, {tx, ty}, width, height);
            if (fast.width != reference.width || fast.height != reference.height ||
                fast.rgba.size() != reference.rgba.size()) {
                return 1;
            }
            for (std::size_t i = 0; i < fast.rgba.size(); ++i) {
                const std::uint16_t expected = floatToHalf(reference.rgba[i]);
                const bool same = isHalfNaN(expected) ? isHalfNaN(fast.rgba[i]) : fast.rgba[i] == expected;
                mismatches += same ? 0 : 1;
            }
        }
    }
    return mismatches;
}

void testCompositeTileHalf() {
    const std::uint32_t w = 700, h = 530;  // partial edge tiles; one tile left unallocated
    auto makeDocument = [&](std::uint32_t seed) {
        std::unique_ptr<LayerNode> root = LayerNode::createGroup("Document");
        LayerNode& background = root->addChild(LayerNode::createRaster("Background", w, h));
        const std::vector<std::uint16_t> pixels = awkwardPixels(std::size_t{w} * 400, seed);
        background.raster()->writeRegion(0, 0, w, 400, pixels);  // rows 400.. stay unallocated in tile row 1
        return root;
    };

    // Pass-through: a lone raster, alone or next to content the CPU skips.
    auto single = makeDocument(1);
    CHECK(compareComposites(*single, w, h) == 0);
    single->addChild(LayerNode::createAdjustment("Exposure", {"exposure", std::vector<std::byte>(16)}));
    LayerNode& hidden = single->addChild(LayerNode::createRaster("Hidden", w, h));
    hidden.raster()->writeRegion(0, 0, 2, 1, std::vector<float>{1, 1, 1, 1, 1, 1, 1, 1});
    hidden.setVisible(false);
    CHECK(compareComposites(*single, w, h) == 0);

    // Everything else takes the full compositor: the results must still agree.
    auto halfOpacity = makeDocument(2);
    halfOpacity->child(0).setOpacity(0.5f);
    CHECK(compareComposites(*halfOpacity, w, h) == 0);

    auto masked = makeDocument(3);
    masked->child(0).addMask(w, h).writeRegion(0, 0, 3, 1, std::vector<float>{0.0f, 0.5f, 1.0f});
    CHECK(compareComposites(*masked, w, h) == 0);

    auto twoLayers = makeDocument(4);
    LayerNode& top = twoLayers->addChild(LayerNode::createRaster("Top", w, h));
    top.raster()->writeRegion(10, 10, 4, 4, std::vector<float>(4 * 4 * 4, 0.5f));
    top.setBlendMode(BlendMode::MULTIPLY);
    CHECK(compareComposites(*twoLayers, w, h) == 0);

    auto dimRoot = makeDocument(5);
    dimRoot->setOpacity(0.25f);
    CHECK(compareComposites(*dimRoot, w, h) == 0);

    CHECK(compareComposites(*LayerNode::createGroup("Empty"), w, h) == 0);

    // Tiles outside the canvas are rejected on both paths.
    bool threw = false;
    try {
        (void)compositeTileHalf(*single, {2, 0}, w, h);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}

void testHalfRegionWrite() {
    SparseRasterLayer layer(600, 3);
    std::vector<std::uint16_t> source(600 * 3 * 4);
    std::iota(source.begin(), source.end(), std::uint16_t{1});
    layer.writeRegion(0, 0, 600, 3, source);
    CHECK(layer.tileCount() == 2);
    bool exact = true;
    for (std::uint32_t y = 0; y < 3; ++y) {
        for (std::uint32_t x = 0; x < 600; ++x) {
            const PixelTile* tile = layer.getTile(SparseRasterLayer::tileKeyFor(x, y));
            const std::uint16_t* texel = tile->data.data() + tile->index(x % TILE_SIZE, y % TILE_SIZE);
            exact = exact && std::memcmp(texel, &source[(std::size_t{y} * 600 + x) * 4], 8) == 0;
        }
    }
    CHECK(exact);
    CHECK(layer.takeDirtyTiles().size() == 2);

    // Strided, clipped at the right edge.
    std::vector<std::uint16_t> strided(2 * 40, 0x3C00);  // 8 pixels = 32 values, rows 40 apart
    layer.writeRegion(595, 1, 8, 2, strided, 40);
    const PixelTile* edge = layer.getTile({1, 0});
    CHECK(edge->data[edge->index(599 - TILE_SIZE, 2)] == 0x3C00);
    bool threw = false;
    try {
        layer.writeRegion(0, 0, 8, 2, std::span<const std::uint16_t>(strided.data(), 20));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void testParallelFor() {
    std::vector<int> hits(10007, 0);
    detail::parallelFor(hits.size(), 64, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) ++hits[i];
    });
    CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));

    std::atomic<int> calls{0};
    detail::parallelFor(0, 1, [&](std::size_t, std::size_t) { ++calls; });
    CHECK(calls == 0);

    bool rethrown = false;
    try {
        detail::parallelFor(100, 1, [](std::size_t begin, std::size_t) {
            if (begin == 37) throw std::runtime_error("chunk 37");
        });
    } catch (const std::runtime_error& e) {
        rethrown = std::string(e.what()) == "chunk 37";
    }
    CHECK(rethrown);
}

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// One pixel of a composited canvas.
std::array<float, 4> pixelAt(const LayerNode& root, std::uint32_t width, std::uint32_t height, std::uint32_t x,
                             std::uint32_t y) {
    const CompositedTile tile = compositeTileCPU(root, SparseRasterLayer::tileKeyFor(x, y), width, height);
    const float* p = &tile.rgba[(std::size_t{y % TILE_SIZE} * tile.width + x % TILE_SIZE) * 4];
    return {p[0], p[1], p[2], p[3]};
}

void fill(SparseRasterLayer& layer, std::array<float, 4> rgba) {
    std::vector<float> pixels(std::size_t{layer.width()} * layer.height() * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) std::copy(rgba.begin(), rgba.end(), pixels.begin() + static_cast<std::ptrdiff_t>(i));
    layer.writeRegion(0, 0, layer.width(), layer.height(), pixels);
}

void testBlendModes() {
    // Separable modes against the W3C formulas at a few points.
    CHECK(blendChannel(BlendMode::DARKEN, 0.3f, 0.6f) == 0.3f);
    CHECK(blendChannel(BlendMode::LIGHTEN, 0.3f, 0.6f) == 0.6f);
    CHECK(near(blendChannel(BlendMode::DIFFERENCE, 0.3f, 0.6f), 0.3f, 1e-6f));
    CHECK(near(blendChannel(BlendMode::EXCLUSION, 0.5f, 0.5f), 0.5f, 1e-6f));
    CHECK(near(blendChannel(BlendMode::COLOR_BURN, 0.5f, 0.5f), 0.0f, 1e-6f));
    CHECK(near(blendChannel(BlendMode::COLOR_BURN, 1.0f, 0.2f), 1.0f, 1e-6f));
    CHECK(near(blendChannel(BlendMode::HARD_LIGHT, 0.5f, 0.25f), 0.25f, 1e-6f));   // multiply(0.5, 0.5)
    CHECK(near(blendChannel(BlendMode::HARD_LIGHT, 0.5f, 0.75f), 0.75f, 1e-6f));   // screen(0.5, 0.5)
    CHECK(near(blendChannel(BlendMode::SOFT_LIGHT, 0.5f, 0.5f), 0.5f, 1e-6f));     // 50 % grey is neutral
    CHECK(near(blendChannel(BlendMode::SOFT_LIGHT, 0.64f, 1.0f), 0.8f, 1e-6f));    // sqrt branch
    CHECK(near(blendChannel(BlendMode::OVERLAY, 0.25f, 0.5f), 0.25f, 1e-6f));
    CHECK(blendChannel(BlendMode::MULTIPLY, 4.0f, 0.5f) == 2.0f);  // HDR kept
    // Non-separable modes: the luminosity of one colour with the hue of the other.
    const std::array<float, 3> grey{0.5f, 0.5f, 0.5f}, red{1.0f, 0.0f, 0.0f};
    const std::array<float, 3> colour = blendColor(BlendMode::COLOR, grey, red);
    CHECK(near(0.3f * colour[0] + 0.59f * colour[1] + 0.11f * colour[2], 0.5f, 1e-5f));
    CHECK(colour[0] > colour[1] && near(colour[1], colour[2], 1e-6f));
    const std::array<float, 3> lumi = blendColor(BlendMode::LUMINOSITY, red, grey);
    CHECK(near(0.3f * lumi[0] + 0.59f * lumi[1] + 0.11f * lumi[2], 0.5f, 1e-5f) && lumi[0] > lumi[1]);
    const std::array<float, 3> desaturated = blendColor(BlendMode::SATURATION, red, grey);  // grey has no saturation
    CHECK(near(desaturated[0], desaturated[1], 1e-6f) && near(desaturated[1], desaturated[2], 1e-6f));
    const std::array<float, 3> hue = blendColor(BlendMode::HUE, grey, red);  // no saturation to carry the hue
    CHECK(near(hue[0], 0.5f, 1e-6f) && near(hue[1], 0.5f, 1e-6f));

    // Through the compositor: a 50 % grey Difference layer over white.
    const std::uint32_t w = 8, h = 8;
    auto root = LayerNode::createGroup("Document");
    fill(*root->addChild(LayerNode::createRaster("White", w, h)).raster(), {1.0f, 1.0f, 1.0f, 1.0f});
    LayerNode& top = root->addChild(LayerNode::createRaster("Grey", w, h));
    fill(*top.raster(), {0.25f, 0.5f, 0.75f, 1.0f});
    top.setBlendMode(BlendMode::DIFFERENCE);
    const std::array<float, 4> p = pixelAt(*root, w, h, 3, 3);
    CHECK(near(p[0], 0.75f, 1e-3f) && near(p[1], 0.5f, 1e-3f) && near(p[2], 0.25f, 1e-3f) && p[3] == 1.0f);
}

void testTransforms() {
    // Affine helpers.
    LayerTransform t;
    CHECK(t.isIdentity());
    t.translateX = 10.0f;
    t.translateY = -4.0f;
    t.rotation = 90.0f;
    t.scaleX = 2.0f;
    t.pivotX = 5.0f;
    t.pivotY = 5.0f;
    const Affine m = t.matrix();
    const std::array<float, 2> pivot = applyAffine(m, 5.0f, 5.0f);
    CHECK(near(pivot[0], 15.0f, 1e-4f) && near(pivot[1], 1.0f, 1e-4f));  // the pivot only moves by the translation
    const std::array<float, 2> right = applyAffine(m, 6.0f, 5.0f);       // one pixel right, scaled 2x, turned 90 degrees clockwise
    CHECK(near(right[0], 15.0f, 1e-4f) && near(right[1], 3.0f, 1e-4f));
    const std::optional<Affine> inverse = invertAffine(m);
    CHECK(inverse.has_value());
    const std::array<float, 2> back = applyAffine(*inverse, right[0], right[1]);
    CHECK(near(back[0], 6.0f, 1e-4f) && near(back[1], 5.0f, 1e-4f));
    CHECK(!invertAffine({1.0f, 2.0f, 2.0f, 4.0f, 0.0f, 0.0f}));

    // A raster moved by whole pixels lands exactly; the vacated area is empty.
    const std::uint32_t w = 64, h = 32;
    auto root = LayerNode::createGroup("Document");
    LayerNode& layer = root->addChild(LayerNode::createRaster("Dot", w, h));
    layer.raster()->writePixel(10, 10, std::vector<float>{0.8f, 0.4f, 0.2f, 1.0f});
    LayerTransform move;
    move.translateX = 7.0f;
    move.translateY = 3.0f;
    layer.setTransform(move);
    CHECK(near(pixelAt(*root, w, h, 17, 13)[0], 0.8f, 1e-3f) && near(pixelAt(*root, w, h, 17, 13)[3], 1.0f, 1e-6f));
    CHECK(pixelAt(*root, w, h, 10, 10)[3] == 0.0f);
    // Half a pixel: the dot spreads over two pixels at half coverage, colour kept (premultiplied sampling).
    move.translateX = 7.5f;
    layer.setTransform(move);
    const std::array<float, 4> half = pixelAt(*root, w, h, 17, 13);
    CHECK(near(half[3], 0.5f, 1e-3f) && near(half[0], 0.8f, 2e-3f));
    // Singular or non-finite transforms are refused.
    LayerTransform flat = move;
    flat.scaleX = 0.0f;
    layer.setTransform(flat);
    CHECK(layer.transform() == move);
    flat.scaleX = std::nanf("");
    layer.setTransform(flat);
    CHECK(layer.transform() == move);
}

void testVectors() {
    // Flattening: a rectangle keeps its corners; a circle stays on its radius.
    VectorContent rect;
    rect.verbs = {PathVerb::MOVE_TO, PathVerb::LINE_TO, PathVerb::LINE_TO, PathVerb::LINE_TO, PathVerb::CLOSE};
    rect.points = {4.0f, 4.0f, 20.0f, 4.0f, 20.0f, 12.0f, 4.0f, 12.0f};
    rect.fillColor = {0.2f, 0.4f, 0.6f, 1.0f};
    const std::vector<Polyline> lines = flattenPath(rect);
    CHECK(lines.size() == 1 && lines[0].closed && lines[0].points.size() == 4);
    VectorContent circle;
    constexpr float k = 0.5523f, r = 10.0f, c = 16.0f;
    circle.verbs = {PathVerb::MOVE_TO, PathVerb::CUBIC_TO, PathVerb::CUBIC_TO, PathVerb::CUBIC_TO, PathVerb::CUBIC_TO, PathVerb::CLOSE};
    circle.points = {c + r, c,         c + r,     c + k * r, c + k * r, c + r,     c,     c + r,
                     c - k * r, c + r, c - r,     c + k * r, c - r,     c,         c - r, c - k * r,
                     c - k * r, c - r, c,         c - r,     c + k * r, c - r,     c + r, c - k * r, c + r, c};
    float worst = 0.0f;
    const std::vector<Polyline> ring = flattenPath(circle);
    CHECK(ring.size() == 1 && ring[0].closed);
    for (const auto& p : ring[0].points) worst = std::max(worst, std::fabs(std::hypot(p[0] - c, p[1] - c) - r));
    CHECK(worst < 0.05f);
    // Truncated point data stops cleanly.
    VectorContent broken = rect;
    broken.points.resize(5);
    CHECK(flattenPath(broken).size() == 1);

    // Fill coverage: full inside, none outside, half on a half-pixel edge.
    const std::uint32_t w = 32, h = 24;
    auto root = LayerNode::createGroup("Document");
    rect.points = {4.0f, 4.0f, 20.5f, 4.0f, 20.5f, 12.0f, 4.0f, 12.0f};
    LayerNode& shape = root->addChild(LayerNode::createVector("Rect", rect));
    CHECK(near(pixelAt(*root, w, h, 10, 8)[3], 1.0f, 1e-5f) && near(pixelAt(*root, w, h, 10, 8)[2], 0.6f, 1e-5f));
    CHECK(pixelAt(*root, w, h, 2, 2)[3] == 0.0f);
    CHECK(near(pixelAt(*root, w, h, 20, 8)[3], 0.5f, 1e-4f));
    // A transformed shape moves with its layer.
    LayerTransform t;
    t.translateX = 8.0f;
    shape.setTransform(t);
    CHECK(pixelAt(*root, w, h, 6, 8)[3] == 0.0f && near(pixelAt(*root, w, h, 14, 8)[3], 1.0f, 1e-5f));
    // Stroke: an unfilled outline, 2 px wide, covers the edge and not the middle.
    VectorContent outline = rect;
    outline.fillColor[3] = 0.0f;
    outline.strokeColor = {1.0f, 0.0f, 0.0f, 1.0f};
    outline.strokeWidth = 2.0f;
    auto stroked = LayerNode::createGroup("Document");
    stroked->addChild(LayerNode::createVector("Outline", outline));
    CHECK(near(pixelAt(*stroked, w, h, 10, 4)[3], 1.0f, 1e-4f));  // on the top edge
    CHECK(pixelAt(*stroked, w, h, 10, 8)[3] == 0.0f);             // inside
    CHECK(near(pixelAt(*stroked, w, h, 10, 4)[0], 1.0f, 1e-5f));
}

void testAdjustmentLayers() {
    const std::uint32_t w = 16, h = 16;
    auto root = LayerNode::createGroup("Document");
    fill(*root->addChild(LayerNode::createRaster("Grey", w, h)).raster(), {0.18f, 0.18f, 0.18f, 1.0f});
    // +1 EV doubles the pixels below.
    LayerNode& exposure = root->addChild(LayerNode::createAdjustment("Exposure", {"exposure", ExposureNode::pack({1.0f, 0.0f, 0.0f, 0.0f})}));
    CHECK(near(pixelAt(*root, w, h, 5, 5)[0], 0.36f, 1e-3f));
    // Opacity mixes, a mask confines it, and transparency stays transparent.
    exposure.setOpacity(0.5f);
    CHECK(near(pixelAt(*root, w, h, 5, 5)[0], 0.27f, 1e-3f));
    exposure.setOpacity(1.0f);
    exposure.addMask(w, h).writePixel(5, 5, std::vector<float>{0.0f});
    CHECK(near(pixelAt(*root, w, h, 5, 5)[0], 0.18f, 1e-3f) && near(pixelAt(*root, w, h, 6, 5)[0], 0.36f, 1e-3f));
    auto empty = LayerNode::createGroup("Document");
    empty->addChild(LayerNode::createRaster("Nothing", w, h));
    empty->addChild(LayerNode::createAdjustment("Exposure", {"exposure", ExposureNode::pack({2.0f, 0.0f, 0.0f, 0.0f})}));
    CHECK(pixelAt(*empty, w, h, 3, 3)[3] == 0.0f);
    // Inside a group it only reaches the layers of that group.
    auto grouped = LayerNode::createGroup("Document");
    fill(*grouped->addChild(LayerNode::createRaster("Below", w, h)).raster(), {0.18f, 0.18f, 0.18f, 1.0f});
    LayerNode& group = grouped->addChild(LayerNode::createGroup("Group"));
    LayerNode& inner = group.addChild(LayerNode::createRaster("Inner", w, h));
    inner.raster()->writePixel(0, 0, std::vector<float>{0.1f, 0.1f, 0.1f, 1.0f});
    group.addChild(LayerNode::createAdjustment("Exposure", {"exposure", ExposureNode::pack({1.0f, 0.0f, 0.0f, 0.0f})}));
    CHECK(near(pixelAt(*grouped, w, h, 0, 0)[0], 0.2f, 1e-3f));   // the group's own pixel is brightened
    CHECK(near(pixelAt(*grouped, w, h, 5, 5)[0], 0.18f, 1e-3f));  // the layer below the group is not
    // White balance, HSL, curves and grading layers match their CPU references.
    const WhiteBalanceParams warm{3200.0f, 10.0f};
    auto wb = LayerNode::createGroup("Document");
    fill(*wb->addChild(LayerNode::createRaster("Grey", w, h)).raster(), {0.3f, 0.2f, 0.1f, 1.0f});
    wb->addChild(LayerNode::createAdjustment("WB", {"white_balance", packParams(warm)}));
    const Rgb expected = applyWhiteBalance(whiteBalancePush(warm), {0.3f, 0.2f, 0.1f});
    const std::array<float, 4> got = pixelAt(*wb, w, h, 1, 1);
    CHECK(near(got[0], expected[0], 2e-3f) && near(got[1], expected[1], 2e-3f) && near(got[2], expected[2], 2e-3f));
    // Unknown or malformed adjustments are skipped, not fatal.
    wb->addChild(LayerNode::createAdjustment("Bad", {"denoise", std::vector<std::byte>(3)}));
    CHECK(near(pixelAt(*wb, w, h, 1, 1)[0], expected[0], 2e-3f));
}

void testSmartObjects() {
    const std::uint32_t w = 40, h = 30;
    auto root = LayerNode::createGroup("Document");
    LayerNode& object = root->addChild(LayerNode::createSmartObject("Photo", SmartObjectContent{"asset", nullptr}));
    CHECK(!object.contentBounds().has_value());
    CHECK(pixelAt(*root, w, h, 5, 5)[3] == 0.0f);  // nothing until loaded
    auto pixels = std::make_shared<SparseRasterLayer>(10, 10, 4, 0.0f);
    fill(*pixels, {0.5f, 0.25f, 0.125f, 1.0f});
    object.smartObject()->pixels = pixels;
    LayerTransform t;
    t.translateX = 20.0f;
    t.translateY = 10.0f;
    object.setTransform(t);
    CHECK(object.contentBounds().has_value());
    CHECK(near(pixelAt(*root, w, h, 25, 15)[0], 0.5f, 1e-3f) && pixelAt(*root, w, h, 5, 5)[3] == 0.0f);
}

void testDirtyTracking() {
    const std::uint32_t w = 1200, h = 700;  // 3 x 2 tiles
    auto root = LayerNode::createGroup("Document");
    LayerNode& background = root->addChild(LayerNode::createRaster("Background", w, h));
    (void)takeDirtyTiles(*root, w, h);  // the structure change above: everything
    CHECK(takeDirtyTiles(*root, w, h).empty());
    background.raster()->writePixel(600, 100, std::vector<float>{1, 1, 1, 1});
    CHECK((takeDirtyTiles(*root, w, h) == std::vector<TileKey>{{1, 0}}));
    // Property changes redo the whole canvas.
    background.setOpacity(0.5f);
    CHECK(takeDirtyTiles(*root, w, h).size() == 6);
    CHECK(takeDirtyTiles(*root, w, h).empty());
    // A moved layer's pixels land where it is drawn (plus a pixel of
    // bilinear reach into the neighbouring tiles).
    const std::uint32_t big = 2048;  // 4 x 4 tiles
    auto moving = LayerNode::createGroup("Document");
    LayerNode& layer = moving->addChild(LayerNode::createRaster("Layer", big, big));
    LayerTransform t;
    t.translateX = 1024.0f;
    layer.setTransform(t);
    (void)takeDirtyTiles(*moving, big, big);
    layer.raster()->writePixel(100, 1100, std::vector<float>{1, 1, 1, 1});  // tile (0, 2) -> drawn in (2, 2)
    const std::vector<TileKey> moved = takeDirtyTiles(*moving, big, big);
    CHECK(std::find(moved.begin(), moved.end(), TileKey{2, 2}) != moved.end());
    CHECK(std::find(moved.begin(), moved.end(), TileKey{0, 2}) == moved.end());
    CHECK(std::find(moved.begin(), moved.end(), TileKey{2, 0}) == moved.end());
    // Only tiles on the canvas: the layer moved partly off it.
    layer.raster()->writePixel(1500, 100, std::vector<float>{1, 1, 1, 1});  // tile (2, 0) -> drawn in (4, 0), off the canvas
    for (const TileKey& key : takeDirtyTiles(*moving, big, big)) CHECK(key.tx < 4 && key.ty < 4);
    // Content edits through accessors are flagged explicitly.
    root->child(0).markCompositeDirty();
    CHECK(takeDirtyTiles(*root, w, h).size() == 6);
}

}  // namespace

int main() {
    testHalvesToFloats();
    testFloatsToHalves();
    testCompositeTileHalf();
    testHalfRegionWrite();
    testParallelFor();
    testBlendModes();
    testTransforms();
    testVectors();
    testAdjustmentLayers();
    testSmartObjects();
    testDirtyTracking();
    if (g_failures) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "layer stack: all checks passed\n";
    return 0;
}
