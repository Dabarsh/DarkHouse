// Mask engine tests (CPU, no GPU): every component shape, combine modes,
// inversion and opacity, the incremental brush rasterizer, local adjustment
// edits, serialization and sanitizing. The GPU generates the same masks in
// mask_gpu_test.cpp.

#include "mask_engine.hpp"

#include <cmath>
#include <iostream>
#include <limits>
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

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

constexpr std::uint32_t W = 200;
constexpr std::uint32_t H = 100;
const Rgb kGrey{0.18f, 0.18f, 0.18f};

float alphaAt(const MaskComponent& c, float u, float v, const Rgb& rgb = kGrey, float brush = 0.0f) {
    const auto x = static_cast<std::uint32_t>(u * W), y = static_cast<std::uint32_t>(v * H);
    return componentAlpha(c, x, y, W, H, rgb, brush);
}

void testLinear() {
    MaskComponent c;
    c.shape = MaskShape::LINEAR_GRADIENT;
    c.start = {0.25f, 0.5f};
    c.end = {0.75f, 0.5f};
    CHECK(near(alphaAt(c, 0.1f, 0.5f), 1.0f, 1e-6f));   // before the start: full effect
    CHECK(near(alphaAt(c, 0.9f, 0.5f), 0.0f, 1e-6f));   // past the end: none
    CHECK(near(alphaAt(c, 0.4975f, 0.2f), 0.5f, 0.02f));  // halfway, anywhere along the line
    c.invert = true;
    CHECK(near(alphaAt(c, 0.1f, 0.5f), 0.0f, 1e-6f));
}

void testRadial() {
    MaskComponent c;
    c.shape = MaskShape::RADIAL_GRADIENT;
    c.start = {0.5f, 0.5f};
    c.size = {0.1f, 0.4f};  // 20 x 40 pixel radii: a tall ellipse
    c.feather = 0.2f;
    CHECK(near(alphaAt(c, 0.5f, 0.5f), 1.0f, 1e-6f));
    CHECK(near(alphaAt(c, 0.5f, 0.8f), 1.0f, 1e-6f));   // 30 px down: inside the 40 px radius
    CHECK(near(alphaAt(c, 0.7f, 0.5f), 0.0f, 1e-6f));   // 40 px right: outside the 20 px radius
    c.angle = 90.0f;  // rotated a quarter turn: now wide
    CHECK(near(alphaAt(c, 0.5f, 0.8f), 0.0f, 1e-6f));
    CHECK(near(alphaAt(c, 0.65f, 0.5f), 1.0f, 1e-6f));
}

void testRanges() {
    MaskComponent lum;
    lum.shape = MaskShape::LUMINANCE_RANGE;
    lum.low = 0.0f;
    lum.high = 0.4f;
    lum.falloff = 0.05f;
    CHECK(near(alphaAt(lum, 0.5f, 0.5f, {0.02f, 0.02f, 0.02f}), 1.0f, 1e-6f));  // shadows (L ~ 0.27)
    CHECK(near(alphaAt(lum, 0.5f, 0.5f, {0.8f, 0.8f, 0.8f}), 0.0f, 1e-6f));     // highlights

    MaskComponent colour;
    colour.shape = MaskShape::COLOR_RANGE;
    colour.hue = 29.0f;  // red
    colour.hueWidth = 40.0f;
    colour.low = 0.03f;
    CHECK(alphaAt(colour, 0.5f, 0.5f, {0.8f, 0.05f, 0.05f}) > 0.99f);
    CHECK(alphaAt(colour, 0.5f, 0.5f, {0.05f, 0.05f, 0.8f}) == 0.0f);
    CHECK(alphaAt(colour, 0.5f, 0.5f, {0.3f, 0.3f, 0.3f}) == 0.0f);  // grey has no hue

    MaskComponent sky;
    sky.shape = MaskShape::SKY;
    const Rgb blue{0.2f, 0.4f, 0.9f};
    CHECK(alphaAt(sky, 0.5f, 0.1f, blue) > 0.9f);
    CHECK(alphaAt(sky, 0.5f, 0.9f, blue) == 0.0f);                  // blue at the bottom: water, not sky
    CHECK(alphaAt(sky, 0.5f, 0.1f, {0.6f, 0.3f, 0.1f}) == 0.0f);    // orange at the top

    MaskComponent subject;
    subject.shape = MaskShape::SUBJECT;
    CHECK(near(alphaAt(subject, 0.5f, 0.5f), 1.0f, 1e-6f));
    CHECK(alphaAt(subject, 0.02f, 0.02f) == 0.0f);
}

void testCombine() {
    MaskComponent add;
    add.mode = MaskMode::ADD;
    CHECK(near(combineMask(0.0f, add, 0.5f), 0.5f, 1e-6f));
    CHECK(near(combineMask(0.5f, add, 0.5f), 0.75f, 1e-6f));  // union, never above 1
    MaskComponent subtract;
    subtract.mode = MaskMode::SUBTRACT;
    CHECK(near(combineMask(0.8f, subtract, 1.0f), 0.0f, 1e-6f));
    CHECK(near(combineMask(0.8f, subtract, 0.5f), 0.4f, 1e-6f));
    MaskComponent intersect;
    intersect.mode = MaskMode::INTERSECT;
    CHECK(near(combineMask(0.8f, intersect, 0.5f), 0.4f, 1e-6f));
    intersect.opacity = 0.0f;  // a transparent intersect changes nothing
    CHECK(near(combineMask(0.8f, intersect, 0.0f), 0.8f, 1e-6f));
    add.opacity = 0.5f;
    CHECK(near(combineMask(0.0f, add, 1.0f), 0.5f, 1e-6f));

    // Whole mask: a radial minus a linear gradient, then inverted.
    LocalAdjustment mask;
    MaskComponent radial;
    radial.shape = MaskShape::RADIAL_GRADIENT;
    radial.start = {0.5f, 0.5f};
    radial.size = {0.4f, 0.4f};
    radial.feather = 0.01f;
    MaskComponent cut;  // removes everything right of x = 0.7 (fading in from 0.65)
    cut.shape = MaskShape::LINEAR_GRADIENT;
    cut.mode = MaskMode::SUBTRACT;
    cut.start = {0.7f, 0.5f};
    cut.end = {0.65f, 0.5f};
    mask.components = {radial, cut};
    std::vector<float> rgba(std::size_t{W} * H * 4, 0.18f);
    std::vector<float> m = evaluateMask(mask, W, H, rgba);
    CHECK(near(m[std::size_t{50} * W + 100], 1.0f, 1e-6f));  // centre: in the radial, not cut
    CHECK(near(m[std::size_t{50} * W + 150], 0.0f, 1e-6f));  // in the radial but cut
    CHECK(near(m[std::size_t{50} * W + 1], 0.0f, 1e-6f));    // outside the radial
    mask.invert = true;
    m = evaluateMask(mask, W, H, rgba);
    CHECK(near(m[std::size_t{50} * W + 100], 0.0f, 1e-6f));
    CHECK(near(m[std::size_t{50} * W + 150], 1.0f, 1e-6f));
    CHECK(near(m[std::size_t{50} * W + 1], 1.0f, 1e-6f));
}

void testBrush() {
    BrushRaster raster;
    raster.reset(W, H);
    std::vector<BrushDab> dabs{{0.25f, 0.5f, 0.05f, 0.0f, 1.0f, false}};  // hard, 10 px radius
    std::array<std::uint32_t, 4> r = raster.sync(dabs);
    CHECK(r[0] <= 40 && r[2] >= 60 && r[1] <= 40 && r[3] >= 60);
    CHECK(raster.at(50, 50) == 1.0f);
    CHECK(raster.at(70, 50) == 0.0f);

    // A second dab only touches its own rectangle.
    dabs.push_back({0.75f, 0.5f, 0.05f, 0.5f, 0.5f, false});
    r = raster.sync(dabs);
    CHECK(r[0] >= 130 && r[2] <= 171);
    CHECK(near(raster.at(150, 50), 0.5f, 1e-6f));  // flow 0.5 at the centre
    CHECK(raster.at(50, 50) == 1.0f);

    // Erasing, then an incremental result equal to painting everything at once.
    dabs.push_back({0.25f, 0.5f, 0.02f, 0.0f, 1.0f, true});
    raster.sync(dabs);
    CHECK(raster.at(50, 50) == 0.0f);
    CHECK(raster.at(40, 50) == 1.0f);  // outside the 4 px eraser
    BrushRaster fresh;
    fresh.reset(W, H);
    fresh.sync(dabs);
    CHECK(fresh.coverage() == raster.coverage());

    // Undo (a shorter, different list) repaints from scratch.
    dabs.pop_back();
    r = raster.sync(dabs);
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == W && r[3] == H);
    CHECK(raster.at(50, 50) == 1.0f);
    CHECK(raster.sync(dabs)[2] == 0);  // nothing new: empty rectangle
}

void testLocalAdjust() {
    const Rgb c{0.3f, 0.2f, 0.1f};
    const Rgb same = applyLocalAdjust(LocalAdjustParams{}, c);
    CHECK(near(same[0], c[0], 1e-5f) && near(same[1], c[1], 1e-5f) && near(same[2], c[2], 1e-5f));
    LocalAdjustParams brighter;
    brighter.exposure = 1.0f;
    CHECK(near(applyLocalAdjust(brighter, kGrey)[1], 0.36f, 1e-4f));
    LocalAdjustParams grey;
    grey.saturation = -100.0f;
    const Rgb g = applyLocalAdjust(grey, {0.6f, 0.2f, 0.1f});
    CHECK(near(g[0], g[1], 2e-3f) && near(g[1], g[2], 2e-3f));
    LocalAdjustParams warm;
    warm.temperature = 100.0f;
    const Rgb warmed = applyLocalAdjust(warm, kGrey);
    CHECK(warmed[0] > warmed[2] * 1.1f);
}

void testSerialization() {
    LocalAdjustments all;
    for (std::uint32_t s = 0; s < kMaskShapeCount; ++s) {
        LocalAdjustment mask;
        mask.name = "Mask " + std::to_string(s);
        mask.invert = s % 2 == 1;
        mask.amount = 0.25f * static_cast<float>(s % 4);
        mask.params.exposure = 0.5f;
        mask.params.tint = -12.0f;
        MaskComponent c;
        c.shape = static_cast<MaskShape>(s);
        c.mode = static_cast<MaskMode>(s % 3);
        c.hue = 100.0f + static_cast<float>(s);
        if (c.shape == MaskShape::BRUSH) c.dabs = {{0.1f, 0.2f, 0.03f, 0.4f, 0.9f, false}, {0.3f, 0.4f, 0.02f, 0.1f, 1.0f, true}};
        mask.components = {c, c};
        all.masks.push_back(mask);
    }
    const std::vector<std::byte> bytes = serialize(all);
    const LocalAdjustments back = deserializeLocalAdjustments(bytes);
    CHECK(back.masks.size() == kMaskShapeCount);
    CHECK(serialize(back) == bytes);
    CHECK(back.masks[0].components[0].dabs.size() == 2 && back.masks[0].components[0].dabs[1].erase);
    CHECK(back.masks[3].invert && back.masks[3].name == "Mask 3");
    CHECK(deserializeLocalAdjustments({}).masks.empty());

    auto throws = [](std::vector<std::byte> data) {
        try {
            (void)deserializeLocalAdjustments(data);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    std::vector<std::byte> truncated(bytes.begin(), bytes.end() - 3);
    CHECK(throws(truncated));
    std::vector<std::byte> badMagic = bytes;
    badMagic[0] = std::byte{0};
    CHECK(throws(badMagic));
    std::vector<std::byte> trailing = bytes;
    trailing.push_back(std::byte{1});
    CHECK(throws(trailing));

    // Sanitize: values into range, limits enforced.
    LocalAdjustments wild;
    LocalAdjustment mask;
    mask.amount = 7.0f;
    mask.params.exposure = std::numeric_limits<float>::infinity();
    MaskComponent c;
    c.opacity = -1.0f;
    c.angle = -90.0f;
    c.dabs = {{std::numeric_limits<float>::quiet_NaN(), 0.5f, 5.0f, 2.0f, 3.0f, false}};
    mask.components.assign(kMaxComponentsPerMask + 3, c);
    wild.masks.assign(kMaxMasks + 2, mask);
    const LocalAdjustments clean = sanitize(wild);
    CHECK(clean.masks.size() == kMaxMasks);
    CHECK(clean.masks[0].components.size() == kMaxComponentsPerMask);
    CHECK(clean.masks[0].amount == 1.0f && clean.masks[0].params.exposure == 0.0f);
    const MaskComponent& cc = clean.masks[0].components[0];
    CHECK(cc.opacity == 0.0f && cc.angle == 270.0f);
    CHECK(cc.dabs[0].x == 0.5f && cc.dabs[0].radius == 1.0f && cc.dabs[0].feather == 1.0f && cc.dabs[0].flow == 1.0f);
}

void testLayerMask() {
    // The same mask shapes drive a compositing layer's mask.
    auto layer = LayerNode::createRaster("Layer", W, H);
    SparseRasterLayer& target = layer->addMask(W, H);
    LocalAdjustment mask;
    MaskComponent linear;
    linear.shape = MaskShape::LINEAR_GRADIENT;
    linear.start = {0.0f, 0.5f};
    linear.end = {1.0f, 0.5f};
    mask.components = {linear};
    const std::vector<float> rgba(std::size_t{W} * H * 4, 0.18f);
    writeLayerMask(mask, target, rgba);
    float left[1], right[1];
    target.readPixel(5, 50, left);
    target.readPixel(195, 50, right);
    CHECK(left[0] > 0.99f && right[0] < 0.01f);
    CHECK(target.hasDirtyTiles());
    bool threw = false;
    try {
        writeLayerMask(mask, *layer->raster(), rgba);  // four channels: not a mask
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Geometric masks need no image; ranges do.
    CHECK(!readsImage(mask));
    const std::vector<float> withImage = evaluateMask(mask, W, H, rgba);
    const std::vector<float> withoutImage = evaluateMask(mask, W, H, {});
    CHECK(withImage == withoutImage);
    MaskComponent range;
    range.shape = MaskShape::LUMINANCE_RANGE;
    mask.components.push_back(range);
    CHECK(readsImage(mask));
    threw = false;
    try {
        (void)evaluateMask(mask, W, H, {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    testLinear();
    testRadial();
    testRanges();
    testCombine();
    testBrush();
    testLocalAdjust();
    testSerialization();
    testLayerMask();
    if (g_failures) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "mask engine: all checks passed\n";
    return 0;
}
