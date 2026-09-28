#include "render_pipeline.hpp"

#include "color_nodes.hpp"
#include "denoise_node.hpp"
#include "vulkan_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>

namespace darkhouse {
namespace {

std::uint32_t divideRoundUp(std::uint32_t value, std::uint32_t divisor) { return (value + divisor - 1) / divisor; }

float finiteClamp(float value, float lo, float hi) { return std::isfinite(value) ? std::clamp(value, lo, hi) : 0.0f; }

}  // namespace

// -----------------------------------------------------------------------------
// PointOperatorNode
// -----------------------------------------------------------------------------

PointOperatorNode::PointOperatorNode(const VulkanContext& context, const std::filesystem::path& shaderPath,
                                     std::uint32_t pushConstantSize, PixelFormat outputFormat)
    : context_(context), pushConstantSize_(pushConstantSize), outputFormat_(outputFormat) {
    const VkDevice device = context_.device();
    try {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;  // 0 = input, 1 = output
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0;
        pushRange.size = pushConstantSize_;
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout_;
        pipelineLayoutInfo.pushConstantRangeCount = pushConstantSize_ > 0 ? 1 : 0;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        checkVk(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout_),
                "vkCreatePipelineLayout");

        const VkShaderModule module = context_.loadShaderModule(shaderPath);
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

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = 2;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = descriptorPool_;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &setLayout_;
        checkVk(vkAllocateDescriptorSets(device, &setInfo, &descriptorSet_), "vkAllocateDescriptorSets");
    } catch (...) {
        destroy();
        throw;
    }
}

PointOperatorNode::~PointOperatorNode() { destroy(); }

void PointOperatorNode::destroy() noexcept {
    const VkDevice device = context_.device();
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool_, nullptr);  // frees the set
    if (setLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    context_.destroyTexture(output_);
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

void PointOperatorNode::setInputTexture(std::uint32_t slot, const GPUTexture& texture) {
    const std::string name(typeName());
    if (slot != 0) throw std::out_of_range(name + " node has a single input (slot 0)");
    if (!texture.valid()) throw std::invalid_argument(name + " node: input texture is not allocated");
    if (texture.format != PixelFormat::R16G16B16A16_SFLOAT) {
        throw std::invalid_argument(name + " node: input must be R16G16B16A16_SFLOAT (rgba16f)");
    }

    bool rewrite = texture.view != input_.view;
    input_ = texture;
    if (!output_.valid() || output_.width != texture.width || output_.height != texture.height) {
        context_.destroyTexture(output_);
        output_ = context_.createTexture(
            texture.width, texture.height, outputFormat_,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        rewrite = true;
    }
    if (rewrite) writeDescriptors();
}

void PointOperatorNode::writeDescriptors() {
    std::array<VkDescriptorImageInfo, 2> images{};
    images[0].imageView = input_.view;
    images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[1].imageView = output_.view;
    images[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(context_.device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void PointOperatorNode::executeCompute(VkCommandBuffer commandBuffer) {
    if (!input_.valid() || !output_.valid()) {
        throw std::logic_error(std::string(typeName()) + " node: executeCompute called before setInputTexture");
    }
    // Every output texel is rewritten, so the old contents can be discarded.
    // Only a WAR execution dependency on earlier readers is needed.
    recordImageBarrier(commandBuffer, output_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       /*discardContents=*/true);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0,
                            nullptr);
    if (pushConstantSize_ > 0) {
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantSize_,
                           pushConstants());
    }
    vkCmdDispatch(commandBuffer, divideRoundUp(output_.width, kWorkgroupSize),
                  divideRoundUp(output_.height, kWorkgroupSize), 1);
}

// -----------------------------------------------------------------------------
// ExposureNode
// -----------------------------------------------------------------------------

ExposureNode::ExposureNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : PointOperatorNode(context, shaderDirectory / "exposure.spv", sizeof(ExposureParams),
                        PixelFormat::R16G16B16A16_SFLOAT) {}

void ExposureNode::updateUniforms(std::span<const std::byte> packedParams) {
    if (packedParams.size() != sizeof(ExposureParams)) {
        throw std::invalid_argument("ExposureNode::updateUniforms expects " + std::to_string(sizeof(ExposureParams)) +
                                    " bytes, got " + std::to_string(packedParams.size()));
    }
    ExposureParams params;
    std::memcpy(&params, packedParams.data(), sizeof params);
    setParams(params);
}

void ExposureNode::setParams(const ExposureParams& params) noexcept {
    params_.exposureEV = finiteClamp(params.exposureEV, -10.0f, 10.0f);
    params_.highlights = finiteClamp(params.highlights, -1.0f, 1.0f);
    params_.shadows = finiteClamp(params.shadows, -1.0f, 1.0f);
    params_.contrast = finiteClamp(params.contrast, -1.0f, 1.0f);
}

std::vector<std::byte> ExposureNode::pack(const ExposureParams& params) {
    std::vector<std::byte> bytes(sizeof(ExposureParams));
    std::memcpy(bytes.data(), &params, sizeof params);
    return bytes;
}

// -----------------------------------------------------------------------------
// DisplayTransformNode
// -----------------------------------------------------------------------------

DisplayTransformNode::DisplayTransformNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : PointOperatorNode(context, shaderDirectory / "display_srgb.spv", 0, PixelFormat::R8G8B8A8_UNORM) {}

void DisplayTransformNode::updateUniforms(std::span<const std::byte> packedParams) {
    if (!packedParams.empty()) throw std::invalid_argument("DisplayTransformNode takes no parameters");
}

std::unique_ptr<ComputeNode> createComputeNode(std::string_view nodeType, const VulkanContext& context,
                                               const std::filesystem::path& shaderDirectory) {
    if (nodeType == ExposureNode::kTypeName) return std::make_unique<ExposureNode>(context, shaderDirectory);
    if (nodeType == DenoiseNode::kTypeName) return std::make_unique<DenoiseNode>(context, shaderDirectory);
    if (nodeType == WhiteBalanceNode::kTypeName) return std::make_unique<WhiteBalanceNode>(context, shaderDirectory);
    if (nodeType == HslNode::kTypeName) return std::make_unique<HslNode>(context, shaderDirectory);
    if (nodeType == ColorGradingNode::kTypeName) return std::make_unique<ColorGradingNode>(context, shaderDirectory);
    throw std::invalid_argument("unknown compute node type '" + std::string(nodeType) + "'");
}

// -----------------------------------------------------------------------------
// RenderPipelineGraph
// -----------------------------------------------------------------------------

RenderPipelineGraph::NodeId RenderPipelineGraph::addNode(std::unique_ptr<ComputeNode> node) {
    if (!node) throw std::invalid_argument("RenderPipelineGraph::addNode: node is null");
    nodes_.push_back(std::move(node));
    orderDirty_ = true;
    return static_cast<NodeId>(nodes_.size() - 1);
}

void RenderPipelineGraph::requireNode(NodeId id) const {
    if (id >= nodes_.size()) throw std::out_of_range("RenderPipelineGraph: unknown node id " + std::to_string(id));
}

void RenderPipelineGraph::requireSlot(NodeId id, std::uint32_t slot) const {
    requireNode(id);
    if (slot >= nodes_[id]->inputCount()) {
        throw std::out_of_range("RenderPipelineGraph: node " + std::to_string(id) + " has no input slot " +
                                std::to_string(slot));
    }
}

void RenderPipelineGraph::unbindSlot(NodeId target, std::uint32_t slot) {
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                      [&](const Connection& c) { return c.target == target && c.slot == slot; }),
                       connections_.end());
    externalInputs_.erase(std::remove_if(externalInputs_.begin(), externalInputs_.end(),
                                         [&](const ExternalBinding& b) { return b.target == target && b.slot == slot; }),
                          externalInputs_.end());
}

void RenderPipelineGraph::connectNodes(NodeId source, NodeId target, std::uint32_t targetSlot) {
    requireNode(source);
    requireSlot(target, targetSlot);
    if (source == target || reachable(target, source)) {
        throw std::invalid_argument("RenderPipelineGraph::connectNodes: " + std::to_string(source) + " -> " +
                                    std::to_string(target) + " would create a cycle");
    }
    unbindSlot(target, targetSlot);
    connections_.push_back({source, target, targetSlot});
    orderDirty_ = true;
}

void RenderPipelineGraph::bindExternalInput(NodeId target, std::uint32_t targetSlot, GPUTexture& texture) {
    requireSlot(target, targetSlot);
    unbindSlot(target, targetSlot);
    externalInputs_.push_back({target, targetSlot, &texture});
    orderDirty_ = true;
}

bool RenderPipelineGraph::reachable(NodeId from, NodeId to) const {
    std::vector<bool> seen(nodes_.size(), false);
    std::vector<NodeId> stack{from};
    while (!stack.empty()) {
        const NodeId current = stack.back();
        stack.pop_back();
        if (current == to) return true;
        if (seen[current]) continue;
        seen[current] = true;
        for (const Connection& c : connections_) {
            if (c.source == current && !seen[c.target]) stack.push_back(c.target);
        }
    }
    return false;
}

void RenderPipelineGraph::rebuildExecutionOrder() {
    // Kahn's algorithm, seeded in id order so the result is deterministic.
    std::vector<std::uint32_t> inDegree(nodes_.size(), 0);
    for (const Connection& c : connections_) ++inDegree[c.target];
    std::deque<NodeId> ready;
    for (NodeId id = 0; id < nodes_.size(); ++id) {
        if (inDegree[id] == 0) ready.push_back(id);
    }
    executionOrder_.clear();
    executionOrder_.reserve(nodes_.size());
    while (!ready.empty()) {
        const NodeId id = ready.front();
        ready.pop_front();
        executionOrder_.push_back(id);
        for (const Connection& c : connections_) {
            if (c.source == id && --inDegree[c.target] == 0) ready.push_back(c.target);
        }
    }
    if (executionOrder_.size() != nodes_.size()) {
        throw std::logic_error("RenderPipelineGraph: cycle detected");  // prevented by connectNodes()
    }
    orderDirty_ = false;
}

const std::vector<RenderPipelineGraph::NodeId>& RenderPipelineGraph::executionOrder() {
    if (orderDirty_) rebuildExecutionOrder();
    return executionOrder_;
}

void RenderPipelineGraph::validateInputsBound() const {
    for (NodeId id = 0; id < nodes_.size(); ++id) {
        for (std::uint32_t slot = 0; slot < nodes_[id]->inputCount(); ++slot) {
            const bool connected =
                std::any_of(connections_.begin(), connections_.end(),
                            [&](const Connection& c) { return c.target == id && c.slot == slot; }) ||
                std::any_of(externalInputs_.begin(), externalInputs_.end(),
                            [&](const ExternalBinding& b) { return b.target == id && b.slot == slot; });
            if (!connected) {
                throw std::logic_error("RenderPipelineGraph: input " + std::to_string(slot) + " of node " +
                                       std::to_string(id) + " (" + std::string(nodes_[id]->typeName()) +
                                       ") is not bound");
            }
        }
    }
}

void RenderPipelineGraph::evaluateGraph(VkCommandBuffer commandBuffer) {
    if (nodes_.empty()) return;
    validateInputsBound();
    const std::vector<NodeId>& order = executionOrder();

    // Timestamp 0 before the first node, i + 1 after node i. The pool only
    // grows, and never while an earlier evaluation can still be running (see
    // ComputeNode::setInputTexture: the same rule holds for the whole graph).
    const auto timestampCount = static_cast<std::uint32_t>(order.size() + 1);
    if (profilingContext_ && (!timestamps_ || timestamps_->capacity() < timestampCount)) {
        timestamps_ = std::make_unique<GpuTimestamps>(*profilingContext_, std::max<std::uint32_t>(timestampCount, 16));
    }
    const GpuTimestamps* timer = timestamps_ && timestamps_->supported() ? timestamps_.get() : nullptr;
    if (timer) timer->reset(commandBuffer, timestampCount);
    timedOrder_.clear();

    // External inputs were written outside the graph (uploads, earlier passes).
    // Make those writes visible to compute reads and move the images to GENERAL.
    for (ExternalBinding& binding : externalInputs_) {
        recordImageBarrier(commandBuffer, *binding.texture, VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    if (timer) timer->write(commandBuffer, 0);

    for (std::size_t i = 0; i < order.size(); ++i) {
        const NodeId id = order[i];
        ComputeNode& current = *nodes_[id];
        for (const Connection& c : connections_) {
            if (c.target == id) current.setInputTexture(c.slot, nodes_[c.source]->getOutputTexture());
        }
        for (const ExternalBinding& binding : externalInputs_) {
            if (binding.target == id) current.setInputTexture(binding.slot, *binding.texture);
        }
        current.executeCompute(commandBuffer);

        // Publish this node's storage writes. Nodes inside the graph read them
        // in the compute stage; after the last node, anything may consume them
        // (display sampling, readback, export).
        const bool last = i + 1 == order.size();
        recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            last ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            last ? VK_ACCESS_2_MEMORY_READ_BIT : VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        if (timer) timer->write(commandBuffer, static_cast<std::uint32_t>(i + 1));
    }
    if (timer) timedOrder_ = order;
}

void RenderPipelineGraph::enableProfiling(const VulkanContext& context) { profilingContext_ = &context; }

std::vector<RenderPipelineGraph::NodeTiming> RenderPipelineGraph::readTimings() const {
    if (!timestamps_ || timedOrder_.empty()) return {};
    const std::vector<double> intervals = timestamps_->intervals(static_cast<std::uint32_t>(timedOrder_.size() + 1));
    if (intervals.size() != timedOrder_.size()) return {};
    std::vector<NodeTiming> timings;
    timings.reserve(timedOrder_.size());
    for (std::size_t i = 0; i < timedOrder_.size(); ++i) {
        const NodeId id = timedOrder_[i];
        timings.push_back({id, id < nodes_.size() ? std::string(nodes_[id]->typeName()) : std::string(), intervals[i]});
    }
    return timings;
}

ComputeNode& RenderPipelineGraph::node(NodeId id) {
    requireNode(id);
    return *nodes_[id];
}

const GPUTexture& RenderPipelineGraph::outputOf(NodeId id) const {
    requireNode(id);
    return nodes_[id]->getOutputTexture();
}

std::vector<RenderPipelineGraph::NodeId> RenderPipelineGraph::sinkNodes() const {
    std::vector<NodeId> sinks;
    for (NodeId id = 0; id < nodes_.size(); ++id) {
        const bool feedsAnother = std::any_of(connections_.begin(), connections_.end(),
                                              [&](const Connection& c) { return c.source == id; });
        if (!feedsAnother) sinks.push_back(id);
    }
    return sinks;
}

void RenderPipelineGraph::clear() noexcept {
    connections_.clear();
    externalInputs_.clear();
    executionOrder_.clear();
    nodes_.clear();
    timedOrder_.clear();  // the query pool stays for the next nodes
    orderDirty_ = true;
}

}  // namespace darkhouse
