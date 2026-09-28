#include "color_nodes.hpp"

namespace darkhouse {

WhiteBalanceNode::WhiteBalanceNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : PointOperatorNode(context, shaderDirectory / "white_balance.spv", sizeof(WhiteBalancePush),
                        PixelFormat::R16G16B16A16_SFLOAT),
      push_(whiteBalancePush(params_)) {}

void WhiteBalanceNode::updateUniforms(std::span<const std::byte> packedParams) {
    setParams(unpackParams<WhiteBalanceParams>(packedParams, "WhiteBalanceNode::updateUniforms"));
}

void WhiteBalanceNode::setParams(const WhiteBalanceParams& params) noexcept {
    params_ = sanitize(params);
    push_ = whiteBalancePush(params_);
}

HslNode::HslNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : PointOperatorNode(context, shaderDirectory / "hsl_adjust.spv", sizeof(HslParams),
                        PixelFormat::R16G16B16A16_SFLOAT) {}

void HslNode::updateUniforms(std::span<const std::byte> packedParams) {
    setParams(unpackParams<HslParams>(packedParams, "HslNode::updateUniforms"));
}

ColorGradingNode::ColorGradingNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : PointOperatorNode(context, shaderDirectory / "color_grading.spv", sizeof(ColorGradingPush),
                        PixelFormat::R16G16B16A16_SFLOAT),
      push_(colorGradingPush(params_)) {}

void ColorGradingNode::updateUniforms(std::span<const std::byte> packedParams) {
    setParams(unpackParams<ColorGradingParams>(packedParams, "ColorGradingNode::updateUniforms"));
}

void ColorGradingNode::setParams(const ColorGradingParams& params) noexcept {
    params_ = sanitize(params);
    push_ = colorGradingPush(params_);
}

ToneCurveNode::ToneCurveNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : PointOperatorNode(context, shaderDirectory / "tone_curve.spv", sizeof(ToneCurvePush),
                        PixelFormat::R16G16B16A16_SFLOAT, sizeof(ToneCurveTables::lut)) {
    setParams(ToneCurveParams{});
}

void ToneCurveNode::updateUniforms(std::span<const std::byte> packedParams) {
    setParams(unpackParams<ToneCurveParams>(packedParams, "ToneCurveNode::updateUniforms"));
}

void ToneCurveNode::setParams(const ToneCurveParams& params) {
    params_ = sanitize(params);
    tables_ = bakeToneCurve(params_);
    push_.endSlope = tables_.endSlope;
    push_.activeMask = tables_.activeMask;
    markTableDirty();
}

}  // namespace darkhouse
