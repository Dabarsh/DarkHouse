#include "local_adjust_node.hpp"

#include "vulkan_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace darkhouse {
namespace {

// std430 layout of LocalAdjustGpu in shaders/local_adjust.comp.
struct LocalAdjustGpu {
    float amount;
    float exposure;
    float highlights;
    float shadows;
    float contrast;
    float saturationScale;
    std::uint32_t enabled;
    std::uint32_t whiteBalance;
    float wb[3][4];
};
static_assert(sizeof(LocalAdjustGpu) == 80);

struct Push {
    std::uint32_t maskCount;
    std::int32_t overlayMask;
};

std::uint32_t groups(std::uint32_t size) { return (size + 15) / 16; }

}  // namespace

LocalAdjustNode::LocalAdjustNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : context_(context) {
    const VkDevice device = context_.device();
    try {
        engine_ = std::make_unique<MaskEngine>(context_, shaderDirectory);

        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;  // input, masks, output, adjustments
            bindings[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");

        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        checkVk(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");

        const VkShaderModule module = context_.loadShaderModule(shaderDirectory / "local_adjust.spv");
        ScopeExit destroyModule([&] { vkDestroyShaderModule(device, module, nullptr); });
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout_;
        checkVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_),
                "vkCreateComputePipelines");

        std::array<VkDescriptorPoolSize, 2> poolSizes{{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
                                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = descriptorPool_;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &setLayout_;
        checkVk(vkAllocateDescriptorSets(device, &setInfo, &descriptorSet_), "vkAllocateDescriptorSets");

        adjustmentBuffer_ = context_.createBuffer(kMaxMasks * sizeof(LocalAdjustGpu),
                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  /*hostVisible=*/false);
    } catch (...) {
        destroy();
        throw;
    }
}

LocalAdjustNode::~LocalAdjustNode() { destroy(); }

void LocalAdjustNode::destroy() noexcept {
    const VkDevice device = context_.device();
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    if (setLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    context_.destroyBuffer(adjustmentBuffer_);
    context_.destroyTexture(output_);
    engine_.reset();
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

void LocalAdjustNode::setInputTexture(std::uint32_t slot, const GPUTexture& texture) {
    if (slot != 0) throw std::out_of_range("local_adjust node has a single input (slot 0)");
    if (!texture.valid() || texture.format != PixelFormat::R16G16B16A16_SFLOAT) {
        throw std::invalid_argument("local_adjust node: input must be an allocated rgba16f texture");
    }
    if (texture.view != input_.view) descriptorsDirty_ = true;
    input_ = texture;
    if (!output_.valid() || output_.width != texture.width || output_.height != texture.height) {
        context_.destroyTexture(output_);
        output_ = context_.createTexture(texture.width, texture.height, PixelFormat::R16G16B16A16_SFLOAT,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        descriptorsDirty_ = true;
    }
}

void LocalAdjustNode::updateUniforms(std::span<const std::byte> packedParams) {
    setAdjustments(deserializeLocalAdjustments(packedParams));
}

void LocalAdjustNode::setAdjustments(const LocalAdjustments& adjustments) {
    adjustments_ = sanitize(adjustments);
    std::vector<LocalAdjustGpu> records;
    for (const LocalAdjustment& mask : adjustments_.masks) {
        LocalAdjustGpu g{};
        g.amount = mask.amount;
        g.exposure = mask.params.exposure;
        g.highlights = mask.params.highlights / 100.0f;
        g.shadows = mask.params.shadows / 100.0f;
        g.contrast = mask.params.contrast / 100.0f;
        g.saturationScale = std::max(0.0f, 1.0f + mask.params.saturation / 100.0f);
        g.enabled = mask.enabled ? 1u : 0u;
        g.whiteBalance = mask.params.temperature != 0.0f || mask.params.tint != 0.0f ? 1u : 0u;
        const WhiteBalancePush wb = whiteBalancePush(
            {kReferenceTemperature * std::exp2(0.6f * mask.params.temperature / 100.0f), 1.5f * mask.params.tint});
        std::memcpy(g.wb, wb.rows, sizeof g.wb);
        records.push_back(g);
    }
    adjustmentData_.resize(records.size() * sizeof(LocalAdjustGpu));
    std::memcpy(adjustmentData_.data(), records.data(), adjustmentData_.size());
}

void LocalAdjustNode::writeDescriptors() {
    GPUTexture& masks = engine_->maskTexture();
    std::array<VkDescriptorImageInfo, 3> images{};
    images[0] = {VK_NULL_HANDLE, input_.view, VK_IMAGE_LAYOUT_GENERAL};
    images[1] = {VK_NULL_HANDLE, masks.view, VK_IMAGE_LAYOUT_GENERAL};
    images[2] = {VK_NULL_HANDLE, output_.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo buffer{adjustmentBuffer_.buffer, 0, VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i < 3) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffer;
        }
    }
    vkUpdateDescriptorSets(context_.device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    boundMasks_ = masks.view;
    descriptorsDirty_ = false;
}

void LocalAdjustNode::executeCompute(VkCommandBuffer commandBuffer) {
    if (!input_.valid() || !output_.valid()) {
        throw std::logic_error("local_adjust node: executeCompute called before setInputTexture");
    }
    // Host work first: brush dabs are painted and uploaded (their own
    // submission, ahead of this one on the queue); textures may be replaced.
    engine_->prepare(adjustments_, input_.width, input_.height);
    if (descriptorsDirty_ || engine_->maskTexture().view != boundMasks_) writeDescriptors();

    engine_->record(commandBuffer, input_);

    recordImageBarrier(commandBuffer, output_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       /*discardContents=*/true);
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!adjustmentData_.empty()) {
        vkCmdUpdateBuffer(commandBuffer, adjustmentBuffer_.buffer, 0, adjustmentData_.size(), adjustmentData_.data());
    }
    // Masks written by the generate pass and the parameters, visible to this pass.
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    const Push push{engine_->maskCount(), overlay_ < static_cast<int>(engine_->maskCount()) ? overlay_ : -1};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(commandBuffer, groups(output_.width), groups(output_.height), 1);
}

}  // namespace darkhouse
