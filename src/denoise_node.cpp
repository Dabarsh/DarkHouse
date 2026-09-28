#include "denoise_node.hpp"

#include "vulkan_utils.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace darkhouse {
namespace {

constexpr std::uint32_t kUpGroup = 16;  // local_size of denoise_up / denoise_final

// Push-constant blocks; layouts must match the shaders.
struct SizesPush {
    std::int32_t inputSize[2];
    std::int32_t outputSize[2];
};
struct UpPush {
    std::int32_t fineSize[2];
    std::int32_t coarseSize[2];
    std::uint32_t level;
};
struct FinalPush {
    std::int32_t size[2];
    std::int32_t coarseSize[2];
    float detail;
};
static_assert(sizeof(SizesPush) == 16 && sizeof(UpPush) == 20 && sizeof(FinalPush) == 20);

std::uint32_t groups(std::uint32_t size, std::uint32_t groupSize) { return std::max(1u, (size + groupSize - 1) / groupSize); }

std::int32_t toInt(std::uint32_t value) { return static_cast<std::int32_t>(value); }

VkDescriptorImageInfo storageImage(const GPUTexture& texture) {
    VkDescriptorImageInfo info{};
    info.imageView = texture.view;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return info;
}

VkDescriptorBufferInfo storageBuffer(const GPUBuffer& buffer) {
    VkDescriptorBufferInfo info{};
    info.buffer = buffer.buffer;
    info.range = VK_WHOLE_SIZE;
    return info;
}

// Collects descriptor writes for one set; binding i is the i-th resource added.
class SetWriter {
public:
    explicit SetWriter(VkDescriptorSet set) : set_(set) {}
    SetWriter& image(const GPUTexture& texture) {
        images_.push_back({storageImage(texture), static_cast<std::uint32_t>(bindings_++)});
        return *this;
    }
    SetWriter& buffer(const GPUBuffer& buffer) {
        buffers_.push_back({storageBuffer(buffer), static_cast<std::uint32_t>(bindings_++)});
        return *this;
    }
    void flush(VkDevice device) const {
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(images_.size() + buffers_.size());
        for (const auto& [info, binding] : images_) {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set_;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.pImageInfo = &info;
            writes.push_back(write);
        }
        for (const auto& [info, binding] : buffers_) {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set_;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &info;
            writes.push_back(write);
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

private:
    VkDescriptorSet set_;
    std::size_t bindings_ = 0;
    std::vector<std::pair<VkDescriptorImageInfo, std::uint32_t>> images_;
    std::vector<std::pair<VkDescriptorBufferInfo, std::uint32_t>> buffers_;
};

void computeToCompute(VkCommandBuffer commandBuffer) {
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

}  // namespace

DenoiseNode::DenoiseNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory) : context_(context) {
    try {
        createPipelines(shaderDirectory);
        histogram_ = context_.createBuffer(3 * DH_DENOISE_HIST_BINS * sizeof(std::uint32_t),
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           /*hostVisible=*/false);
        thresholds_ = context_.createBuffer(DH_DENOISE_PARAMS_FLOATS * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            /*hostVisible=*/true);
        std::memset(thresholds_.mapped, 0, DH_DENOISE_PARAMS_FLOATS * sizeof(float));
    } catch (...) {
        destroy();
        throw;
    }
}

DenoiseNode::~DenoiseNode() { destroy(); }

void DenoiseNode::createPipelines(const std::filesystem::path& shaderDirectory) {
    const VkDevice device = context_.device();
    struct Spec {
        const char* shader;
        std::vector<VkDescriptorType> bindings;
        std::uint32_t pushSize;
    };
    constexpr VkDescriptorType kImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType kBuffer = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    const std::array<Spec, PASS_COUNT> specs{{
        {"denoise_down0.spv", {kImage, kImage, kBuffer}, sizeof(SizesPush)},
        {"denoise_down.spv", {kImage, kImage}, sizeof(SizesPush)},
        {"denoise_sigma.spv", {kBuffer, kBuffer}, sizeof(float) * 4},
        {"denoise_up.spv", {kImage, kImage, kImage, kImage, kBuffer}, sizeof(UpPush)},
        {"denoise_final.spv", {kImage, kImage, kImage, kImage, kBuffer}, sizeof(FinalPush)},
    }};

    for (std::size_t p = 0; p < PASS_COUNT; ++p) {
        const Spec& spec = specs[p];
        Pipeline& pipeline = pipelines_[p];

        std::vector<VkDescriptorSetLayoutBinding> bindings(spec.bindings.size());
        for (std::uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = spec.bindings[i];
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        setInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &pipeline.setLayout), "vkCreateDescriptorSetLayout");

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = spec.pushSize;
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &pipeline.setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipeline.layout), "vkCreatePipelineLayout");

        const VkShaderModule module = context_.loadShaderModule(shaderDirectory / spec.shader);
        ScopeExit destroyModule([&] { vkDestroyShaderModule(device, module, nullptr); });
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipeline.layout;
        checkVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline.pipeline),
                (std::string("vkCreateComputePipelines (") + spec.shader + ")").c_str());
    }

    // Enough sets for the deepest pyramid: down0 + sigma + final + 2 * (L - 1).
    constexpr std::uint32_t kMaxSets = 3 + 2 * (DH_DENOISE_MAX_LEVELS - 1);
    const std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 + 2 * (DH_DENOISE_MAX_LEVELS - 1) + 4 * (DH_DENOISE_MAX_LEVELS - 1) + 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 + 2 + (DH_DENOISE_MAX_LEVELS - 1) + 1},
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxSets;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");
}

void DenoiseNode::allocateResources(std::uint32_t width, std::uint32_t height) {
    releaseResources();
    levels_ = denoiseLevelCount(width, height);
    output_ = context_.createTexture(width, height, PixelFormat::R16G16B16A16_SFLOAT,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    for (std::uint32_t k = 1; k <= levels_; ++k) {
        const auto [w, h] = denoiseLevelSize(width, height, k);
        gaussian_.push_back(context_.createTexture(w, h, PixelFormat::R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT));
        if (k < levels_) {
            reconstructed_.push_back(
                context_.createTexture(w, h, PixelFormat::R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT));
        }
    }

    const VkDevice device = context_.device();
    auto allocate = [&](Pass pass) {
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = descriptorPool_;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &pipelines_[pass].setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        checkVk(vkAllocateDescriptorSets(device, &info, &set), "vkAllocateDescriptorSets");
        return set;
    };
    down0Set_ = allocate(DOWN0);
    sigmaSet_ = allocate(SIGMA);
    finalSet_ = allocate(FINAL);
    for (std::uint32_t k = 1; k < levels_; ++k) {
        downSets_.push_back(allocate(DOWN));
        upSets_.push_back(allocate(UP));
    }
}

void DenoiseNode::releaseResources() noexcept {
    if (descriptorPool_ != VK_NULL_HANDLE) vkResetDescriptorPool(context_.device(), descriptorPool_, 0);
    down0Set_ = sigmaSet_ = finalSet_ = VK_NULL_HANDLE;
    downSets_.clear();
    upSets_.clear();
    for (GPUTexture& texture : gaussian_) context_.destroyTexture(texture);
    for (GPUTexture& texture : reconstructed_) context_.destroyTexture(texture);
    gaussian_.clear();
    reconstructed_.clear();
    context_.destroyTexture(output_);
    levels_ = 0;
    measured_ = false;
}

void DenoiseNode::writeDescriptors() {
    const VkDevice device = context_.device();
    const GPUTexture& result1 = levels_ == 1 ? gaussian_[0] : reconstructed_[0];
    SetWriter(down0Set_).image(input_).image(gaussian_[0]).buffer(histogram_).flush(device);
    SetWriter(sigmaSet_).buffer(histogram_).buffer(thresholds_).flush(device);
    SetWriter(finalSet_).image(input_).image(gaussian_[0]).image(result1).image(output_).buffer(thresholds_).flush(device);
    for (std::uint32_t k = 1; k < levels_; ++k) {
        SetWriter(downSets_[k - 1]).image(gaussian_[k - 1]).image(gaussian_[k]).flush(device);
        // R_(k+1) is G_L itself at the coarsest level.
        const GPUTexture& coarseResult = (k + 1 == levels_) ? gaussian_[levels_ - 1] : reconstructed_[k];
        SetWriter(upSets_[k - 1])
            .image(gaussian_[k - 1])
            .image(gaussian_[k])
            .image(coarseResult)
            .image(reconstructed_[k - 1])
            .buffer(thresholds_)
            .flush(device);
    }
}

void DenoiseNode::destroy() noexcept {
    releaseResources();
    const VkDevice device = context_.device();
    context_.destroyBuffer(histogram_);
    context_.destroyBuffer(thresholds_);
    if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    for (Pipeline& pipeline : pipelines_) {
        if (pipeline.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline.pipeline, nullptr);
        if (pipeline.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipeline.layout, nullptr);
        if (pipeline.setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, pipeline.setLayout, nullptr);
        pipeline = Pipeline{};
    }
}

void DenoiseNode::setInputTexture(std::uint32_t slot, const GPUTexture& texture) {
    if (slot != 0) throw std::out_of_range("DenoiseNode has a single input (slot 0)");
    if (!texture.valid()) throw std::invalid_argument("DenoiseNode: input texture is not allocated");
    if (texture.format != PixelFormat::R16G16B16A16_SFLOAT) {
        throw std::invalid_argument("DenoiseNode: input must be R16G16B16A16_SFLOAT (rgba16f)");
    }
    const bool resized = !output_.valid() || output_.width != texture.width || output_.height != texture.height;
    const bool rebound = texture.view != input_.view;
    input_ = texture;
    if (resized) allocateResources(texture.width, texture.height);
    if (resized || rebound) writeDescriptors();
}

void DenoiseNode::dispatch(VkCommandBuffer commandBuffer, Pass pass, VkDescriptorSet set, const void* push,
                           std::uint32_t pushSize, std::uint32_t groupsX, std::uint32_t groupsY) const {
    const Pipeline& pipeline = pipelines_[pass];
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commandBuffer, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize, push);
    vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
}

void DenoiseNode::recordCopy(VkCommandBuffer commandBuffer) {
    // The graph publishes node outputs to compute reads only, so make them
    // visible to the copy, and publish the copy like a compute write.
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    recordImageBarrier(commandBuffer, output_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       /*discardContents=*/true);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {output_.width, output_.height, 1};
    vkCmdCopyImage(commandBuffer, input_.image, VK_IMAGE_LAYOUT_GENERAL, output_.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &region);
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
}

void DenoiseNode::executeCompute(VkCommandBuffer commandBuffer) {
    if (!input_.valid() || !output_.valid()) {
        throw std::logic_error("DenoiseNode::executeCompute called before setInputTexture");
    }
    if (isIdentity(params_)) {
        recordCopy(commandBuffer);
        measured_ = false;
        return;
    }

    // Every intermediate and the output are fully rewritten: discard old
    // contents, and order after earlier readers (e.g. the UI sampling the
    // previous output in a frame still in flight).
    std::vector<VkImageMemoryBarrier2> transitions;
    auto discard = [&](GPUTexture& texture) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        transitions.push_back(barrier);
        texture.layout = VK_IMAGE_LAYOUT_GENERAL;
    };
    for (GPUTexture& texture : gaussian_) discard(texture);
    for (GPUTexture& texture : reconstructed_) discard(texture);
    discard(output_);
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(transitions.size());
    dependency.pImageMemoryBarriers = transitions.data();
    vkCmdPipelineBarrier2(commandBuffer, &dependency);

    // Fresh histogram for this evaluation (the sigma pass of the previous
    // one may still be reading it: execution dependency first).
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(commandBuffer, histogram_.buffer, 0, VK_WHOLE_SIZE, 0);
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // 1. Full-size input -> G_1 (+ noise histogram).
    const GPUTexture& g1 = gaussian_[0];
    const SizesPush down0{{toInt(input_.width), toInt(input_.height)}, {toInt(g1.width), toInt(g1.height)}};
    dispatch(commandBuffer, DOWN0, down0Set_, &down0, sizeof down0, groups(g1.width, DH_DENOISE_GROUP),
             groups(g1.height, DH_DENOISE_GROUP));
    computeToCompute(commandBuffer);

    // 2. Remaining pyramid levels.
    for (std::uint32_t k = 1; k < levels_; ++k) {
        const GPUTexture& src = gaussian_[k - 1];
        const GPUTexture& dst = gaussian_[k];
        const SizesPush push{{toInt(src.width), toInt(src.height)}, {toInt(dst.width), toInt(dst.height)}};
        dispatch(commandBuffer, DOWN, downSets_[k - 1], &push, sizeof push, groups(dst.width, DH_DENOISE_GROUP),
                 groups(dst.height, DH_DENOISE_GROUP));
        computeToCompute(commandBuffer);
    }

    // 3. Noise estimate and thresholds.
    const float sigmaPush[4] = {params_.luminance, params_.chrominance, params_.detail, params_.noiseLevel};
    dispatch(commandBuffer, SIGMA, sigmaSet_, sigmaPush, sizeof sigmaPush, 1, 1);
    computeToCompute(commandBuffer);

    // 4. Reconstruction, coarse to fine.
    for (std::uint32_t k = levels_ - 1; k >= 1; --k) {
        const GPUTexture& fine = gaussian_[k - 1];
        const GPUTexture& coarse = gaussian_[k];
        const UpPush push{{toInt(fine.width), toInt(fine.height)}, {toInt(coarse.width), toInt(coarse.height)}, k};
        dispatch(commandBuffer, UP, upSets_[k - 1], &push, sizeof push, groups(fine.width, kUpGroup),
                 groups(fine.height, kUpGroup));
        computeToCompute(commandBuffer);
    }

    // 5. Finest band, back to linear RGB.
    const FinalPush final{{toInt(output_.width), toInt(output_.height)}, {toInt(g1.width), toInt(g1.height)},
                          params_.detail};
    dispatch(commandBuffer, FINAL, finalSet_, &final, sizeof final, groups(output_.width, kUpGroup),
             groups(output_.height, kUpGroup));

    // Sigma readback: the host reads the thresholds buffer once the
    // submission's fence has signalled.
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    measured_ = true;
}

void DenoiseNode::updateUniforms(std::span<const std::byte> packedParams) {
    if (packedParams.size() != sizeof(DenoiseParams)) {
        throw std::invalid_argument("DenoiseNode::updateUniforms expects " + std::to_string(sizeof(DenoiseParams)) +
                                    " bytes, got " + std::to_string(packedParams.size()));
    }
    DenoiseParams params;
    std::memcpy(&params, packedParams.data(), sizeof params);
    setParams(params);
}

std::vector<std::byte> DenoiseNode::pack(const DenoiseParams& params) {
    std::vector<std::byte> bytes(sizeof(DenoiseParams));
    std::memcpy(bytes.data(), &params, sizeof params);
    return bytes;
}

DenoiseStatistics DenoiseNode::statistics() const noexcept {
    DenoiseStatistics stats;
    stats.levels = levels_;
    if (measured_ && thresholds_.mapped) {
        const auto* values = static_cast<const float*>(thresholds_.mapped);
        for (std::size_t c = 0; c < 3; ++c) stats.sigma[c] = values[DH_DENOISE_MAX_LEVELS * 4 + c];
    }
    return stats;
}

}  // namespace darkhouse
