// DarkHouse — local (masked) adjustments develop node ("local_adjust").
//
// Its parameters are every mask of the photo (LocalAdjustments, serialized
// by mask_engine.hpp). An evaluation is two passes:
//
//   mask_generate   MaskEngine: every mask's alpha into an RGBA16F array
//   local_adjust    for each enabled mask, in order:
//                   colour = mix(colour, adjusted(colour), alpha * amount)
//
// Brush strokes are painted on the CPU as they arrive and only the changed
// rectangles are uploaded, so painting stays cheap on large images.
#pragma once

#include "mask_engine.hpp"
#include "render_pipeline.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace darkhouse {

class LocalAdjustNode final : public ComputeNode {
public:
    static constexpr std::string_view kTypeName = "local_adjust";

    // Loads <shaderDirectory>/mask_generate.spv and local_adjust.spv.
    LocalAdjustNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory);
    ~LocalAdjustNode() override;

    LocalAdjustNode(const LocalAdjustNode&) = delete;
    LocalAdjustNode& operator=(const LocalAdjustNode&) = delete;

    [[nodiscard]] std::string_view typeName() const noexcept override { return kTypeName; }
    [[nodiscard]] std::uint32_t inputCount() const noexcept override { return 1; }
    void setInputTexture(std::uint32_t slot, const GPUTexture& texture) override;
    [[nodiscard]] const GPUTexture& getOutputTexture() const override { return output_; }
    void executeCompute(VkCommandBuffer commandBuffer) override;
    // Bytes from serialize(LocalAdjustments); empty = no masks.
    void updateUniforms(std::span<const std::byte> packedParams) override;

    void setAdjustments(const LocalAdjustments& adjustments);
    [[nodiscard]] const LocalAdjustments& adjustments() const noexcept { return adjustments_; }

    // Shows mask `index` as a red overlay in the output (-1: none). A viewing
    // aid, not part of the saved parameters.
    void setOverlay(int index) noexcept { overlay_ = index; }
    [[nodiscard]] int overlay() const noexcept { return overlay_; }

    // The generated masks (for tests and tools): mask i in layer i / 4,
    // channel i % 4, valid once an evaluation has completed.
    [[nodiscard]] GPUTexture& maskTexture() noexcept { return engine_->maskTexture(); }

private:
    void writeDescriptors();
    void destroy() noexcept;

    const VulkanContext& context_;
    std::unique_ptr<MaskEngine> engine_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    GPUBuffer adjustmentBuffer_;
    std::vector<std::byte> adjustmentData_;
    GPUTexture input_;
    GPUTexture output_;
    VkImageView boundMasks_ = VK_NULL_HANDLE;
    bool descriptorsDirty_ = true;
    LocalAdjustments adjustments_;
    int overlay_ = -1;
};

}  // namespace darkhouse
