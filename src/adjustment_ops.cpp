#include "adjustment_ops.hpp"

#include "render_pipeline.hpp"
#include "tone_curve.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace darkhouse {

struct PointAdjustment::State {
    std::variant<ExposureParams, WhiteBalancePush, ToneCurveTables, HslParams, ColorGradingPush> op;
};

bool supportsAdjustmentLayer(std::string_view nodeType) noexcept {
    constexpr std::array<std::string_view, 5> kTypes{"exposure", "white_balance", "tone_curve", "hsl", "color_grading"};
    for (std::string_view type : kTypes) {
        if (type == nodeType) return true;
    }
    return false;
}

std::optional<PointAdjustment> PointAdjustment::create(std::string_view nodeType, std::span<const std::byte> bytes) {
    auto state = std::make_shared<State>();
    PointAdjustment adjustment;
    try {
        if (nodeType == "exposure") {
            ExposureParams p = unpackParams<ExposureParams>(bytes, "exposure layer");
            // Same limits as ExposureNode::setParams.
            auto limit = [](float v, float bound) { return std::isfinite(v) ? std::clamp(v, -bound, bound) : 0.0f; };
            p = {limit(p.exposureEV, 10.0f), limit(p.highlights, 1.0f), limit(p.shadows, 1.0f), limit(p.contrast, 1.0f)};
            state->op = p;
            adjustment.identity_ = p.exposureEV == 0.0f && p.highlights == 0.0f && p.shadows == 0.0f && p.contrast == 0.0f;
        } else if (nodeType == "white_balance") {
            const WhiteBalanceParams p = sanitize(unpackParams<WhiteBalanceParams>(bytes, "white balance layer"));
            state->op = whiteBalancePush(p);
            adjustment.identity_ = isIdentity(p);
        } else if (nodeType == "tone_curve") {
            const ToneCurveParams p = sanitize(unpackParams<ToneCurveParams>(bytes, "tone curve layer"));
            state->op = bakeToneCurve(p);
            adjustment.identity_ = isIdentity(p);
        } else if (nodeType == "hsl") {
            const HslParams p = sanitize(unpackParams<HslParams>(bytes, "hsl layer"));
            state->op = p;
            adjustment.identity_ = isIdentity(p);
        } else if (nodeType == "color_grading") {
            const ColorGradingParams p = sanitize(unpackParams<ColorGradingParams>(bytes, "color grading layer"));
            state->op = colorGradingPush(p);
            adjustment.identity_ = isIdentity(p);
        } else {
            return std::nullopt;
        }
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    adjustment.state_ = std::move(state);
    return adjustment;
}

Rgb PointAdjustment::apply(const Rgb& rgb) const noexcept {
    if (identity_ || !state_) return rgb;
    return std::visit(
        [&](const auto& op) -> Rgb {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, ExposureParams>) {
                return applyTone(rgb, op.exposureEV, op.highlights, op.shadows, op.contrast);
            } else if constexpr (std::is_same_v<T, WhiteBalancePush>) {
                return applyWhiteBalance(op, rgb);
            } else if constexpr (std::is_same_v<T, ToneCurveTables>) {
                return applyToneCurve(op, rgb);
            } else if constexpr (std::is_same_v<T, HslParams>) {
                return applyHsl(op, rgb);
            } else {
                return applyColorGrading(op, rgb);
            }
        },
        state_->op);
}

}  // namespace darkhouse
