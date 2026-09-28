#include "tone_curve.hpp"

#include <algorithm>
#include <cmath>

namespace darkhouse {
namespace {

constexpr float kMinSpacing = 1.0f / 256.0f;  // closest two points may be in x
constexpr float kHalfMax = 65504.0f;

// Slopes of the monotone cubic through the points (PCHIP: weighted harmonic
// mean inside, a limited three-point estimate at the ends).
std::array<float, kMaxCurvePoints> splineSlopes(const Curve& curve) noexcept {
    std::array<float, kMaxCurvePoints> m{};
    const std::uint32_t n = curve.count;
    const auto& p = curve.points;
    if (n < 2) return m;
    std::array<float, kMaxCurvePoints> h{}, d{};
    for (std::uint32_t k = 0; k + 1 < n; ++k) {
        h[k] = p[k + 1].x - p[k].x;
        d[k] = (p[k + 1].y - p[k].y) / h[k];
    }
    if (n == 2) {
        m[0] = m[1] = d[0];
        return m;
    }
    for (std::uint32_t k = 1; k + 1 < n; ++k) {
        if (d[k - 1] * d[k] <= 0.0f) {
            m[k] = 0.0f;  // a local extremum stays flat: no overshoot
        } else {
            const float w1 = 2.0f * h[k] + h[k - 1];
            const float w2 = h[k] + 2.0f * h[k - 1];
            m[k] = (w1 + w2) / (w1 / d[k - 1] + w2 / d[k]);
        }
    }
    auto endSlope = [](float h0, float h1, float d0, float d1) {
        float slope = ((2.0f * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
        if (slope * d0 <= 0.0f) {
            slope = 0.0f;
        } else if (d0 * d1 <= 0.0f && std::fabs(slope) > 3.0f * std::fabs(d0)) {
            slope = 3.0f * d0;
        }
        return slope;
    };
    m[0] = endSlope(h[0], h[1], d[0], d[1]);
    m[n - 1] = endSlope(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
    return m;
}

// Lookup with linear interpolation; past white, the curve's slope at white.
float lookup(const float* table, float slope, float encoded) noexcept {
    constexpr float kLast = static_cast<float>(kToneCurveLutSize - 1);
    if (encoded > 1.0f) return table[kToneCurveLutSize - 1] + (encoded - 1.0f) * slope;
    const float position = std::max(encoded, 0.0f) * kLast;
    const std::size_t i0 = std::min(static_cast<std::size_t>(position), kToneCurveLutSize - 2);
    const float f = position - static_cast<float>(i0);
    return table[i0] + (table[i0 + 1] - table[i0]) * f;
}

}  // namespace

Curve sanitize(const Curve& curve) {
    Curve out;
    const std::uint32_t count = std::min<std::uint32_t>(curve.count, kMaxCurvePoints);
    std::array<CurvePoint, kMaxCurvePoints> points{};
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const CurvePoint& p = curve.points[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
        points[kept++] = {std::clamp(p.x, 0.0f, 1.0f), std::clamp(p.y, 0.0f, 1.0f)};
    }
    std::stable_sort(points.begin(), points.begin() + kept, [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    for (std::uint32_t i = 0; i < kept; ++i) {
        if (out.count > 0 && points[i].x - out.points[out.count - 1].x < kMinSpacing) continue;
        out.points[out.count++] = points[i];
    }
    if (out.count < 2 || isIdentity(out)) return Curve{};
    return out;
}

ToneCurveParams sanitize(const ToneCurveParams& params) {
    ToneCurveParams out;
    for (std::size_t c = 0; c < kCurveChannelCount; ++c) out.curves[c] = sanitize(params.curves[c]);
    return out;
}

bool isIdentity(const Curve& curve) noexcept {
    if (curve.count == 0) return true;
    const CurvePoint& first = curve.points[0];
    const CurvePoint& last = curve.points[std::min<std::uint32_t>(curve.count, kMaxCurvePoints) - 1];
    if (first.x > 1e-4f || last.x < 1.0f - 1e-4f) return false;
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(curve.count, kMaxCurvePoints); ++i) {
        if (std::fabs(curve.points[i].y - curve.points[i].x) > 1e-4f) return false;
    }
    return true;
}

bool isIdentity(const ToneCurveParams& params) noexcept {
    return std::all_of(params.curves.begin(), params.curves.end(), [](const Curve& c) { return isIdentity(c); });
}

float evaluateCurve(const Curve& curve, float x) noexcept {
    const std::uint32_t n = std::min<std::uint32_t>(curve.count, kMaxCurvePoints);
    if (n == 0) return x;
    const auto& p = curve.points;
    if (n == 1 || x <= p[0].x) return p[0].y;
    if (x >= p[n - 1].x) return p[n - 1].y;
    std::uint32_t k = 0;
    while (k + 2 < n && x > p[k + 1].x) ++k;
    const std::array<float, kMaxCurvePoints> m = splineSlopes(curve);
    const float h = p[k + 1].x - p[k].x;
    const float t = (x - p[k].x) / h;
    const float t2 = t * t, t3 = t2 * t;
    const float y = (2.0f * t3 - 3.0f * t2 + 1.0f) * p[k].y + (t3 - 2.0f * t2 + t) * h * m[k] +
                    (-2.0f * t3 + 3.0f * t2) * p[k + 1].y + (t3 - t2) * h * m[k + 1];
    return std::clamp(y, 0.0f, 1.0f);
}

float encodeTransfer(float linear) noexcept {
    const float x = std::max(linear, 0.0f);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

float decodeTransfer(float encoded) noexcept {
    const float e = std::max(encoded, 0.0f);
    return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
}

ToneCurveTables bakeToneCurve(const ToneCurveParams& params) {
    ToneCurveTables tables;
    constexpr float kLast = static_cast<float>(kToneCurveLutSize - 1);
    for (std::size_t c = 0; c < kCurveChannelCount; ++c) {
        const Curve curve = sanitize(params.curves[c]);
        float* table = &tables.lut[c * kToneCurveLutSize];
        for (std::size_t i = 0; i < kToneCurveLutSize; ++i) {
            const float x = static_cast<float>(i) / kLast;
            table[i] = evaluateCurve(curve, x);
        }
        tables.endSlope[c] = std::max((table[kToneCurveLutSize - 1] - table[kToneCurveLutSize - 2]) * kLast, 0.0f);
        if (!isIdentity(curve)) tables.activeMask |= 1u << c;
    }
    return tables;
}

Rgb applyToneCurve(const ToneCurveTables& tables, const Rgb& rgb) noexcept {
    Rgb out = rgb;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        float e = encodeTransfer(rgb[channel]);
        if (tables.activeMask & 1u) e = lookup(&tables.lut[0], tables.endSlope[0], e);
        const std::size_t c = channel + 1;
        if (tables.activeMask & (1u << c)) e = lookup(&tables.lut[c * kToneCurveLutSize], tables.endSlope[c], e);
        out[channel] = std::clamp(decodeTransfer(e), 0.0f, kHalfMax);
    }
    return out;
}

Curve curvePreset(CurvePreset preset) {
    Curve curve;
    auto set = [&](std::initializer_list<CurvePoint> points) {
        for (const CurvePoint& p : points) curve.points[curve.count++] = p;
    };
    switch (preset) {
    case CurvePreset::LINEAR: break;
    case CurvePreset::MEDIUM_CONTRAST: set({{0.0f, 0.0f}, {0.25f, 0.20f}, {0.5f, 0.5f}, {0.75f, 0.80f}, {1.0f, 1.0f}}); break;
    case CurvePreset::STRONG_CONTRAST: set({{0.0f, 0.0f}, {0.25f, 0.15f}, {0.5f, 0.5f}, {0.75f, 0.86f}, {1.0f, 1.0f}}); break;
    case CurvePreset::FADED: set({{0.0f, 0.08f}, {0.25f, 0.28f}, {0.5f, 0.52f}, {0.75f, 0.76f}, {1.0f, 0.95f}}); break;
    }
    return curve;
}

const char* toString(CurvePreset preset) noexcept {
    switch (preset) {
    case CurvePreset::LINEAR: return "Linear";
    case CurvePreset::MEDIUM_CONTRAST: return "Medium Contrast";
    case CurvePreset::STRONG_CONTRAST: return "Strong Contrast";
    case CurvePreset::FADED: return "Faded";
    }
    return "?";
}

}  // namespace darkhouse
