// DarkHouse — colour adjustments of the develop stack.
//
// Three GPU nodes, all on scene-linear Rec.709 RGB (RGBA16F), in develop
// order:
//
//   white_balance  temperature / tint: a Bradford chromatic adaptation from
//                  the chosen illuminant (a point on the Planckian locus,
//                  shifted off it by tint) to the 5500 K reference, applied
//                  as one 3x3 matrix
//   hsl            colour mixer: hue / saturation / luminance of 8 hue bands,
//                  in Oklch (perceptual hue, chroma, lightness)
//   color_grading  presence (vibrance, saturation) and 3-way colour wheels
//                  (shadows / midtones / highlights, plus global) with
//                  blending and balance, in Oklab
//
// Each has a parameter struct (serialized as-is into edit_nodes), the push
// constants its shader receives (derived here, on the CPU, once per change)
// and a CPU reference that the GPU output is tested against.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace darkhouse {

using Rgb = std::array<float, 3>;

// --- Oklab (Björn Ottosson) on linear sRGB / Rec.709 --------------------------------

[[nodiscard]] Rgb linearSrgbToOklab(const Rgb& rgb) noexcept;
[[nodiscard]] Rgb oklabToLinearSrgb(const Rgb& lab) noexcept;

// --- White balance ----------------------------------------------------------------------

inline constexpr float kReferenceTemperature = 5500.0f;  // "as shot": the identity setting
inline constexpr float kMinTemperature = 2000.0f;
inline constexpr float kMaxTemperature = 20000.0f;
inline constexpr float kTintRange = 150.0f;

struct WhiteBalanceParams {
    float temperature = kReferenceTemperature;  // kelvin of the scene illuminant; lower = cooler result
    float tint = 0.0f;                          // [-150, 150]; positive = towards magenta
};
static_assert(sizeof(WhiteBalanceParams) == 8);

// Row-major 3x3 matrix, padded to vec4 rows for the push-constant block.
struct WhiteBalancePush {
    float rows[3][4];
};
static_assert(sizeof(WhiteBalancePush) == 48);

[[nodiscard]] WhiteBalanceParams sanitize(const WhiteBalanceParams& params) noexcept;
[[nodiscard]] bool isIdentity(const WhiteBalanceParams& params) noexcept;
// CIE 1931 xy of the illuminant: the Planckian locus at `temperature`,
// moved along the locus normal (in CIE 1960 uv) by tint.
[[nodiscard]] std::array<float, 2> illuminantChromaticity(float temperature, float tint) noexcept;
[[nodiscard]] WhiteBalancePush whiteBalancePush(const WhiteBalanceParams& params) noexcept;
[[nodiscard]] Rgb applyWhiteBalance(const WhiteBalancePush& push, const Rgb& rgb) noexcept;

// --- Tone ---------------------------------------------------------------------------------

// Exposure (stops), then highlights / shadows and contrast (each -1..1), as
// the exposure node and the local adjustments apply them
// (shaders/tone_common.glsl).
[[nodiscard]] Rgb applyTone(Rgb rgb, float exposureEV, float highlights, float shadows, float contrast) noexcept;

// --- HSL colour mixer ---------------------------------------------------------------------

inline constexpr std::size_t kHslBands = 8;
inline constexpr std::array<const char*, kHslBands> kHslBandNames{"Red",  "Orange", "Yellow", "Green",
                                                                  "Aqua", "Blue",   "Purple", "Magenta"};
// Band centres: the Oklch hue (degrees) of each band's pure sRGB colour.
inline constexpr std::array<float, kHslBands> kHslBandHues{29.2f, 53.0f, 109.8f, 142.5f, 194.8f, 264.1f, 293.9f, 328.4f};
inline constexpr float kHslMaxHueShift = 30.0f;  // degrees at +-100

struct HslParams {
    std::array<float, kHslBands> hue{};         // [-100, 100]: +-30 degrees towards the neighbours
    std::array<float, kHslBands> saturation{};  // [-100, 100]: chroma x0 .. x2
    std::array<float, kHslBands> luminance{};   // [-100, 100]: -1 .. +1 stop of lightness
};
static_assert(sizeof(HslParams) == 96, "HslParams is also the push-constant block of hsl_adjust.comp");

[[nodiscard]] HslParams sanitize(const HslParams& params) noexcept;
[[nodiscard]] bool isIdentity(const HslParams& params) noexcept;
[[nodiscard]] Rgb applyHsl(const HslParams& params, const Rgb& rgb) noexcept;

// --- Colour grading ------------------------------------------------------------------------

struct ColorWheel {
    float hue = 0.0f;         // degrees, Oklch hue of the tint
    float saturation = 0.0f;  // [0, 100]
    float luminance = 0.0f;   // [-100, 100]: -1 .. +1 stop of lightness in the zone
};

struct ColorGradingParams {
    float vibrance = 0.0f;    // [-100, 100]: saturation that protects already-saturated colours
    float saturation = 0.0f;  // [-100, 100]: chroma x0 .. x2
    ColorWheel shadows;
    ColorWheel midtones;
    ColorWheel highlights;
    ColorWheel global;
    float blending = 50.0f;   // [0, 100]: overlap of the tonal zones
    float balance = 0.0f;     // [-100, 100]: positive moves the zones' split towards the shadows
};
static_assert(sizeof(ColorGradingParams) == 64);

inline constexpr float kGradingMaxChroma = 0.06f;  // Oklab ab offset at wheel saturation 100

struct ColorGradingPush {
    float saturationScale;  // 1 + saturation / 100
    float vibrance;         // vibrance / 100
    float pivot;            // lightness splitting shadows / highlights (0.25 .. 0.75)
    float blending;         // transition width, 0.01 .. 1
    float wheels[4][4];     // shadows, midtones, highlights, global: Oklab a, b offset, lightness stops, 0
};
static_assert(sizeof(ColorGradingPush) == 80);

[[nodiscard]] ColorGradingParams sanitize(const ColorGradingParams& params) noexcept;
[[nodiscard]] bool isIdentity(const ColorGradingParams& params) noexcept;
[[nodiscard]] ColorGradingPush colorGradingPush(const ColorGradingParams& params) noexcept;
// Weights of the shadow, midtone and highlight zones at Oklab lightness L (they sum to 1).
[[nodiscard]] std::array<float, 3> gradingZoneWeights(const ColorGradingPush& push, float lightness) noexcept;
[[nodiscard]] Rgb applyColorGrading(const ColorGradingPush& push, const Rgb& rgb) noexcept;

// --- Serialized form (edit_nodes.serialized_params) ------------------------------------------

// The structs are plain floats in host byte order (little-endian on every
// supported target), the same convention as ExposureParams.
template <class Params>
[[nodiscard]] std::vector<std::byte> packParams(const Params& params) {
    std::vector<std::byte> bytes(sizeof(Params));
    const auto* source = reinterpret_cast<const std::byte*>(&params);
    std::copy(source, source + sizeof(Params), bytes.begin());
    return bytes;
}

// Throws std::invalid_argument when the size is wrong.
template <class Params>
[[nodiscard]] Params unpackParams(std::span<const std::byte> bytes, const char* what) {
    if (bytes.size() != sizeof(Params)) {
        throw std::invalid_argument(std::string(what) + " expects " + std::to_string(sizeof(Params)) + " bytes, got " +
                                    std::to_string(bytes.size()));
    }
    Params params;
    std::memcpy(&params, bytes.data(), sizeof params);
    return params;
}

}  // namespace darkhouse
