// DarkHouse — lens corrections (the Develop module's Lens Corrections).
//
// One resampling pass that undoes what the lens did to the image:
//
//   distortion   barrel / pincushion, from a lens profile (lensfun ptlens,
//                poly3 or poly5 model) scaled by an amount, plus a manual term
//   lateral CA   red and blue sampled at slightly different radii than green
//                (profile poly3 / linear TCA, plus manual red/cyan and
//                blue/yellow sliders)
//   vignetting   the profile's pa falloff divided out (with an amount), plus a
//                manual corner brightening / darkening with a midpoint
//
// Each output pixel maps to a source position: undistorted normalized
// coordinates -> distorted (the profile model gives the source radius for a
// corrected radius) -> red / blue shifted by TCA. Geometry is normalized as
// lensfun does: radius 1 at half the shorter image side for distortion and
// TCA, at half the diagonal for vignetting, both scaled by the ratio of the
// calibration camera's crop factor to the photo camera's. "Constrain crop"
// zooms in just enough that no output pixel samples outside the photo.
//
// The GPU node (shaders/lens_correction.comp) and the CPU reference here
// share the mapping (lensSourcePosition) and bilinear sampling with clamped
// edges.
#pragma once

#include "lens_database.hpp"
#include "render_pipeline.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace darkhouse {

// The lens_correction node's parameters (serialized as raw bytes).
struct LensCorrectionParams {
    // Profile: coefficients resolved from a lens database (lens_database.hpp).
    std::uint32_t profileEnabled = 0;
    DistortionModel distortionModel = DistortionModel::NONE;
    std::array<float, 3> distortion{};
    TcaModel tcaModel = TcaModel::NONE;
    std::array<float, 3> tcaRed{1.0f, 0.0f, 0.0f};
    std::array<float, 3> tcaBlue{1.0f, 0.0f, 0.0f};
    std::uint32_t hasVignetting = 0;
    std::array<float, 3> vignetting{};
    float radiusScale = 1.0f;
    float distortionAmount = 100.0f;  // % of the profile's distortion correction, 0..200
    float vignettingAmount = 100.0f;  // % of the profile's vignetting correction, 0..200
    std::uint32_t removeChromaticAberration = 1;

    // Manual corrections.
    float manualDistortion = 0.0f;          // -100..100; positive removes barrel distortion
    float manualVignetting = 0.0f;          // -100..100; positive brightens the corners
    float manualVignettingMidpoint = 50.0f;  // 0..100; how far in from the corners it reaches
    float manualCaRed = 0.0f;               // -100..100 red / cyan fringes
    float manualCaBlue = 0.0f;              // -100..100 blue / yellow fringes
    std::uint32_t constrainCrop = 1;        // zoom so no edge samples outside the photo
    float scale = 100.0f;                   // 50..150 %, on top of the constraint

    char profileName[64]{};  // for display; which profile the coefficients came from
};
static_assert(sizeof(LensCorrectionParams) == 172, "LensCorrectionParams is serialized as raw bytes");

[[nodiscard]] LensCorrectionParams sanitize(const LensCorrectionParams& params);
[[nodiscard]] bool isIdentity(const LensCorrectionParams& params) noexcept;
// Copies a resolved profile into the profile fields (and enables them).
void applyProfile(LensCorrectionParams& params, const ResolvedLensProfile& profile);

// Push constants of shaders/lens_correction.comp: the parameters prepared
// for one image size (128 bytes, the guaranteed push-constant limit).
struct LensCorrectionPush {
    std::array<float, 4> geometry{};     // centre x, centre y, distortion unit, vignetting unit (pixels)
    std::array<float, 4> distortion{};   // coefficients, amount (0..2)
    std::array<float, 4> tcaRed{};       // v, c, b, manual scale
    std::array<float, 4> tcaBlue{};      // v, c, b, manual scale
    std::array<float, 4> vignetting{};   // k1, k2, k3, amount (0 = off)
    std::array<float, 4> manual{};       // distortion k, vignetting stops, midpoint, output scale
    std::array<float, 4> reserved{};
    std::array<std::uint32_t, 4> models{};  // distortion model, TCA model, flags, unused
};
static_assert(sizeof(LensCorrectionPush) == 128, "LensCorrectionPush must match the shader");

// Prepares the push constants for a width x height image, including the
// constrain-crop zoom.
[[nodiscard]] LensCorrectionPush lensCorrectionPush(const LensCorrectionParams& params, std::uint32_t width,
                                                    std::uint32_t height);

// Source positions (pixels) of the red, green and blue samples for output
// pixel centre (x, y), and the vignetting gain there.
struct LensSample {
    std::array<float, 2> red{}, green{}, blue{};
    float gain = 1.0f;
};
[[nodiscard]] LensSample lensSourcePosition(const LensCorrectionPush& push, float x, float y) noexcept;

// CPU reference: the corrected image (linear RGBA floats, width x height).
[[nodiscard]] std::vector<float> applyLensCorrection(const LensCorrectionPush& push, std::uint32_t width,
                                                     std::uint32_t height, std::span<const float> rgba);

class LensCorrectionNode final : public PointOperatorNode {
public:
    static constexpr std::string_view kTypeName = "lens_correction";

    LensCorrectionNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory);

    [[nodiscard]] std::string_view typeName() const noexcept override { return kTypeName; }
    void updateUniforms(std::span<const std::byte> packedParams) override;
    void setInputTexture(std::uint32_t slot, const GPUTexture& texture) override;

    void setParams(const LensCorrectionParams& params);
    [[nodiscard]] const LensCorrectionParams& params() const noexcept { return params_; }
    [[nodiscard]] const LensCorrectionPush& push() const noexcept { return push_; }

private:
    [[nodiscard]] const void* pushConstants() const noexcept override { return &push_; }

    LensCorrectionParams params_;
    LensCorrectionPush push_;
    std::uint32_t width_ = 1;
    std::uint32_t height_ = 1;
};

}  // namespace darkhouse
