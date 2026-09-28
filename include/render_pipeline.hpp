// DarkHouse — GPU render pipeline.
//
// Develop and adjustment operators are Vulkan compute nodes in a DAG. The graph
// records every node into one command buffer in topological order and places
// synchronization2 barriers between producers and consumers. Images are
// scene-referred linear RGBA16F throughout, which leaves headroom for HDR RAW
// data and keeps bandwidth at half of FP32.
#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef VK_API_VERSION_1_3
#error "DarkHouse requires Vulkan 1.3 headers (VK_API_VERSION_1_3 is not defined)"
#endif

namespace darkhouse {

enum class PixelFormat : std::uint8_t {
    R8G8B8A8_UNORM,
    R16G16B16A16_SFLOAT,
    R32G32B32A32_SFLOAT,
};

[[nodiscard]] VkFormat toVkFormat(PixelFormat format) noexcept;
[[nodiscard]] std::uint32_t bytesPerPixel(PixelFormat format) noexcept;

// Plain handle bundle. It owns nothing; VulkanContext::createTexture() and
// destroyTexture() manage its lifetime.
struct GPUTexture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::R16G16B16A16_SFLOAT;
    // Layout the image will be in once every command recorded so far has run.
    // Barrier helpers keep it up to date.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    [[nodiscard]] bool valid() const noexcept { return image != VK_NULL_HANDLE; }
};

struct VulkanContextOptions {
    std::string applicationName = "DarkHouse";
    bool enableValidation = false;  // uses VK_LAYER_KHRONOS_validation when it is installed
};

// Instance, device and compute queue. Selects a Vulkan 1.3 device with
// synchronization2 and prefers discrete GPUs. On macOS it enables portability
// enumeration so MoltenVK is found.
// Not thread-safe: use it from the render thread only.
class VulkanContext {
public:
    explicit VulkanContext(const VulkanContextOptions& options);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return physicalDevice_; }
    [[nodiscard]] VkDevice device() const noexcept { return device_; }
    [[nodiscard]] VkQueue computeQueue() const noexcept { return computeQueue_; }
    [[nodiscard]] std::uint32_t computeQueueFamily() const noexcept { return computeQueueFamily_; }
    [[nodiscard]] const std::string& deviceName() const noexcept { return deviceName_; }
    [[nodiscard]] bool validationEnabled() const noexcept { return validationEnabled_; }

    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required) const;

    [[nodiscard]] GPUTexture createTexture(std::uint32_t width, std::uint32_t height, PixelFormat format,
                                           VkImageUsageFlags usage) const;
    // Safe to call on an empty texture. Resets `texture` to its default state.
    void destroyTexture(GPUTexture& texture) const noexcept;

    [[nodiscard]] VkShaderModule loadShaderModule(const std::filesystem::path& spirvPath) const;

    // Records into a one-shot command buffer, submits it to the compute queue
    // and blocks until the GPU finishes.
    void submitAndWait(const std::function<void(VkCommandBuffer)>& record) const;

    // One tightly packed texel rectangle to copy into a texture.
    struct RegionUpload {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::span<const std::byte> texels;  // width * height * bytesPerPixel(format)
    };
    // Copies all regions through one staging buffer and one submission.
    // Leaves the texture in VK_IMAGE_LAYOUT_GENERAL.
    void uploadRegions(GPUTexture& texture, std::span<const RegionUpload> regions) const;

    // Fills the texture with a solid colour. Needs TRANSFER_DST usage and
    // leaves the texture in VK_IMAGE_LAYOUT_GENERAL.
    void clearTexture(GPUTexture& texture, float r, float g, float b, float a) const;

private:
    void createInstance(const VulkanContextOptions& options);
    void pickPhysicalDevice();
    void createDevice();
    void destroy() noexcept;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    std::uint32_t computeQueueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    std::string deviceName_;
    bool validationEnabled_ = false;
};

// synchronization2 helpers (core in Vulkan 1.3). recordImageBarrier moves
// `texture` from its tracked layout to `newLayout` and updates texture.layout.
// Pass discardContents when the next access overwrites every texel.
void recordImageBarrier(VkCommandBuffer commandBuffer, GPUTexture& texture, VkImageLayout newLayout,
                        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                        VkAccessFlags2 dstAccess, bool discardContents = false);
void recordMemoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

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

// Push-constant block of shaders/exposure.comp. The layout must match the shader.
struct ExposureParams {
    float exposureEV = 0.0f;  // stops, clamped to [-10, 10]; linear gain is 2^EV
    float highlights = 0.0f;  // [-1, 1]; negative recovers highlights
    float shadows = 0.0f;     // [-1, 1]; positive lifts shadows
    float contrast = 0.0f;    // [-1, 1]; 0 = identity, pivots on middle grey
};
static_assert(sizeof(ExposureParams) == 16, "ExposureParams must match the shader push-constant block");

class ExposureNode final : public ComputeNode {
public:
    static constexpr std::string_view kTypeName = "exposure";
    static constexpr std::uint32_t kWorkgroupSize = 16;  // local_size_x/y in exposure.comp

    // Loads <shaderDirectory>/exposure.spv.
    ExposureNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory);
    ~ExposureNode() override;

    ExposureNode(const ExposureNode&) = delete;
    ExposureNode& operator=(const ExposureNode&) = delete;

    [[nodiscard]] std::string_view typeName() const noexcept override { return kTypeName; }
    [[nodiscard]] std::uint32_t inputCount() const noexcept override { return 1; }

    void setInputTexture(std::uint32_t slot, const GPUTexture& texture) override;
    [[nodiscard]] const GPUTexture& getOutputTexture() const override { return output_; }
    void executeCompute(VkCommandBuffer commandBuffer) override;
    void updateUniforms(std::span<const std::byte> packedParams) override;

    void setParams(const ExposureParams& params) noexcept;
    [[nodiscard]] const ExposureParams& params() const noexcept { return params_; }

    // Serialized form: 4 x IEEE-754 float32 in host byte order (little-endian on
    // every supported target).
    [[nodiscard]] static std::vector<std::byte> pack(const ExposureParams& params);

private:
    void writeDescriptors();
    void destroy() noexcept;

    const VulkanContext& context_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    GPUTexture input_;
    GPUTexture output_;
    ExposureParams params_;
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
