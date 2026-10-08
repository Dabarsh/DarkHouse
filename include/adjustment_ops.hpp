// DarkHouse — CPU evaluation of point adjustments, for adjustment layers.
//
// An adjustment layer (LayerType::PARAMETRIC_ADJUSTMENT) stores the same
// (node type, serialized parameters) pair as a develop-stack node. The
// compositor applies it to the pixels below it with these CPU references,
// which the GPU nodes are tested against:
//
//   exposure        applyTone
//   white_balance   applyWhiteBalance
//   tone_curve      applyToneCurve
//   hsl             applyHsl
//   color_grading   applyColorGrading
//
// Other develop nodes (denoise, lens_correction, local_adjust) look at more
// than one pixel and are not available as adjustment layers.
#pragma once

#include "color_adjust.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace darkhouse {

[[nodiscard]] bool supportsAdjustmentLayer(std::string_view nodeType) noexcept;

class PointAdjustment {
public:
    // Prepares the operator; nullopt for an unsupported type or parameters
    // that do not parse.
    [[nodiscard]] static std::optional<PointAdjustment> create(std::string_view nodeType,
                                                              std::span<const std::byte> serializedParams);

    [[nodiscard]] bool identity() const noexcept { return identity_; }
    [[nodiscard]] Rgb apply(const Rgb& rgb) const noexcept;

private:
    struct State;
    std::shared_ptr<const State> state_;
    bool identity_ = true;
};

}  // namespace darkhouse
