// Colour adjustment tests (CPU reference, no GPU): Oklab, white balance,
// HSL colour mixer, colour grading, tone curves, parameter packing and the
// canonical develop-stack order. The GPU nodes are checked against these references
// in color_gpu_test.cpp.

#include "color_adjust.hpp"
#include "develop_stack.hpp"
#include "render_pipeline.hpp"
#include "tone_curve.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
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
bool nearRgb(const Rgb& a, const Rgb& b, float tolerance) {
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance);
}
float chromaOf(const Rgb& rgb) {
    const Rgb lab = linearSrgbToOklab(rgb);
    return std::hypot(lab[1], lab[2]);
}
float hueOf(const Rgb& rgb) {
    const Rgb lab = linearSrgbToOklab(rgb);
    float h = std::atan2(lab[2], lab[1]) * 180.0f / 3.14159265f;
    return h < 0.0f ? h + 360.0f : h;
}
float lightnessOf(const Rgb& rgb) { return linearSrgbToOklab(rgb)[0]; }

// Linear sRGB of an illuminant's white (Y = 1).
Rgb illuminantRgb(float temperature, float tint) {
    const std::array<float, 2> xy = illuminantChromaticity(temperature, tint);
    const float X = xy[0] / xy[1], Y = 1.0f, Z = (1.0f - xy[0] - xy[1]) / xy[1];
    return {3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z, -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
            0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z};
}

void testOklab() {
    const Rgb white = linearSrgbToOklab({1.0f, 1.0f, 1.0f});
    CHECK(near(white[0], 1.0f, 1e-4f) && near(white[1], 0.0f, 1e-4f) && near(white[2], 0.0f, 1e-4f));
    for (const Rgb& c : std::vector<Rgb>{{0.2f, 0.5f, 0.9f}, {1.0f, 0.0f, 0.0f}, {0.01f, 0.02f, 0.005f}, {3.0f, 2.0f, 1.0f}}) {
        CHECK(nearRgb(oklabToLinearSrgb(linearSrgbToOklab(c)), c, 2e-5f * std::max(1.0f, c[0])));
    }
    // The HSL band centres are the hues of their pure sRGB colours.
    CHECK(near(hueOf({1.0f, 0.0f, 0.0f}), kHslBandHues[0], 0.2f));
    CHECK(near(hueOf({0.0f, 0.0f, 1.0f}), kHslBandHues[5], 0.2f));
}

void testWhiteBalance() {
    // Identity at the reference.
    const WhiteBalancePush identity = whiteBalancePush(WhiteBalanceParams{});
    CHECK(isIdentity(WhiteBalanceParams{}));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) CHECK(identity.rows[r][c] == (r == c ? 1.0f : 0.0f));
    }
    // The locus: 2856 K is CIE illuminant A; D65 lies at 6504 K, Duv +0.0032
    // (tint +21.3 at 1.5e-4 Duv per unit).
    const std::array<float, 2> a = illuminantChromaticity(2856.0f, 0.0f);
    CHECK(near(a[0], 0.4476f, 0.001f) && near(a[1], 0.4074f, 0.001f));
    const std::array<float, 2> d65 = illuminantChromaticity(6504.0f, 0.0032f / 1.5e-4f);
    CHECK(near(d65[0], 0.3127f, 0.0005f) && near(d65[1], 0.3290f, 0.0005f));

    const Rgb grey{0.18f, 0.18f, 0.18f};
    // Lower temperature = the light was warm = the correction cools the photo.
    const Rgb cooled = applyWhiteBalance(whiteBalancePush({3200.0f, 0.0f}), grey);
    CHECK(cooled[2] > cooled[0] * 1.3f);
    const Rgb warmed = applyWhiteBalance(whiteBalancePush({9000.0f, 0.0f}), grey);
    CHECK(warmed[0] > warmed[2] * 1.1f);
    // Positive tint = magenta result (less green than red and blue).
    const Rgb magenta = applyWhiteBalance(whiteBalancePush({kReferenceTemperature, 120.0f}), grey);
    CHECK(magenta[1] < magenta[0] && magenta[1] < magenta[2]);
    const Rgb green = applyWhiteBalance(whiteBalancePush({kReferenceTemperature, -120.0f}), grey);
    CHECK(green[1] > green[0] && green[1] > green[2]);

    // Chromatic adaptation: the illuminant's own white becomes the reference white.
    for (const WhiteBalanceParams& p : {WhiteBalanceParams{3000.0f, 0.0f}, WhiteBalanceParams{7500.0f, 40.0f}}) {
        const Rgb source = illuminantRgb(p.temperature, p.tint);
        const Rgb adapted = applyWhiteBalance(whiteBalancePush(p), source);
        const Rgb reference = illuminantRgb(kReferenceTemperature, 0.0f);
        const float scale = reference[1] / std::max(adapted[1], 1e-6f);
        CHECK(nearRgb({adapted[0] * scale, adapted[1] * scale, adapted[2] * scale}, reference, 2e-3f));
    }
    // Sanitize: out-of-range and non-finite values.
    const WhiteBalanceParams wild = sanitize(WhiteBalanceParams{std::numeric_limits<float>::quiet_NaN(), 1000.0f});
    CHECK(wild.temperature == kReferenceTemperature && wild.tint == kTintRange);
}

void testHsl() {
    const std::vector<Rgb> samples{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.9f, 0.9f, 0.1f}, {0.1f, 0.8f, 0.2f},
                                   {0.4f, 0.1f, 0.6f}, {0.2f, 0.2f, 0.2f}, {0.0f, 0.0f, 0.0f}, {2.5f, 1.2f, 0.3f}};
    // Identity.
    for (const Rgb& c : samples) CHECK(nearRgb(applyHsl(HslParams{}, c), c, 3e-5f * std::max(1.0f, c[0])));

    // Red saturation -100: red goes grey, blue is untouched, greys stay grey.
    HslParams desaturateRed;
    desaturateRed.saturation[0] = -100.0f;
    CHECK(chromaOf(applyHsl(desaturateRed, {1.0f, 0.0f, 0.0f})) < 1e-3f);
    CHECK(nearRgb(applyHsl(desaturateRed, {0.0f, 0.0f, 1.0f}), {0.0f, 0.0f, 1.0f}, 1e-4f));
    CHECK(nearRgb(applyHsl(desaturateRed, {0.2f, 0.2f, 0.2f}), {0.2f, 0.2f, 0.2f}, 1e-4f));

    // Hue +100 on the blue band turns a band-centre blue by +30 degrees.
    HslParams shiftBlue;
    shiftBlue.hue[5] = 100.0f;
    const Rgb blue = oklabToLinearSrgb({0.5f, 0.1f * std::cos(kHslBandHues[5] * 3.14159265f / 180.0f),
                                        0.1f * std::sin(kHslBandHues[5] * 3.14159265f / 180.0f)});
    CHECK(near(hueOf(applyHsl(shiftBlue, blue)), kHslBandHues[5] + kHslMaxHueShift, 0.5f));

    // Luminance +100 on green: one stop brighter in lightness terms.
    HslParams brightenGreen;
    brightenGreen.luminance[3] = 100.0f;
    const Rgb green = oklabToLinearSrgb({0.6f, 0.15f * std::cos(kHslBandHues[3] * 3.14159265f / 180.0f),
                                         0.15f * std::sin(kHslBandHues[3] * 3.14159265f / 180.0f)});
    CHECK(near(lightnessOf(applyHsl(brightenGreen, green)), 0.6f * std::cbrt(2.0f), 2e-3f));

    // Between two band centres both bands contribute, smoothly.
    HslParams orangeOnly;
    orangeOnly.saturation[1] = -100.0f;
    const float midHue = 0.5f * (kHslBandHues[0] + kHslBandHues[1]);
    const Rgb between = oklabToLinearSrgb({0.65f, 0.12f * std::cos(midHue * 3.14159265f / 180.0f),
                                           0.12f * std::sin(midHue * 3.14159265f / 180.0f)});
    CHECK(near(chromaOf(applyHsl(orangeOnly, between)), 0.06f, 2e-3f));  // half of the band's effect

    HslParams wild;
    wild.hue[2] = 500.0f;
    wild.luminance[4] = std::numeric_limits<float>::infinity();
    const HslParams clean = sanitize(wild);
    CHECK(clean.hue[2] == 100.0f && clean.luminance[4] == 0.0f);
}

void testColorGrading() {
    const ColorGradingPush neutral = colorGradingPush(ColorGradingParams{});
    CHECK(isIdentity(ColorGradingParams{}));
    for (const Rgb& c : std::vector<Rgb>{{0.5f, 0.2f, 0.1f}, {0.02f, 0.02f, 0.03f}, {0.9f, 0.9f, 0.95f}}) {
        CHECK(nearRgb(applyColorGrading(neutral, c), c, 3e-5f));
    }

    // Zone weights: a partition of unity, shadows at black, highlights at white.
    for (float balance : {-100.0f, 0.0f, 100.0f}) {
        for (float blending : {0.0f, 50.0f, 100.0f}) {
            ColorGradingParams p;
            p.balance = balance;
            p.blending = blending;
            const ColorGradingPush push = colorGradingPush(p);
            for (float L = 0.0f; L <= 1.0f; L += 0.05f) {
                const std::array<float, 3> w = gradingZoneWeights(push, L);
                CHECK(near(w[0] + w[1] + w[2], 1.0f, 1e-5f) && w[0] >= -1e-6f && w[1] >= -1e-6f && w[2] >= -1e-6f);
            }
            CHECK(near(gradingZoneWeights(push, 0.0f)[0], 1.0f, 1e-6f));
            CHECK(near(gradingZoneWeights(push, 1.0f)[2], 1.0f, 1e-6f));
        }
    }
    // Positive balance moves the split down: a midtone gets more highlight weight.
    ColorGradingParams lowSplit;
    lowSplit.balance = 100.0f;
    CHECK(gradingZoneWeights(colorGradingPush(lowSplit), 0.6f)[2] > gradingZoneWeights(neutral, 0.6f)[2]);

    // A blue shadow wheel tints dark greys, not bright ones; black stays black.
    ColorGradingParams blueShadows;
    blueShadows.shadows = {264.0f, 100.0f, 0.0f};
    const ColorGradingPush push = colorGradingPush(blueShadows);
    const Rgb dark = applyColorGrading(push, {0.02f, 0.02f, 0.02f});
    const Rgb bright = applyColorGrading(push, {0.8f, 0.8f, 0.8f});
    CHECK(dark[2] > dark[0] * 1.2f);
    CHECK(chromaOf(bright) < 0.005f);
    CHECK(nearRgb(applyColorGrading(push, {0.0f, 0.0f, 0.0f}), {0.0f, 0.0f, 0.0f}, 1e-6f));
    CHECK(near(hueOf(dark), 264.0f, 3.0f));

    // The global wheel reaches every tone; luminance brightens by a stop at +100.
    ColorGradingParams warmGlobal;
    warmGlobal.global = {60.0f, 50.0f, 100.0f};
    const Rgb warmed = applyColorGrading(colorGradingPush(warmGlobal), {0.4f, 0.4f, 0.4f});
    CHECK(chromaOf(warmed) > 0.02f);
    CHECK(near(lightnessOf(warmed), lightnessOf({0.4f, 0.4f, 0.4f}) * std::cbrt(2.0f), 3e-3f));

    // Presence: saturation -100 removes colour; vibrance favours muted colours.
    ColorGradingParams grey;
    grey.saturation = -100.0f;
    CHECK(chromaOf(applyColorGrading(colorGradingPush(grey), {0.8f, 0.2f, 0.1f})) < 1e-3f);
    ColorGradingParams vibrant;
    vibrant.vibrance = 100.0f;
    const ColorGradingPush vibrantPush = colorGradingPush(vibrant);
    const Rgb muted{0.30f, 0.25f, 0.22f}, vivid{0.9f, 0.05f, 0.02f};
    const float mutedGain = chromaOf(applyColorGrading(vibrantPush, muted)) / chromaOf(muted);
    const float vividGain = chromaOf(applyColorGrading(vibrantPush, vivid)) / chromaOf(vivid);
    CHECK(mutedGain > 1.8f && vividGain < 1.3f);
}

void testPacking() {
    ColorGradingParams p;
    p.highlights = {200.0f, 30.0f, -10.0f};
    p.balance = 25.0f;
    const std::vector<std::byte> bytes = packParams(p);
    CHECK(bytes.size() == sizeof(ColorGradingParams));
    const ColorGradingParams back = unpackParams<ColorGradingParams>(bytes, "test");
    CHECK(back.highlights.hue == 200.0f && back.balance == 25.0f);
    bool threw = false;
    try {
        (void)unpackParams<HslParams>(bytes, "test");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    // ExposureNode::pack uses the same convention.
    CHECK(packParams(ExposureParams{1.0f, 0.0f, 0.0f, 0.0f}) == ExposureNode::pack(ExposureParams{1.0f, 0.0f, 0.0f, 0.0f}));
}

Curve makeCurve(std::initializer_list<CurvePoint> points) {
    Curve curve;
    for (const CurvePoint& p : points) curve.points[curve.count++] = p;
    return curve;
}

void testToneCurve() {
    // Transfer curve round trip, including past white.
    for (float v : {0.0f, 0.001f, 0.0031308f, 0.18f, 0.5f, 1.0f, 4.0f, 100.0f}) {
        CHECK(near(decodeTransfer(encodeTransfer(v)), v, 1e-5f * std::max(1.0f, v)));
    }
    CHECK(near(encodeTransfer(0.18f), 0.4614f, 1e-3f));

    // Identity: no points, or the diagonal through both corners.
    CHECK(isIdentity(Curve{}));
    CHECK(sanitize(makeCurve({{0.0f, 0.0f}, {1.0f, 1.0f}})).count == 0);
    CHECK(sanitize(makeCurve({{0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 1.0f}})).count == 0);
    CHECK(!isIdentity(makeCurve({{0.2f, 0.2f}, {0.8f, 0.8f}})));  // flat outside: clips

    // Sanitize sorts, clamps, merges near-duplicates and drops NaN.
    const Curve messy = sanitize(makeCurve({{1.2f, 0.9f}, {0.5f, 0.6f}, {0.501f, 0.1f}, {-0.3f, 0.1f},
                                            {std::numeric_limits<float>::quiet_NaN(), 0.5f}}));
    CHECK(messy.count == 3);
    CHECK(messy.points[0] == (CurvePoint{0.0f, 0.1f}) && messy.points[1] == (CurvePoint{0.5f, 0.6f}) &&
          messy.points[2] == (CurvePoint{1.0f, 0.9f}));

    // The spline passes through its points, is monotone for rising points and
    // does not overshoot them.
    const Curve s = curvePreset(CurvePreset::STRONG_CONTRAST);
    for (std::uint32_t i = 0; i < s.count; ++i) CHECK(near(evaluateCurve(s, s.points[i].x), s.points[i].y, 1e-5f));
    float previous = -1.0f;
    bool monotone = true;
    for (int i = 0; i <= 1000; ++i) {
        const float y = evaluateCurve(s, static_cast<float>(i) / 1000.0f);
        monotone = monotone && y >= previous - 1e-6f;
        previous = y;
    }
    CHECK(monotone);
    const Curve peak = makeCurve({{0.0f, 0.0f}, {0.5f, 0.9f}, {1.0f, 0.2f}});  // not monotone: no overshoot
    float highest = 0.0f;
    for (int i = 0; i <= 1000; ++i) highest = std::max(highest, evaluateCurve(peak, static_cast<float>(i) / 1000.0f));
    CHECK(highest <= 0.9f + 1e-5f);
    CHECK(near(evaluateCurve(makeCurve({{0.2f, 0.3f}, {0.8f, 0.7f}}), 0.05f), 0.3f, 1e-6f));  // flat before the first point

    // Baked tables: identity passes colours through, the contrast curve
    // darkens shadows and brightens highlights, and HDR keeps rising.
    const ToneCurveTables identity = bakeToneCurve(ToneCurveParams{});
    CHECK(identity.activeMask == 0);
    for (const Rgb& c : {Rgb{0.18f, 0.18f, 0.18f}, Rgb{0.9f, 0.02f, 0.4f}, Rgb{3.0f, 2.0f, 1.0f}}) {
        CHECK(nearRgb(applyToneCurve(identity, c), c, 1e-6f * 4.0f));
    }
    ToneCurveParams contrast;
    contrast.curves[0] = s;
    const ToneCurveTables baked = bakeToneCurve(contrast);
    CHECK(baked.activeMask == 1u);
    CHECK(applyToneCurve(baked, {0.05f, 0.05f, 0.05f})[0] < 0.05f);
    CHECK(applyToneCurve(baked, {0.6f, 0.6f, 0.6f})[0] > 0.6f);
    const float white = applyToneCurve(baked, {1.0f, 1.0f, 1.0f})[0];
    CHECK(near(white, 1.0f, 1e-4f));
    CHECK(applyToneCurve(baked, {4.0f, 4.0f, 4.0f})[0] > white);

    // Channel curves act on their channel only, after the composite curve.
    ToneCurveParams warm;
    warm.curves[static_cast<std::size_t>(CurveChannel::RED)] = makeCurve({{0.0f, 0.0f}, {0.5f, 0.6f}, {1.0f, 1.0f}});
    warm.curves[static_cast<std::size_t>(CurveChannel::BLUE)] = makeCurve({{0.0f, 0.0f}, {0.5f, 0.4f}, {1.0f, 1.0f}});
    const Rgb grey = applyToneCurve(bakeToneCurve(warm), {0.2f, 0.2f, 0.2f});
    CHECK(grey[0] > 0.2f && near(grey[1], 0.2f, 1e-5f) && grey[2] < 0.2f);

    // Serialized as raw bytes, like the other colour nodes.
    const std::vector<std::byte> bytes = packParams(warm);
    CHECK(bytes.size() == 528);
    const ToneCurveParams back = unpackParams<ToneCurveParams>(bytes, "test");
    CHECK(back.curves[1].count == 3 && back.curves[1].points[1] == (CurvePoint{0.5f, 0.6f}));
}

std::vector<std::string> types(const std::vector<EditNodeRecord>& stack) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        out.push_back(stack[i].nodeType);
        if (stack[i].nodeIndex != static_cast<std::int32_t>(i)) out.push_back("<bad index>");
    }
    return out;
}

void testDevelopOrder() {
    std::vector<EditNodeRecord> stack{{0, "exposure", {}}};
    stack = withDevelopNode(stack, {0, "color_grading", {}});
    stack = withDevelopNode(stack, {0, "white_balance", {}});
    stack = withDevelopNode(stack, {0, "hsl", {}});
    stack = withDevelopNode(stack, {0, "denoise", {}});
    stack = withDevelopNode(stack, {0, "something_else", {}});
    stack = withDevelopNode(stack, {0, "tone_curve", {}});
    CHECK((types(stack) == std::vector<std::string>{"denoise", "white_balance", "exposure", "tone_curve", "hsl",
                                                    "color_grading", "something_else"}));
    CHECK(findDevelopNode(stack, "hsl") == std::optional<std::size_t>(4));
    CHECK(!findDevelopNode(stack, "missing"));
    CHECK(developStage("denoise") < developStage("exposure"));
}

}  // namespace

int main() {
    testOklab();
    testWhiteBalance();
    testHsl();
    testColorGrading();
    testToneCurve();
    testPacking();
    testDevelopOrder();
    if (g_failures) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "colour adjustments: all checks passed\n";
    return 0;
}
