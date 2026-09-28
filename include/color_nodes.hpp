// DarkHouse — GPU nodes for the colour adjustments in color_adjust.hpp.
//
//   WhiteBalanceNode   "white_balance"  shaders/white_balance.comp
//   HslNode            "hsl"            shaders/hsl_adjust.comp
//   ColorGradingNode   "color_grading"  shaders/color_grading.comp
//
// Each keeps its sanitized parameters and the push constants derived from
// them; a parameter change costs a CPU-side update and nothing else (the next
// evaluation records the new push constants).
#pragma once

#include "color_adjust.hpp"
#include "render_pipeline.hpp"

#include <filesystem>
#include <span>
#include <string_view>

namespace darkhouse {

class WhiteBalanceNode final : public PointOperatorNode {
public:
    static constexpr std::string_view kTypeName = "white_balance";

    WhiteBalanceNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory);

    [[nodiscard]] std::string_view typeName() const noexcept override { return kTypeName; }
    void updateUniforms(std::span<const std::byte> packedParams) override;

    void setParams(const WhiteBalanceParams& params) noexcept;
    [[nodiscard]] const WhiteBalanceParams& params() const noexcept { return params_; }

private:
    [[nodiscard]] const void* pushConstants() const noexcept override { return &push_; }

    WhiteBalanceParams params_;
    WhiteBalancePush push_;
};

class HslNode final : public PointOperatorNode {
public:
    static constexpr std::string_view kTypeName = "hsl";

    HslNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory);

    [[nodiscard]] std::string_view typeName() const noexcept override { return kTypeName; }
    void updateUniforms(std::span<const std::byte> packedParams) override;

    void setParams(const HslParams& params) noexcept { params_ = sanitize(params); }
    [[nodiscard]] const HslParams& params() const noexcept { return params_; }

private:
    [[nodiscard]] const void* pushConstants() const noexcept override { return &params_; }

    HslParams params_;
};

class ColorGradingNode final : public PointOperatorNode {
public:
    static constexpr std::string_view kTypeName = "color_grading";

    ColorGradingNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory);

    [[nodiscard]] std::string_view typeName() const noexcept override { return kTypeName; }
    void updateUniforms(std::span<const std::byte> packedParams) override;

    void setParams(const ColorGradingParams& params) noexcept;
    [[nodiscard]] const ColorGradingParams& params() const noexcept { return params_; }

private:
    [[nodiscard]] const void* pushConstants() const noexcept override { return &push_; }

    ColorGradingParams params_;
    ColorGradingPush push_;
};

}  // namespace darkhouse
