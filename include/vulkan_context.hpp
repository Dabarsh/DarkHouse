// DarkHouse — Vulkan 1.3 device context.
//
// Owns the instance, the physical/logical device and the queue the develop
// graph runs on, plus texture and upload helpers. Images are scene-referred
// linear RGBA16F throughout, which leaves headroom for HDR RAW data and keeps
// bandwidth at half of FP32.
#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

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

}  // namespace darkhouse
