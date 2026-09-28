#include "vulkan_context.hpp"

#include "vulkan_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace darkhouse {

std::string vkResultName(VkResult result) {
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
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + vkResultName(result));
}

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
// Defined in vulkan_beta.h, which is not included by default.
constexpr const char* kPortabilitySubsetExtension = "VK_KHR_portability_subset";

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

VkImageSubresourceRange colorRange() {
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = VK_REMAINING_ARRAY_LAYERS;  // every layer of array textures
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

void logInfo(const std::string& message) { std::clog << "[DarkHouse] info: " << message << '\n'; }

// Picks the queue family the context runs everything on.
// Headless: prefers a universal (graphics + compute) family, else compute-only.
// Presenting: requires graphics + compute + present support on `surface`, so
// the UI, the develop graph and presentation share one queue.
std::optional<std::uint32_t> findQueueFamily(VkPhysicalDevice device, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    std::optional<std::uint32_t> computeOnly;
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) == 0) continue;
        const bool universal = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (surface != VK_NULL_HANDLE) {
            VkBool32 presentable = VK_FALSE;
            if (!universal || vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentable) != VK_SUCCESS ||
                presentable != VK_TRUE) {
                continue;
            }
            return i;
        }
        if (universal) return i;
        if (!computeOnly) computeOnly = i;
    }
    return surface != VK_NULL_HANDLE ? std::nullopt : computeOnly;
}

VkDeviceSize deviceLocalMemory(VkPhysicalDevice device) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    VkDeviceSize total = 0;
    for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) total += memory.memoryHeaps[i].size;
    }
    return total;
}

int deviceTypeScore(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
    default: return 0;  // CPU implementations (lavapipe, SwiftShader) and others
    }
}

// Validation layers ship with the Vulkan SDK, which is often not installed
// system-wide. When the loader cannot see the layer, point it at the manifest
// directory CMake found at configure time (DARKHOUSE_VK_LAYER_DIR), unless the
// user already steers layer discovery through the environment.
void makeValidationLayerDiscoverable() {
#ifdef DARKHOUSE_VK_LAYER_DIR
    if (instanceLayerAvailable(kValidationLayer)) return;
    if (std::getenv("VK_LAYER_PATH") || std::getenv("VK_ADD_LAYER_PATH")) return;
#if defined(_WIN32)
    _putenv_s("VK_ADD_LAYER_PATH", DARKHOUSE_VK_LAYER_DIR);
#else
    setenv("VK_ADD_LAYER_PATH", DARKHOUSE_VK_LAYER_DIR, 0);
#endif
    logInfo(std::string("validation: searching for layers in ") + DARKHOUSE_VK_LAYER_DIR);
#endif
}

VKAPI_ATTR VkBool32 VKAPI_CALL onDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* userData) {
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (error && userData) static_cast<std::atomic<std::uint32_t>*>(userData)->fetch_add(1);
    std::ostringstream line;
    line << "[DarkHouse] " << (error ? "error" : "warn") << ": vulkan: "
         << (data && data->pMessageIdName ? data->pMessageIdName : "") << (data && data->pMessageIdName ? ": " : "")
         << (data && data->pMessage ? data->pMessage : "(no message)") << '\n';
    std::clog << line.str();
    return VK_FALSE;  // never abort the call that triggered the message
}

VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo(std::atomic<std::uint32_t>* errorCounter) {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onDebugMessage;
    info.pUserData = errorCounter;
    return info;
}

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
        createDebugMessenger();
        if (options.createSurface) {
            surface_ = options.createSurface(instance_);
            if (surface_ == VK_NULL_HANDLE) throw std::runtime_error("createSurface returned VK_NULL_HANDLE");
        }
        pickPhysicalDevice();
        createDevice();
    } catch (...) {
        destroy();
        throw;
    }
}

VulkanContext::~VulkanContext() { destroy(); }

std::uint32_t VulkanContext::validationErrorCount() const noexcept { return validationErrors_.load(); }

void VulkanContext::waitIdle() const noexcept {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
}

void VulkanContext::createInstance(const VulkanContextOptions& options) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = options.applicationName.c_str();
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "DarkHouse Engine";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    for (const std::string& name : options.instanceExtensions) {
        if (!instanceExtensionAvailable(name.c_str())) {
            throw std::runtime_error("required Vulkan instance extension " + name + " is not available");
        }
    }
    if (options.enableValidation) makeValidationLayerDiscoverable();
    const bool debugUtilsAvailable = instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    auto tryCreate = [&](bool withValidation) {
        std::vector<const char*> layers;
        std::vector<const char*> extensions;
        VkInstanceCreateFlags flags = 0;
        for (const std::string& name : options.instanceExtensions) extensions.push_back(name.c_str());
#ifdef VK_KHR_portability_enumeration
        // Portability drivers such as MoltenVK are only enumerated when asked for explicitly.
        if (instanceExtensionAvailable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif
        // Chained into vkCreateInstance so instance creation and destruction
        // are validated as well, not only the calls in between.
        const VkDebugUtilsMessengerCreateInfoEXT messengerInfo = debugMessengerInfo(&validationErrors_);
        const bool useMessenger = withValidation && debugUtilsAvailable;
        if (withValidation) layers.push_back(kValidationLayer);
        if (useMessenger) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pNext = useMessenger ? &messengerInfo : nullptr;
        info.flags = flags;
        info.pApplicationInfo = &app;
        info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        info.ppEnabledLayerNames = layers.data();
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        return vkCreateInstance(&info, nullptr, &instance_);
    };

    validationEnabled_ = options.enableValidation && instanceLayerAvailable(kValidationLayer);
    VkResult result = tryCreate(validationEnabled_);
    if (result == VK_ERROR_LAYER_NOT_PRESENT && validationEnabled_) {
        // The layer manifest was found but its library failed to load (e.g. a
        // Homebrew install outside the dyld search path). Validation is a debug
        // aid, so fall back to running without it rather than losing the GPU.
        validationEnabled_ = false;
        result = tryCreate(false);
    }
    checkVk(result, "vkCreateInstance");
}

void VulkanContext::createDebugMessenger() {
    if (!validationEnabled_ || !instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) return;
    const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (!create) return;
    const VkDebugUtilsMessengerCreateInfoEXT info = debugMessengerInfo(&validationErrors_);
    checkVk(create(instance_, &info, nullptr, &debugMessenger_), "vkCreateDebugUtilsMessengerEXT");
}

void VulkanContext::pickPhysicalDevice() {
    std::uint32_t count = 0;
    checkVk(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    checkVk(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");

    const bool presenting = surface_ != VK_NULL_HANDLE;
    std::ostringstream rejected;
    int bestTypeScore = -1;
    VkDeviceSize bestMemory = 0;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        auto reject = [&](const char* reason) { rejected << "\n  " << properties.deviceName << ": " << reason; };

        if (properties.apiVersion < VK_API_VERSION_1_3) {
            reject("Vulkan 1.3 not supported");
            continue;
        }
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (features13.synchronization2 != VK_TRUE || features13.dynamicRendering != VK_TRUE) {
            reject("synchronization2 or dynamicRendering missing");
            continue;
        }
        if (presenting) {
            if (!deviceExtensionAvailable(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                reject("VK_KHR_swapchain missing");
                continue;
            }
            std::uint32_t formatCount = 0;
            std::uint32_t modeCount = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface_, &formatCount, nullptr);
            vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface_, &modeCount, nullptr);
            if (formatCount == 0 || modeCount == 0) {
                reject("cannot present to this window surface");
                continue;
            }
        }
        const auto family = findQueueFamily(candidate, surface_);
        if (!family) {
            reject(presenting ? "no queue family with graphics + compute + present" : "no compute queue family");
            continue;
        }

        // Discrete GPUs first; among equals, the one with the most VRAM.
        const int typeScore = deviceTypeScore(properties.deviceType);
        const VkDeviceSize memory = deviceLocalMemory(candidate);
        if (typeScore > bestTypeScore || (typeScore == bestTypeScore && memory > bestMemory)) {
            bestTypeScore = typeScore;
            bestMemory = memory;
            physicalDevice_ = candidate;
            computeQueueFamily_ = *family;
            deviceName_ = properties.deviceName;
            deviceType_ = properties.deviceType;
            timestampPeriodNs_ = properties.limits.timestampPeriod;
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            timestampValidBits_ = *family < familyCount ? families[*family].timestampValidBits : 0;
        }
    }
    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error(std::string("no suitable Vulkan 1.3 device found") +
                                 (count == 0 ? " (no devices enumerated)" : rejected.str()));
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

    // Both are mandatory in Vulkan 1.3: synchronization2 for every barrier in
    // the engine, dynamic rendering so UI passes need no VkRenderPass objects.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.synchronization2 = VK_TRUE;
    features13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features13;

    std::vector<const char*> extensions;
    if (surface_ != VK_NULL_HANDLE) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // The spec requires enabling VK_KHR_portability_subset whenever the device
    // exposes it (MoltenVK does).
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
    if (instance_ != VK_NULL_HANDLE) {
        if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (debugMessenger_ != VK_NULL_HANDLE) {
            const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyMessenger) destroyMessenger(instance_, debugMessenger_, nullptr);
        }
        vkDestroyInstance(instance_, nullptr);
    }
    commandPool_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    debugMessenger_ = VK_NULL_HANDLE;
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
                                        VkImageUsageFlags usage, std::uint32_t arrayLayers) const {
    if (width == 0 || height == 0) throw std::invalid_argument("createTexture: zero-sized texture");

    GPUTexture texture;
    texture.width = width;
    texture.height = height;
    texture.format = format;
    texture.layers = std::max(arrayLayers, 1u);
    try {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = toVkFormat(format);
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = texture.layers;
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
        viewInfo.viewType = arrayLayers > 0 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
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
    Submission submission = submitAsync(record);
    release(submission);
}

VulkanContext::Submission VulkanContext::submitAsync(const std::function<void(VkCommandBuffer)>& record) const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    Submission submission;
    checkVk(vkAllocateCommandBuffers(device_, &allocInfo, &submission.commandBuffer), "vkAllocateCommandBuffers");

    VkFence fence = VK_NULL_HANDLE;
    try {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(submission.commandBuffer, &beginInfo), "vkBeginCommandBuffer");
        record(submission.commandBuffer);
        checkVk(vkEndCommandBuffer(submission.commandBuffer), "vkEndCommandBuffer");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &fence), "vkCreateFence");

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &submission.commandBuffer;
        checkVk(vkQueueSubmit(computeQueue_, 1, &submit, fence), "vkQueueSubmit");
    } catch (...) {
        if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
        vkFreeCommandBuffers(device_, commandPool_, 1, &submission.commandBuffer);
        throw;
    }
    submission.fence = fence;
    return submission;
}

bool VulkanContext::finished(const Submission& submission) const {
    if (!submission.pending()) return true;
    const VkResult status = vkGetFenceStatus(device_, submission.fence);
    if (status == VK_NOT_READY) return false;
    checkVk(status, "vkGetFenceStatus");
    return true;
}

void VulkanContext::release(Submission& submission) const noexcept {
    if (!submission.pending()) return;
    vkWaitForFences(device_, 1, &submission.fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, submission.fence, nullptr);
    vkFreeCommandBuffers(device_, commandPool_, 1, &submission.commandBuffer);
    submission = Submission{};
}

GPUBuffer VulkanContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) const {
    if (size == 0) throw std::invalid_argument("createBuffer: zero-sized buffer");
    GPUBuffer buffer;
    buffer.size = size;
    try {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateBuffer(device_, &info, nullptr, &buffer.buffer), "vkCreateBuffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer.buffer, &requirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            requirements.memoryTypeBits, hostVisible
                                             ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vkAllocateMemory(device_, &allocInfo, nullptr, &buffer.memory), "vkAllocateMemory (buffer)");
        checkVk(vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0), "vkBindBufferMemory");
        if (hostVisible) checkVk(vkMapMemory(device_, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped), "vkMapMemory");
    } catch (...) {
        destroyBuffer(buffer);
        throw;
    }
    return buffer;
}

void VulkanContext::destroyBuffer(GPUBuffer& buffer) const noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer.buffer, nullptr);
        if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(device_, buffer.memory, nullptr);  // implicitly unmaps
    }
    buffer = GPUBuffer{};
}

std::vector<std::byte> VulkanContext::downloadTexture(GPUTexture& texture, std::uint32_t layer) const {
    if (!texture.valid()) throw std::invalid_argument("downloadTexture: texture is not allocated");
    if (layer >= texture.layers) throw std::out_of_range("downloadTexture: no such array layer");
    const VkDeviceSize size = VkDeviceSize{texture.width} * texture.height * bytesPerPixel(texture.format);
    GPUBuffer readback = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, /*hostVisible=*/true);
    ScopeExit cleanup([&] { destroyBuffer(readback); });

    submitAndWait([&](VkCommandBuffer cmd) {
        recordImageBarrier(cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.baseArrayLayer = layer;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {texture.width, texture.height, 1};
        vkCmdCopyImageToBuffer(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        recordImageBarrier(cmd, texture, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        recordMemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                            VK_ACCESS_2_HOST_READ_BIT);
    });
    const auto* bytes = static_cast<const std::byte*>(readback.mapped);
    return {bytes, bytes + size};
}

void VulkanContext::uploadRegions(GPUTexture& texture, std::span<const RegionUpload> regions,
                                  std::uint32_t layer) const {
    if (regions.empty()) return;
    if (!texture.valid()) throw std::invalid_argument("uploadRegions: texture is not allocated");
    if (layer >= texture.layers) throw std::out_of_range("uploadRegions: no such array layer");

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

    GPUBuffer staging = createBuffer(totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*hostVisible=*/true);
    ScopeExit cleanup([&] { destroyBuffer(staging); });
    auto* mapped = static_cast<std::byte*>(staging.mapped);
    std::vector<VkBufferImageCopy> copies;
    copies.reserve(regions.size());
    VkDeviceSize offset = 0;
    for (const RegionUpload& region : regions) {
        std::memcpy(mapped + offset, region.texels.data(), region.texels.size());
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.bufferRowLength = 0;  // tightly packed
        copy.bufferImageHeight = 0;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = layer;
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

    submitAndWait([&](VkCommandBuffer cmd) {
        recordImageBarrier(cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cmd, staging.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
// GpuTimestamps
// -----------------------------------------------------------------------------

GpuTimestamps::GpuTimestamps(const VulkanContext& context, std::uint32_t capacity) : context_(context) {
    if (capacity == 0 || context_.timestampValidBits() == 0 || context_.timestampPeriodNs() <= 0.0) return;
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = capacity;
    checkVk(vkCreateQueryPool(context_.device(), &info, nullptr, &pool_), "vkCreateQueryPool");
    capacity_ = capacity;
}

GpuTimestamps::~GpuTimestamps() {
    if (pool_ != VK_NULL_HANDLE) vkDestroyQueryPool(context_.device(), pool_, nullptr);
}

void GpuTimestamps::reset(VkCommandBuffer commandBuffer, std::uint32_t count) const {
    if (pool_ != VK_NULL_HANDLE) vkCmdResetQueryPool(commandBuffer, pool_, 0, std::min(count, capacity_));
}

void GpuTimestamps::write(VkCommandBuffer commandBuffer, std::uint32_t index) const {
    if (pool_ != VK_NULL_HANDLE && index < capacity_) {
        vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, index);
    }
}

std::vector<double> GpuTimestamps::intervals(std::uint32_t count) const {
    count = std::min(count, capacity_);
    if (pool_ == VK_NULL_HANDLE || count < 2) return {};
    std::vector<std::uint64_t> ticks(count);
    if (vkGetQueryPoolResults(context_.device(), pool_, 0, count, ticks.size() * sizeof(std::uint64_t), ticks.data(),
                              sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return {};  // VK_NOT_READY: not submitted or not finished
    }
    const std::uint32_t bits = context_.timestampValidBits();
    const std::uint64_t mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    std::vector<double> out(count - 1);
    for (std::uint32_t i = 0; i + 1 < count; ++i) {
        const std::uint64_t delta = ((ticks[i + 1] & mask) - (ticks[i] & mask)) & mask;  // wraps within the valid bits
        out[i] = static_cast<double>(delta) * context_.timestampPeriodNs() * 1e-6;
    }
    return out;
}

}  // namespace darkhouse
