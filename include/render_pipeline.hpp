// DarkHouse — GPU render pipeline.
//
// Develop and adjustment operators are Vulkan compute nodes in a DAG. The graph
// records every node into one command buffer in topological order and places
// synchronization2 barriers between producers and consumers. The device,
// textures and barrier helpers live in vulkan_context.hpp.
#pragma once

#include "vulkan_context.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace darkhouse {

// -----------------------------------------------------------------------------
// Node graph
// -----------------------------------------------------------------------------

// One GPU operator. Inputs are storage images in VK_IMAGE_LAYOUT_GENERAL. A node
// owns its output texture and leaves it in GENERAL after executeCompute().
// setInputTexture() may rewrite descriptors or reallocate the output, so only
// call it when no submitted work that uses this node is still in flight.
class ComputeNode {
public:
    virtual ~ComputeNode() = default;

    [[nodiscard]] virtual std::string_view typeName() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t inputCount() const noexcept = 0;

    virtual void setInputTexture(std::uint32_t slot, const GPUTexture& texture) = 0;
    [[nodiscard]] virtual const GPUTexture& getOutputTexture() const = 0;
    virtual void executeCompute(VkCommandBuffer commandBuffer) = 0;
    // `packedParams` has the same bytes as edit_nodes.serialized_params.
    virtual void updateUniforms(std::span<const std::byte> packedParams) = 0;
};

// A one-input, one-output operator: a single compute pipeline over the whole
// image with 16x16 workgroups, its parameters in push constants. The input
// must be rgba16f; the output is allocated in `outputFormat` at the input size.
class PointOperatorNode : public ComputeNode {
public:
    static constexpr std::uint32_t kWorkgroupSize = 16;  // local_size_x/y of the shaders

    ~PointOperatorNode() override;

    PointOperatorNode(const PointOperatorNode&) = delete;
    PointOperatorNode& operator=(const PointOperatorNode&) = delete;

    [[nodiscard]] std::uint32_t inputCount() const noexcept override { return 1; }

    void setInputTexture(std::uint32_t slot, const GPUTexture& texture) override;
    [[nodiscard]] const GPUTexture& getOutputTexture() const override { return output_; }
    void executeCompute(VkCommandBuffer commandBuffer) override;

protected:
    // Loads `shaderPath` (SPIR-V). `pushConstantSize` may be 0.
    PointOperatorNode(const VulkanContext& context, const std::filesystem::path& shaderPath,
                      std::uint32_t pushConstantSize, PixelFormat outputFormat);

    // pushConstantSize bytes, read whenever the node is recorded.
    [[nodiscard]] virtual const void* pushConstants() const noexcept { return nullptr; }

private:
    void writeDescriptors();
    void destroy() noexcept;

    const VulkanContext& context_;
    std::uint32_t pushConstantSize_;
    PixelFormat outputFormat_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    GPUTexture input_;
    GPUTexture output_;
};

// Push-constant block of shaders/exposure.comp. The layout must match the shader.
struct ExposureParams {
    float exposureEV = 0.0f;  // stops, clamped to [-10, 10]; linear gain is 2^EV
    float highlights = 0.0f;  // [-1, 1]; negative recovers highlights
    float shadows = 0.0f;     // [-1, 1]; positive lifts shadows
    float contrast = 0.0f;    // [-1, 1]; 0 = identity, pivots on middle grey
};
static_assert(sizeof(ExposureParams) == 16, "ExposureParams must match the shader push-constant block");

class ExposureNode final : public PointOperatorNode {
public:
    static constexpr std::string_view kTypeName = "exposure";

    // Loads <shaderDirectory>/exposure.spv.
    ExposureNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory);

    [[nodiscard]] std::string_view typeName() const noexcept override { return kTypeName; }
    void updateUniforms(std::span<const std::byte> packedParams) override;

    void setParams(const ExposureParams& params) noexcept;
    [[nodiscard]] const ExposureParams& params() const noexcept { return params_; }

    // Serialized form: 4 x IEEE-754 float32 in host byte order (little-endian on
    // every supported target).
    [[nodiscard]] static std::vector<std::byte> pack(const ExposureParams& params);

private:
    [[nodiscard]] const void* pushConstants() const noexcept override { return &params_; }

    ExposureParams params_;
};

// The view transform at the end of the develop graph: scene-linear rgba16f to
// sRGB-encoded RGBA8 for the UNORM swapchain the UI draws into (values are
// clipped to [0, 1], with a triangular dither of +-1 LSB against banding).
// Not a develop step: it is never stored in an edit stack.
class DisplayTransformNode final : public PointOperatorNode {
public:
    static constexpr std::string_view kTypeName = "display_srgb";

    // Loads <shaderDirectory>/display_srgb.spv.
    DisplayTransformNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory);

    [[nodiscard]] std::string_view typeName() const noexcept override { return kTypeName; }
    void updateUniforms(std::span<const std::byte> packedParams) override;  // takes no parameters
};

// Maps an edit_nodes.node_type string to a node. Throws std::invalid_argument
// for unknown types.
[[nodiscard]] std::unique_ptr<ComputeNode> createComputeNode(std::string_view nodeType, const VulkanContext& context,
                                                             const std::filesystem::path& shaderDirectory);

class RenderPipelineGraph {
public:
    using NodeId = std::uint32_t;

    NodeId addNode(std::unique_ptr<ComputeNode> node);

    // Feeds `source`'s output into input `targetSlot` of `target`. A slot has
    // exactly one producer, so reconnecting replaces the previous binding.
    // Throws std::invalid_argument if the edge would create a cycle.
    void connectNodes(NodeId source, NodeId target, std::uint32_t targetSlot = 0);

    // Binds a texture owned outside the graph (the uploaded canvas, a decoded
    // RAW) to an input slot. `texture` must outlive the binding.
    void bindExternalInput(NodeId target, std::uint32_t targetSlot, GPUTexture& texture);

    // Records every node in topological order, with barriers between dependent
    // nodes. Throws std::logic_error if any input slot is unbound.
    void evaluateGraph(VkCommandBuffer commandBuffer);

    [[nodiscard]] ComputeNode& node(NodeId id);
    [[nodiscard]] const GPUTexture& outputOf(NodeId id) const;
    [[nodiscard]] std::vector<NodeId> sinkNodes() const;  // nodes whose output feeds no other node
    [[nodiscard]] const std::vector<NodeId>& executionOrder();
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
    void clear() noexcept;

private:
    struct Connection {
        NodeId source;
        NodeId target;
        std::uint32_t slot;
    };
    struct ExternalBinding {
        NodeId target;
        std::uint32_t slot;
        GPUTexture* texture;
    };

    void requireNode(NodeId id) const;
    void requireSlot(NodeId id, std::uint32_t slot) const;
    void unbindSlot(NodeId target, std::uint32_t slot);
    [[nodiscard]] bool reachable(NodeId from, NodeId to) const;
    void rebuildExecutionOrder();
    void validateInputsBound() const;

    std::vector<std::unique_ptr<ComputeNode>> nodes_;
    std::vector<Connection> connections_;
    std::vector<ExternalBinding> externalInputs_;
    std::vector<NodeId> executionOrder_;
    bool orderDirty_ = true;
};

}  // namespace darkhouse
