// Layer stack tests: bulk FP16 conversion (F16C or scalar) against the scalar
// reference, the pass-through composite against the full compositor, binary16
// region writes and the parallel loop.

#include "layer_stack.hpp"
#include "parallel.hpp"

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

}  // namespace

int main() {
    testHalvesToFloats();
    testFloatsToHalves();
    testCompositeTileHalf();
    testHalfRegionWrite();
    testParallelFor();
    if (g_failures) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "layer stack: all checks passed\n";
    return 0;
}
