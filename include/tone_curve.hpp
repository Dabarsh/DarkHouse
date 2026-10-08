// DarkHouse — point tone curves (the Develop module's Tone Curve).
//
// Four curves: a composite RGB curve applied to every channel, then one per
// channel (red, green, blue). Each is a list of control points in a
// perceptual encoding (the sRGB transfer curve: 0 = black, 1 = diffuse
// white), interpolated with a monotone cubic spline (Fritsch–Carlson), so a
// rising set of points never overshoots into bands or inversions.
//
// The curves are baked into lookup tables of kToneCurveLutSize entries on the
// CPU; the GPU (shaders/tone_curve.comp) and the CPU reference below read the
// same tables with the same linear interpolation. Values above white keep the
// curve's slope at white, so highlights beyond 1.0 are not clipped.
#pragma once

#include "color_adjust.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace darkhouse {

enum class CurveChannel : std::uint32_t { RGB, RED, GREEN, BLUE };
inline constexpr std::size_t kCurveChannelCount = 4;
inline constexpr std::size_t kMaxCurvePoints = 16;
inline constexpr std::size_t kToneCurveLutSize = 1024;

struct CurvePoint {
    float x = 0.0f;  // input, 0..1 (encoded)
    float y = 0.0f;  // output, 0..1 (encoded)

    bool operator==(const CurvePoint&) const = default;
};

struct Curve {
    std::uint32_t count = 0;  // 0 = identity
    std::array<CurvePoint, kMaxCurvePoints> points{};
};

// The tone_curve node's parameters (and serialized form: 4 x 132 bytes).
struct ToneCurveParams {
    std::array<Curve, kCurveChannelCount> curves{};
};
static_assert(sizeof(ToneCurveParams) == 4 * (4 + 16 * 8), "ToneCurveParams is serialized as raw bytes");

// Sorts points by x, clamps them to [0, 1], merges points closer than 1/256
// in x and caps the count; one point or a straight identity line becomes
// identity (count 0).
[[nodiscard]] Curve sanitize(const Curve& curve);
[[nodiscard]] ToneCurveParams sanitize(const ToneCurveParams& params);
[[nodiscard]] bool isIdentity(const Curve& curve) noexcept;
[[nodiscard]] bool isIdentity(const ToneCurveParams& params) noexcept;

// The curve's value at x (monotone cubic spline; flat beyond the end points).
[[nodiscard]] float evaluateCurve(const Curve& curve, float x) noexcept;

// sRGB transfer curve, extended past 1 with the same power law.
[[nodiscard]] float encodeTransfer(float linear) noexcept;
[[nodiscard]] float decodeTransfer(float encoded) noexcept;

// The four curves sampled at kToneCurveLutSize evenly spaced inputs.
struct ToneCurveTables {
    std::array<float, kCurveChannelCount * kToneCurveLutSize> lut{};
    std::array<float, kCurveChannelCount> endSlope{};  // extension past white
    std::uint32_t activeMask = 0;                      // bit i: curve i is not the identity
};
[[nodiscard]] ToneCurveTables bakeToneCurve(const ToneCurveParams& params);

// Push constants of shaders/tone_curve.comp.
struct ToneCurvePush {
    std::array<float, 4> endSlope{};
    std::uint32_t activeMask = 0;
    std::uint32_t pad[3]{};
};
static_assert(sizeof(ToneCurvePush) == 32, "ToneCurvePush must match the shader");

// CPU reference of the node.
[[nodiscard]] Rgb applyToneCurve(const ToneCurveTables& tables, const Rgb& rgb) noexcept;

// Built-in curves for the composite channel.
enum class CurvePreset : std::uint8_t { LINEAR, MEDIUM_CONTRAST, STRONG_CONTRAST, FADED };
[[nodiscard]] Curve curvePreset(CurvePreset preset);
[[nodiscard]] const char* toString(CurvePreset preset) noexcept;

}  // namespace darkhouse
