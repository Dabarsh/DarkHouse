#include "render_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace darkhouse {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
// Defined in vulkan_beta.h, which is not included by default.
constexpr const char* kPortabilitySubsetExtension = "VK_KHR_portability_subset";

template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F fn) : fn_(std::move(fn)) {}
    ~ScopeExit() { fn_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F fn_;
};

std::string resultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
    }
}

void checkVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + resultName(result));
}

std::uint32_t divideRoundUp(std::uint32_t value, std::uint32_t divisor) { return (value + divisor - 1) / divisor; }

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

VkImageSubresourceRange colorRange() {
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    return range;
}

bool instanceLayerAvailable(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
    return std::any_of(layers.begin(), layers.end(),
                       [&](const VkLayerProperties& layer) { return std::strcmp(layer.layerName, name) == 0; });
}

bool instanceExtensionAvailable(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const VkExtensionProperties& ext) { return std::strcmp(ext.extensionName, name) == 0; });
}

bool deviceExtensionAvailable(VkPhysicalDevice device, const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const VkExtensionProperties& ext) { return std::strcmp(ext.extensionName, name) == 0; });
}

// Prefers a universal (graphics + compute) family so the future UI front-end
// can present from the queue the develop graph runs on.
std::optional<std::uint32_t> findComputeQueueFamily(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    std::optional<std::uint32_t> computeOnly;
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) == 0) continue;
        if (flags & VK_QUEUE_GRAPHICS_BIT) return i;
        if (!computeOnly) computeOnly = i;
    }
    return computeOnly;
}

float finiteClamp(float value, float lo, float hi) { return std::isfinite(value) ? std::clamp(value, lo, hi) : 0.0f; }

}  // namespace

VkFormat toVkFormat(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case PixelFormat::R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

std::uint32_t bytesPerPixel(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::R8G8B8A8_UNORM: return 4;
    case PixelFormat::R16G16B16A16_SFLOAT: return 8;
    case PixelFormat::R32G32B32A32_SFLOAT: return 16;
    }
    return 0;
}

void recordImageBarrier(VkCommandBuffer commandBuffer, GPUTexture& texture, VkImageLayout newLayout,
                        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                        VkAccessFlags2 dstAccess, bool discardContents) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = discardContents ? VK_IMAGE_LAYOUT_UNDEFINED : texture.layout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = colorRange();

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    texture.layout = newLayout;
}

void recordMemoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

// -----------------------------------------------------------------------------
// VulkanContext
// -----------------------------------------------------------------------------

VulkanContext::VulkanContext(const VulkanContextOptions& options) {
    try {
        createInstance(options);
        pickPhysicalDevice();
        createDevice();
    } catch (...) {
        destroy();
        throw;
    }
}

VulkanContext::~VulkanContext() { destroy(); }

void VulkanContext::createInstance(const VulkanContextOptions& options) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = options.applicationName.c_str();
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "DarkHouse Engine";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    VkInstanceCreateFlags flags = 0;

    if (options.enableValidation && instanceLayerAvailable(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        validationEnabled_ = true;
    }
#ifdef VK_KHR_portability_enumeration
    // Portability drivers such as MoltenVK are only enumerated when asked for explicitly.
    if (instanceExtensionAvailable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VkResult result = vkCreateInstance(&info, nullptr, &instance_);
    if (result == VK_ERROR_LAYER_NOT_PRESENT && validationEnabled_) {
        // The layer manifest was found but its library failed to load (e.g. a
        // Homebrew install outside the dyld search path). Validation is a debug
        // aid, so fall back to running without it rather than losing the GPU.
        validationEnabled_ = false;
        info.enabledLayerCount = 0;
        info.ppEnabledLayerNames = nullptr;
        result = vkCreateInstance(&info, nullptr, &instance_);
    }
    checkVk(result, "vkCreateInstance");
}

void VulkanContext::pickPhysicalDevice() {
    std::uint32_t count = 0;
    checkVk(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    checkVk(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");

    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) continue;

        const auto family = findComputeQueueFamily(candidate);
        if (!family) continue;

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (features13.synchronization2 != VK_TRUE) continue;

        int score = 0;
        switch (properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 3; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 2; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 1; break;
        default: score = 0; break;
        }
        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = candidate;
            computeQueueFamily_ = *family;
            deviceName_ = properties.deviceName;
        }
    }
    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error("no Vulkan 1.3 device with a compute queue and synchronization2 was found");
    }
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
}

void VulkanContext::createDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = computeQueueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features13;

    // The spec requires enabling VK_KHR_portability_subset whenever the device
    // exposes it (MoltenVK does).
    std::vector<const char*> extensions;
    if (deviceExtensionAvailable(physicalDevice_, kPortabilitySubsetExtension)) {
        extensions.push_back(kPortabilitySubsetExtension);
    }

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    checkVk(vkCreateDevice(physicalDevice_, &info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, computeQueueFamily_, 0, &computeQueue_);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = computeQueueFamily_;
    checkVk(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool");
}

void VulkanContext::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    commandPool_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

std::uint32_t VulkanContext::findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required) const {
    for (std::uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
        const bool allowed = (typeBits & (1u << i)) != 0;
        if (allowed && (memoryProperties_.memoryTypes[i].propertyFlags & required) == required) return i;
    }
    throw std::runtime_error("no Vulkan memory type satisfies the requested properties");
}

GPUTexture VulkanContext::createTexture(std::uint32_t width, std::uint32_t height, PixelFormat format,
                                        VkImageUsageFlags usage) const {
    if (width == 0 || height == 0) throw std::invalid_argument("createTexture: zero-sized texture");

    GPUTexture texture;
    texture.width = width;
    texture.height = height;
    texture.format = format;
    try {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = toVkFormat(format);
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        checkVk(vkCreateImage(device_, &imageInfo, nullptr, &texture.image), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, texture.image, &requirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vkAllocateMemory(device_, &allocInfo, nullptr, &texture.memory), "vkAllocateMemory");
        checkVk(vkBindImageMemory(device_, texture.image, texture.memory, 0), "vkBindImageMemory");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = texture.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange = colorRange();
        checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &texture.view), "vkCreateImageView");
    } catch (...) {
        destroyTexture(texture);
        throw;
    }
    return texture;
}

void VulkanContext::destroyTexture(GPUTexture& texture) const noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (texture.view != VK_NULL_HANDLE) vkDestroyImageView(device_, texture.view, nullptr);
        if (texture.image != VK_NULL_HANDLE) vkDestroyImage(device_, texture.image, nullptr);
        if (texture.memory != VK_NULL_HANDLE) vkFreeMemory(device_, texture.memory, nullptr);
    }
    texture = GPUTexture{};
}

VkShaderModule VulkanContext::loadShaderModule(const std::filesystem::path& spirvPath) const {
    std::ifstream in(spirvPath, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("cannot open shader '" + spirvPath.string() +
                                 "' (build the darkhouse_shaders target or pass --shaders <dir>)");
    }
    const std::streamoff size = in.tellg();
    if (size <= 0 || size % 4 != 0) throw std::runtime_error("'" + spirvPath.string() + "' is not valid SPIR-V");
    std::vector<std::uint32_t> code(static_cast<std::size_t>(size) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(code.data()), size);
    if (!in || code.front() != 0x07230203u) {
        throw std::runtime_error("'" + spirvPath.string() + "' is not valid SPIR-V (bad magic number)");
    }

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<std::size_t>(size);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(device_, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

void VulkanContext::submitAndWait(const std::function<void(VkCommandBuffer)>& record) const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    checkVk(vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer), "vkAllocateCommandBuffers");

    VkFence fence = VK_NULL_HANDLE;
    ScopeExit cleanup([&] {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
        vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    });

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");
    record(commandBuffer);
    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &fence), "vkCreateFence");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    checkVk(vkQueueSubmit(computeQueue_, 1, &submit, fence), "vkQueueSubmit");
    checkVk(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

void VulkanContext::uploadRegions(GPUTexture& texture, std::span<const RegionUpload> regions) const {
    if (regions.empty()) return;
    if (!texture.valid()) throw std::invalid_argument("uploadRegions: texture is not allocated");

    // Offsets aligned to 16 bytes satisfy vkCmdCopyBufferToImage for every PixelFormat.
    constexpr VkDeviceSize kOffsetAlignment = 16;
    const std::uint32_t texelSize = bytesPerPixel(texture.format);
    VkDeviceSize totalSize = 0;
    for (const RegionUpload& region : regions) {
        if (region.width == 0 || region.height == 0 || region.x >= texture.width || region.y >= texture.height ||
            region.width > texture.width - region.x || region.height > texture.height - region.y) {
            throw std::out_of_range("uploadRegions: region outside the texture");
        }
        const VkDeviceSize expected = VkDeviceSize{region.width} * region.height * texelSize;
        if (region.texels.size() != expected) throw std::invalid_argument("uploadRegions: texel size mismatch");
        totalSize += alignUp(expected, kOffsetAlignment);
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = totalSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    checkVk(vkCreateBuffer(device_, &bufferInfo, nullptr, &staging), "vkCreateBuffer");
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    ScopeExit cleanup([&] {
        vkDestroyBuffer(device_, staging, nullptr);
        if (stagingMemory != VK_NULL_HANDLE) vkFreeMemory(device_, stagingMemory, nullptr);
    });

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, staging, &requirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    checkVk(vkAllocateMemory(device_, &allocInfo, nullptr, &stagingMemory), "vkAllocateMemory (staging)");
    checkVk(vkBindBufferMemory(device_, staging, stagingMemory, 0), "vkBindBufferMemory");

    void* mapped = nullptr;
    checkVk(vkMapMemory(device_, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    std::vector<VkBufferImageCopy> copies;
    copies.reserve(regions.size());
    VkDeviceSize offset = 0;
    for (const RegionUpload& region : regions) {
        std::memcpy(static_cast<std::byte*>(mapped) + offset, region.texels.data(), region.texels.size());
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.bufferRowLength = 0;  // tightly packed
        copy.bufferImageHeight = 0;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset.x = static_cast<std::int32_t>(region.x);
        copy.imageOffset.y = static_cast<std::int32_t>(region.y);
        copy.imageOffset.z = 0;
        copy.imageExtent.width = region.width;
        copy.imageExtent.height = region.height;
        copy.imageExtent.depth = 1;
        copies.push_back(copy);
        offset += alignUp(region.texels.size(), kOffsetAlignment);
    }
    vkUnmapMemory(device_, stagingMemory);

    submitAndWait([&](VkCommandBuffer cmd) {
        recordImageBarrier(cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cmd, staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(copies.size()), copies.data());
        recordImageBarrier(cmd, texture, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
}

void VulkanContext::clearTexture(GPUTexture& texture, float r, float g, float b, float a) const {
    if (!texture.valid()) throw std::invalid_argument("clearTexture: texture is not allocated");
    submitAndWait([&](VkCommandBuffer cmd) {
        recordImageBarrier(cmd, texture, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           /*discardContents=*/true);
        VkClearColorValue color{};
        color.float32[0] = r;
        color.float32[1] = g;
        color.float32[2] = b;
        color.float32[3] = a;
        const VkImageSubresourceRange range = colorRange();
        vkCmdClearColorImage(cmd, texture.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
        recordMemoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
}

// -----------------------------------------------------------------------------
// ExposureNode
// -----------------------------------------------------------------------------

ExposureNode::ExposureNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : context_(context) {
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
        pushRange.size = sizeof(ExposureParams);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        checkVk(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout_),
                "vkCreatePipelineLayout");

        const VkShaderModule module = context_.loadShaderModule(shaderDirectory / "exposure.spv");
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

ExposureNode::~ExposureNode() { destroy(); }

void ExposureNode::destroy() noexcept {
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

void ExposureNode::setInputTexture(std::uint32_t slot, const GPUTexture& texture) {
    if (slot != 0) throw std::out_of_range("ExposureNode has a single input (slot 0)");
    if (!texture.valid()) throw std::invalid_argument("ExposureNode: input texture is not allocated");
    if (texture.format != PixelFormat::R16G16B16A16_SFLOAT) {
        throw std::invalid_argument("ExposureNode: input must be R16G16B16A16_SFLOAT (rgba16f)");
    }

    bool rewrite = texture.view != input_.view;
    input_ = texture;
    if (!output_.valid() || output_.width != texture.width || output_.height != texture.height) {
        context_.destroyTexture(output_);
        output_ = context_.createTexture(
            texture.width, texture.height, PixelFormat::R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        rewrite = true;
    }
    if (rewrite) writeDescriptors();
}

void ExposureNode::writeDescriptors() {
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

void ExposureNode::executeCompute(VkCommandBuffer commandBuffer) {
    if (!input_.valid() || !output_.valid()) {
        throw std::logic_error("ExposureNode::executeCompute called before setInputTexture");
    }
    // Every output texel is rewritten, so the old contents can be discarded.
    // Only a WAR execution dependency on earlier readers is needed.
    recordImageBarrier(commandBuffer, output_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       /*discardContents=*/true);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ExposureParams),
                       &params_);
    vkCmdDispatch(commandBuffer, divideRoundUp(output_.width, kWorkgroupSize),
                  divideRoundUp(output_.height, kWorkgroupSize), 1);
}

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

std::unique_ptr<ComputeNode> createComputeNode(std::string_view nodeType, const VulkanContext& context,
                                               const std::filesystem::path& shaderDirectory) {
    if (nodeType == ExposureNode::kTypeName) return std::make_unique<ExposureNode>(context, shaderDirectory);
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

    // External inputs were written outside the graph (uploads, earlier passes).
    // Make those writes visible to compute reads and move the images to GENERAL.
    for (ExternalBinding& binding : externalInputs_) {
        recordImageBarrier(commandBuffer, *binding.texture, VK_IMAGE_LAYOUT_GENERAL,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

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
    }
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
    orderDirty_ = true;
}

}  // namespace darkhouse
