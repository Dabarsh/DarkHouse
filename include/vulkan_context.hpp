// DarkHouse — Vulkan 1.3 device context.
//
// Owns the instance, the physical/logical device and the queue the develop
// graph runs on, plus texture and upload helpers. Images are scene-referred
// linear RGBA16F throughout, which leaves headroom for HDR RAW data and keeps
// bandwidth at half of FP32.
//
// The same context drives the desktop UI: when presentation is requested it
// also owns the window surface, selects a queue family that can present to
// it, and enables VK_KHR_swapchain and dynamic rendering. Compute, graphics
// and present then share one queue, so the viewport can sample develop
// outputs without cross-queue ownership transfers.
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
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

// Plain buffer handle bundle, managed by VulkanContext::createBuffer() and
// destroyBuffer(). Host-visible buffers stay persistently mapped.
struct GPUBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;  // non-null for host-visible (coherent) buffers

    [[nodiscard]] bool valid() const noexcept { return buffer != VK_NULL_HANDLE; }
};

struct VulkanContextOptions {
    std::string applicationName = "DarkHouse";
    // Uses VK_LAYER_KHRONOS_validation when it is installed and routes its
    // messages to the DarkHouse log through VK_EXT_debug_utils.
    bool enableValidation = false;

    // Presentation (optional). Leave both empty for a headless compute context.
    // instanceExtensions: extra instance extensions, e.g. from
    //   PlatformWindow::requiredInstanceExtensions().
    // createSurface: called once, right after the instance is created. The
    //   context takes ownership of the returned surface.
    std::vector<std::string> instanceExtensions;
    std::function<VkSurfaceKHR(VkInstance)> createSurface;
};

// Instance, device and one universal queue. Selects a Vulkan 1.3 device with
// synchronization2 and dynamic rendering (plus swapchain and present support
// when a surface is requested) and prefers discrete GPUs, then the most
// device-local memory. On macOS it enables portability enumeration so
// MoltenVK is found.
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
    // The universal queue: compute always; graphics and present when presenting.
    [[nodiscard]] VkQueue computeQueue() const noexcept { return computeQueue_; }
    [[nodiscard]] std::uint32_t computeQueueFamily() const noexcept { return computeQueueFamily_; }
    [[nodiscard]] VkQueue queue() const noexcept { return computeQueue_; }
    [[nodiscard]] std::uint32_t queueFamily() const noexcept { return computeQueueFamily_; }
    [[nodiscard]] std::uint32_t apiVersion() const noexcept { return VK_API_VERSION_1_3; }
    [[nodiscard]] const std::string& deviceName() const noexcept { return deviceName_; }
    [[nodiscard]] VkPhysicalDeviceType deviceType() const noexcept { return deviceType_; }
    [[nodiscard]] bool validationEnabled() const noexcept { return validationEnabled_; }
    // GPU timestamps on the universal queue (see GpuTimestamps): nanoseconds
    // per tick, and how many low bits of a timestamp are valid (0 = none).
    [[nodiscard]] double timestampPeriodNs() const noexcept { return timestampPeriodNs_; }
    [[nodiscard]] std::uint32_t timestampValidBits() const noexcept { return timestampValidBits_; }
    // Validation messages of error severity seen so far (0 without validation).
    [[nodiscard]] std::uint32_t validationErrorCount() const noexcept;

    // VK_NULL_HANDLE for a headless context.
    [[nodiscard]] VkSurfaceKHR surface() const noexcept { return surface_; }
    [[nodiscard]] bool presentationEnabled() const noexcept { return surface_ != VK_NULL_HANDLE; }

    // Blocks until the device has finished all submitted work. Call before
    // destroying resources that in-flight frames may still reference.
    void waitIdle() const noexcept;

    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required) const;

    [[nodiscard]] GPUTexture createTexture(std::uint32_t width, std::uint32_t height, PixelFormat format,
                                           VkImageUsageFlags usage) const;
    // Safe to call on an empty texture. Resets `texture` to its default state.
    void destroyTexture(GPUTexture& texture) const noexcept;

    // hostVisible: HOST_VISIBLE | HOST_COHERENT memory, persistently mapped
    // (staging, readback, small parameter blocks); otherwise DEVICE_LOCAL.
    [[nodiscard]] GPUBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) const;
    // Safe to call on an empty buffer. Resets `buffer` to its default state.
    void destroyBuffer(GPUBuffer& buffer) const noexcept;

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

    // Copies the whole texture to host memory (tightly packed texels) and
    // blocks until done. Needs TRANSFER_SRC usage; leaves the texture in
    // VK_IMAGE_LAYOUT_GENERAL. For tests, export and tools, not per frame.
    [[nodiscard]] std::vector<std::byte> downloadTexture(GPUTexture& texture) const;

private:
    void createInstance(const VulkanContextOptions& options);
    void createDebugMessenger();
    void pickPhysicalDevice();
    void createDevice();
    void destroy() noexcept;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    std::uint32_t computeQueueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    std::string deviceName_;
    VkPhysicalDeviceType deviceType_ = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    double timestampPeriodNs_ = 0.0;
    std::uint32_t timestampValidBits_ = 0;
    bool validationEnabled_ = false;
    std::atomic<std::uint32_t> validationErrors_{0};  // written by the debug-utils callback
};

// Times GPU work recorded into command buffers: reset(), then write(i) at
// points of interest, submit, and once the submission has completed read the
// intervals. Every call is a no-op (and intervals() empty) on a device
// without timestamp support. Must outlive the submissions that use it.
class GpuTimestamps {
public:
    GpuTimestamps(const VulkanContext& context, std::uint32_t capacity);
    ~GpuTimestamps();

    GpuTimestamps(const GpuTimestamps&) = delete;
    GpuTimestamps& operator=(const GpuTimestamps&) = delete;

    [[nodiscard]] bool supported() const noexcept { return pool_ != VK_NULL_HANDLE; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

    // Resets the first `count` timestamps; record it before writing them.
    void reset(VkCommandBuffer commandBuffer, std::uint32_t count) const;
    // Timestamp `index`, taken once all previously recorded commands finish.
    void write(VkCommandBuffer commandBuffer, std::uint32_t index) const;
    // Milliseconds from timestamp i to i + 1, for i < count - 1. Empty if the
    // results are not available.
    [[nodiscard]] std::vector<double> intervals(std::uint32_t count) const;

private:
    const VulkanContext& context_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    std::uint32_t capacity_ = 0;
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
