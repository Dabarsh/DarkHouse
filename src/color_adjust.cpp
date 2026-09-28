#include "color_adjust.hpp"

#include <algorithm>
#include <cmath>

namespace darkhouse {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float finiteOr(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }
float clampFinite(float value, float lo, float hi, float fallback) noexcept {
    return std::clamp(finiteOr(value, fallback), lo, hi);
}

float smoothstep(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float signedCbrt(float v) noexcept { return std::copysign(std::cbrt(std::fabs(v)), v); }

// As shaders/color_common.glsl storable(): negative (out-of-gamut) light to 0, half-float range.
Rgb storable(const Rgb& rgb) noexcept {
    constexpr float kHalfMax = 65504.0f;
    return {std::clamp(rgb[0], 0.0f, kHalfMax), std::clamp(rgb[1], 0.0f, kHalfMax), std::clamp(rgb[2], 0.0f, kHalfMax)};
}

// --- White balance helpers (double precision: the matrix is computed once) ---

using Mat3 = std::array<std::array<double, 3>, 3>;
using Vec3 = std::array<double, 3>;

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k) out[r][c] += a[r][k] * b[k][c];
        }
    }
    return out;
}

Vec3 multiply(const Mat3& m, const Vec3& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

Mat3 inverse(const Mat3& m) {
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    const double k = 1.0 / det;
    Mat3 out{};
    out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * k;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * k;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * k;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
    return out;
}

// Linear Rec.709 / sRGB (D65) to CIE XYZ.
constexpr Mat3 kRgbToXyz{{{0.4124564, 0.3575761, 0.1804375},
                          {0.2126729, 0.7151522, 0.0721750},
                          {0.0193339, 0.1191920, 0.9503041}}};
// Bradford cone response.
constexpr Mat3 kBradford{{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};

// Planckian locus in CIE 1931 xy (Kang et al. 2002 cubic fits, 1667..25000 K).
std::array<double, 2> planckianXy(double t) {
    t = std::clamp(t, 1667.0, 25000.0);
    const double t2 = t * t, t3 = t2 * t;
    const double x = t <= 4000.0 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                                 : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    const double x2 = x * x, x3 = x2 * x;
    double y;
    if (t <= 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }
    return {x, y};
}

std::array<double, 2> xyToUv(const std::array<double, 2>& xy) {
    const double d = -2.0 * xy[0] + 12.0 * xy[1] + 3.0;
    return {4.0 * xy[0] / d, 6.0 * xy[1] / d};
}

std::array<double, 2> uvToXy(const std::array<double, 2>& uv) {
    const double d = 2.0 * uv[0] - 8.0 * uv[1] + 4.0;
    return {3.0 * uv[0] / d, 2.0 * uv[1] / d};
}

// Tint as a distance from the locus in CIE 1960 uv (Duv) per slider unit:
// +-150 reaches +-0.0225, about the green cast of a fluorescent tube.
constexpr double kDuvPerTint = 1.5e-4;

std::array<double, 2> illuminantXy(double temperature, double tint) {
    const std::array<double, 2> uv = xyToUv(planckianXy(temperature));
    // Unit normal of the locus, pointing to the green side (increasing v).
    const std::array<double, 2> before = xyToUv(planckianXy(temperature - 1.0));
    const std::array<double, 2> after = xyToUv(planckianXy(temperature + 1.0));
    double nu = -(after[1] - before[1]);
    double nv = after[0] - before[0];
    const double length = std::hypot(nu, nv);
    nu /= length;
    nv /= length;
    if (nv < 0.0) {
        nu = -nu;
        nv = -nv;
    }
    // A positive tint (magenta result) means the light was green: the
    // illuminant sits above the locus, and adapting from it removes the green.
    const double duv = tint * kDuvPerTint;
    return uvToXy({uv[0] + nu * duv, uv[1] + nv * duv});
}

Vec3 whiteXyz(const std::array<double, 2>& xy) { return {xy[0] / xy[1], 1.0, (1.0 - xy[0] - xy[1]) / xy[1]}; }

}  // namespace

// -----------------------------------------------------------------------------
// Oklab
// -----------------------------------------------------------------------------

Rgb linearSrgbToOklab(const Rgb& c) noexcept {
    const float l = signedCbrt(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = signedCbrt(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = signedCbrt(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

Rgb oklabToLinearSrgb(const Rgb& lab) noexcept {
    const float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    const float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    const float s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    const float l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
    return {4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3,
            -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3,
            -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3};
}

// -----------------------------------------------------------------------------
// White balance
// -----------------------------------------------------------------------------

WhiteBalanceParams sanitize(const WhiteBalanceParams& params) noexcept {
    return {clampFinite(params.temperature, kMinTemperature, kMaxTemperature, kReferenceTemperature),
            clampFinite(params.tint, -kTintRange, kTintRange, 0.0f)};
}

bool isIdentity(const WhiteBalanceParams& params) noexcept {
    const WhiteBalanceParams p = sanitize(params);
    return p.temperature == kReferenceTemperature && p.tint == 0.0f;
}

std::array<float, 2> illuminantChromaticity(float temperature, float tint) noexcept {
    const std::array<double, 2> xy = illuminantXy(temperature, tint);
    return {static_cast<float>(xy[0]), static_cast<float>(xy[1])};
}

WhiteBalancePush whiteBalancePush(const WhiteBalanceParams& params) noexcept {
    const WhiteBalanceParams p = sanitize(params);
    Mat3 matrix{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    if (!isIdentity(p)) {
        // Adapt from the scene illuminant to the reference white (Bradford, von Kries in cone space).
        const Vec3 source = multiply(kBradford, whiteXyz(illuminantXy(p.temperature, p.tint)));
        const Vec3 target = multiply(kBradford, whiteXyz(illuminantXy(kReferenceTemperature, 0.0)));
        const Mat3 gains{{{target[0] / source[0], 0, 0}, {0, target[1] / source[1], 0}, {0, 0, target[2] / source[2]}}};
        const Mat3 adapt = multiply(inverse(kBradford), multiply(gains, kBradford));
        matrix = multiply(inverse(kRgbToXyz), multiply(adapt, kRgbToXyz));
    }
    WhiteBalancePush push{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) push.rows[r][c] = static_cast<float>(matrix[r][c]);
    }
    return push;
}

Rgb applyWhiteBalance(const WhiteBalancePush& push, const Rgb& rgb) noexcept {
    Rgb out{};
    for (int r = 0; r < 3; ++r) out[r] = push.rows[r][0] * rgb[0] + push.rows[r][1] * rgb[1] + push.rows[r][2] * rgb[2];
    return storable(out);
}

// -----------------------------------------------------------------------------
// HSL
// -----------------------------------------------------------------------------

HslParams sanitize(const HslParams& params) noexcept {
    HslParams p;
    for (std::size_t i = 0; i < kHslBands; ++i) {
        p.hue[i] = clampFinite(params.hue[i], -100.0f, 100.0f, 0.0f);
        p.saturation[i] = clampFinite(params.saturation[i], -100.0f, 100.0f, 0.0f);
        p.luminance[i] = clampFinite(params.luminance[i], -100.0f, 100.0f, 0.0f);
    }
    return p;
}

bool isIdentity(const HslParams& params) noexcept {
    const HslParams p = sanitize(params);
    for (std::size_t i = 0; i < kHslBands; ++i) {
        if (p.hue[i] != 0.0f || p.saturation[i] != 0.0f || p.luminance[i] != 0.0f) return false;
    }
    return true;
}

Rgb applyHsl(const HslParams& p, const Rgb& rgb) noexcept {
    const Rgb lab = linearSrgbToOklab(rgb);
    const float chroma = std::hypot(lab[1], lab[2]);
    if (!(chroma > 0.0f)) return storable(rgb);  // no hue (and atan(0, 0) is undefined on the GPU)
    float hue = std::atan2(lab[2], lab[1]) * (180.0f / kPi);
    if (hue < 0.0f) hue += 360.0f;

    // The two band centres around the hue, and a smooth crossfade between them.
    std::size_t i = kHslBands - 1;
    for (std::size_t k = 0; k < kHslBands; ++k) {
        if (hue >= kHslBandHues[k]) i = k;
    }
    const std::size_t j = (i + 1) % kHslBands;
    const float h0 = kHslBandHues[i];
    const float h1 = j == 0 ? kHslBandHues[0] + 360.0f : kHslBandHues[j];
    const float h = hue < h0 ? hue + 360.0f : hue;
    const float blend = smoothstep(0.0f, 1.0f, (h - h0) / (h1 - h0));
    const float wi = 1.0f - blend;
    const float wj = blend;

    // Hue is undefined for greys: adjustments fade in with chroma.
    const float colourful = smoothstep(0.0f, 0.05f, chroma);
    const float hueShift = (wi * p.hue[i] + wj * p.hue[j]) * 0.01f * kHslMaxHueShift * colourful;
    const float saturation = (wi * p.saturation[i] + wj * p.saturation[j]) * 0.01f * colourful;
    const float stops = (wi * p.luminance[i] + wj * p.luminance[j]) * 0.01f * colourful;

    const float newHue = (hue + hueShift) * (kPi / 180.0f);
    const float newChroma = chroma * std::max(0.0f, 1.0f + saturation);
    const float lightness = lab[0] * std::exp2(stops / 3.0f);  // L ~ Y^(1/3): one stop per 3 units of log2
    return storable(oklabToLinearSrgb({lightness, newChroma * std::cos(newHue), newChroma * std::sin(newHue)}));
}

// -----------------------------------------------------------------------------
// Colour grading
// -----------------------------------------------------------------------------

ColorGradingParams sanitize(const ColorGradingParams& params) noexcept {
    auto wheel = [](const ColorWheel& w) {
        float hue = std::fmod(finiteOr(w.hue, 0.0f), 360.0f);
        if (hue < 0.0f) hue += 360.0f;
        return ColorWheel{hue, clampFinite(w.saturation, 0.0f, 100.0f, 0.0f), clampFinite(w.luminance, -100.0f, 100.0f, 0.0f)};
    };
    ColorGradingParams p;
    p.vibrance = clampFinite(params.vibrance, -100.0f, 100.0f, 0.0f);
    p.saturation = clampFinite(params.saturation, -100.0f, 100.0f, 0.0f);
    p.shadows = wheel(params.shadows);
    p.midtones = wheel(params.midtones);
    p.highlights = wheel(params.highlights);
    p.global = wheel(params.global);
    p.blending = clampFinite(params.blending, 0.0f, 100.0f, 50.0f);
    p.balance = clampFinite(params.balance, -100.0f, 100.0f, 0.0f);
    return p;
}

bool isIdentity(const ColorGradingParams& params) noexcept {
    const ColorGradingParams p = sanitize(params);
    auto neutral = [](const ColorWheel& w) { return w.saturation == 0.0f && w.luminance == 0.0f; };
    return p.vibrance == 0.0f && p.saturation == 0.0f && neutral(p.shadows) && neutral(p.midtones) &&
           neutral(p.highlights) && neutral(p.global);
}

ColorGradingPush colorGradingPush(const ColorGradingParams& params) noexcept {
    const ColorGradingParams p = sanitize(params);
    ColorGradingPush push{};
    push.saturationScale = 1.0f + p.saturation * 0.01f;
    push.vibrance = p.vibrance * 0.01f;
    push.pivot = 0.5f - p.balance * 0.01f * 0.25f;
    push.blending = std::clamp(p.blending * 0.01f, 0.01f, 1.0f);
    const ColorWheel* wheels[4] = {&p.shadows, &p.midtones, &p.highlights, &p.global};
    for (int z = 0; z < 4; ++z) {
        const float angle = wheels[z]->hue * (kPi / 180.0f);
        const float chroma = wheels[z]->saturation * 0.01f * kGradingMaxChroma;
        push.wheels[z][0] = chroma * std::cos(angle);
        push.wheels[z][1] = chroma * std::sin(angle);
        push.wheels[z][2] = wheels[z]->luminance * 0.01f;
        push.wheels[z][3] = 0.0f;
    }
    return push;
}

std::array<float, 3> gradingZoneWeights(const ColorGradingPush& push, float lightness) noexcept {
    const float t = std::clamp(lightness, 0.0f, 1.0f);
    const float m = push.pivot;
    const float c1 = 0.5f * m, h1 = 0.5f * m * push.blending;
    const float c2 = 0.5f * (1.0f + m), h2 = 0.5f * (1.0f - m) * push.blending;
    const float toMid = smoothstep(c1 - h1, c1 + h1, t);
    const float toHigh = smoothstep(c2 - h2, c2 + h2, t);
    return {1.0f - toMid, toMid - toHigh, toHigh};
}

Rgb applyColorGrading(const ColorGradingPush& push, const Rgb& rgb) noexcept {
    Rgb lab = linearSrgbToOklab(rgb);
    // Presence: saturation scales all chroma; vibrance mostly the muted colours.
    const float chroma = std::hypot(lab[1], lab[2]);
    const float scale = push.saturationScale * std::max(0.0f, 1.0f + push.vibrance * (1.0f - smoothstep(0.0f, 0.2f, chroma)));
    lab[1] *= scale;
    lab[2] *= scale;

    // Wheels: chroma offsets and lightness per tonal zone; black stays neutral.
    const std::array<float, 3> w = gradingZoneWeights(push, lab[0]);
    float offsetA = push.wheels[3][0], offsetB = push.wheels[3][1], stops = push.wheels[3][2];
    for (int z = 0; z < 3; ++z) {
        offsetA += w[z] * push.wheels[z][0];
        offsetB += w[z] * push.wheels[z][1];
        stops += w[z] * push.wheels[z][2];
    }
    const float guard = smoothstep(0.0f, 0.1f, lab[0]);
    lab[1] += offsetA * guard;
    lab[2] += offsetB * guard;
    lab[0] *= std::exp2(stops / 3.0f);
    return storable(oklabToLinearSrgb(lab));
}

}  // namespace darkhouse
