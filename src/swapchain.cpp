#include "swapchain.hpp"

#include "vulkan_utils.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace darkhouse {
namespace {

VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    if (formats.empty()) throw std::runtime_error("the window surface reports no formats");
    // UNORM, not SRGB: ImGui colours are authored in sRGB already, and the
    // viewport applies its own display transform to scene-referred pixels.
    constexpr std::array<VkFormat, 2> kPreferred{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    for (VkFormat wanted : kPreferred) {
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.format == wanted && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return candidate;
        }
    }
    if (formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED) {
        return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};  // surface has no preference
    }
    return formats.front();
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync) {
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR;  // always supported
    for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) return wanted;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    for (VkCompositeAlphaFlagBitsKHR wanted :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (supported & wanted) return wanted;
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

void transitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                     VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

VkSemaphore createSemaphore(VkDevice device) {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    checkVk(vkCreateSemaphore(device, &info, nullptr, &semaphore), "vkCreateSemaphore");
    return semaphore;
}

}  // namespace

Swapchain::Swapchain(const VulkanContext& context, VkExtent2D framebufferExtent, const SwapchainOptions& options)
    : context_(context), options_(options) {
    if (!context_.presentationEnabled()) throw std::logic_error("Swapchain needs a VulkanContext with a surface");
    try {
        createFrameSync();
        createSwapchain(framebufferExtent);
    } catch (...) {
        destroy();
        throw;
    }
}

Swapchain::~Swapchain() { destroy(); }

void Swapchain::createFrameSync() {
    const VkDevice device = context_.device();
    for (FrameSync& frame : frames_) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;  // reset as a whole every frame
        poolInfo.queueFamilyIndex = context_.queueFamily();
        checkVk(vkCreateCommandPool(device, &poolInfo, nullptr, &frame.commandPool), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        checkVk(vkAllocateCommandBuffers(device, &allocInfo, &frame.commandBuffer), "vkAllocateCommandBuffers");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // the first wait on each slot returns immediately
        checkVk(vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence");

        frame.imageAcquired = createSemaphore(device);
    }
}

void Swapchain::createSwapchain(VkExtent2D framebufferExtent) {
    const VkPhysicalDevice physicalDevice = context_.physicalDevice();
    const VkSurfaceKHR surface = context_.surface();
    const VkDevice device = context_.device();
    requestedExtent_ = framebufferExtent;

    VkSurfaceCapabilitiesKHR caps{};
    checkVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    // currentExtent is authoritative where the platform defines it (Win32,
    // X11); Wayland reports 0xFFFFFFFF and lets the framebuffer size decide.
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
        extent.width = std::clamp(framebufferExtent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(framebufferExtent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        // Minimized. Keep the old swapchain (if any) and retry next frame.
        recreatePending_ = true;
        return;
    }

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
    std::uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &modeCount, modes.data());

    surfaceFormat_ = chooseSurfaceFormat(formats);
    presentMode_ = choosePresentMode(modes, options_.vsync);

    // One more image than the minimum, so the CPU can record while the
    // display holds one image and the GPU renders another.
    std::uint32_t imageCount = std::max(caps.minImageCount + 1, 2u);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);
    minImageCount_ = std::max(caps.minImageCount, 2u);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    const VkSwapchainKHR oldSwapchain = swapchain_;
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat_.format;
    info.imageColorSpace = surfaceFormat_.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;  // one queue does everything
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                            : caps.currentTransform;
    info.compositeAlpha = chooseCompositeAlpha(caps.supportedCompositeAlpha);
    info.presentMode = presentMode_;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    checkVk(vkCreateSwapchainKHR(device, &info, nullptr, &created), "vkCreateSwapchainKHR");
    if (oldSwapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
    swapchain_ = created;
    extent_ = extent;

    std::uint32_t count = 0;
    checkVk(vkGetSwapchainImagesKHR(device, swapchain_, &count, nullptr), "vkGetSwapchainImagesKHR");
    images_.resize(count);
    checkVk(vkGetSwapchainImagesKHR(device, swapchain_, &count, images_.data()), "vkGetSwapchainImagesKHR");

    views_.reserve(count);
    for (VkImage image : images_) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat_.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        checkVk(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView (swapchain)");
        views_.push_back(view);
    }
    while (renderFinished_.size() < count) renderFinished_.push_back(createSemaphore(device));

    recreatePending_ = false;
    ++generation_;
}

void Swapchain::destroySwapchainViews() noexcept {
    for (VkImageView view : views_) vkDestroyImageView(context_.device(), view, nullptr);
    views_.clear();
    images_.clear();
}

void Swapchain::recreate(VkExtent2D framebufferExtent) {
    // Simple and robust: nothing in flight can reference the old images.
    context_.waitIdle();
    destroySwapchainViews();
    createSwapchain(framebufferExtent);
}

void Swapchain::setVsync(bool vsync) noexcept {
    if (options_.vsync == vsync) return;
    options_.vsync = vsync;
    recreatePending_ = true;
}

std::optional<SwapchainFrame> Swapchain::beginFrame(VkExtent2D framebufferExtent) {
    if (framebufferExtent.width == 0 || framebufferExtent.height == 0) return std::nullopt;
    // Compared with the size requested at the last rebuild rather than
    // extent_, which the surface may have clamped: that would rebuild forever.
    const bool resized =
        framebufferExtent.width != requestedExtent_.width || framebufferExtent.height != requestedExtent_.height;
    if (swapchain_ == VK_NULL_HANDLE || recreatePending_ || resized) {
        recreate(framebufferExtent);
        if (swapchain_ == VK_NULL_HANDLE || recreatePending_) return std::nullopt;
    }

    const VkDevice device = context_.device();
    FrameSync& sync = frames_[frameSlot_];
    checkVk(vkWaitForFences(device, 1, &sync.inFlight, VK_TRUE, std::numeric_limits<std::uint64_t>::max()),
            "vkWaitForFences (frame)");

    std::uint32_t imageIndex = 0;
    const VkResult acquired = vkAcquireNextImageKHR(device, swapchain_, std::numeric_limits<std::uint64_t>::max(),
                                                    sync.imageAcquired, VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        // The semaphore was not signalled and the fence is still signalled,
        // so this slot is untouched; rebuild and skip the frame.
        recreate(framebufferExtent);
        return std::nullopt;
    }
    if (acquired == VK_SUBOPTIMAL_KHR) {
        recreatePending_ = true;  // still presentable; rebuild after this frame
    } else {
        checkVk(acquired, "vkAcquireNextImageKHR");
    }

    // Only reset once an image is acquired, so an early return above can never
    // leave the fence unsignalled with nothing submitted to signal it.
    checkVk(vkResetFences(device, 1, &sync.inFlight), "vkResetFences");
    checkVk(vkResetCommandPool(device, sync.commandPool, 0), "vkResetCommandPool");
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(sync.commandBuffer, &beginInfo), "vkBeginCommandBuffer (frame)");

    // The acquire semaphore is waited on at COLOR_ATTACHMENT_OUTPUT, so the
    // transition chains off that stage. Old contents are discarded.
    transitionImage(sync.commandBuffer, images_[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    SwapchainFrame frame;
    frame.commandBuffer = sync.commandBuffer;
    frame.image = images_[imageIndex];
    frame.view = views_[imageIndex];
    frame.extent = extent_;
    frame.format = surfaceFormat_.format;
    frame.imageIndex = imageIndex;
    frame.frameSlot = frameSlot_;
    return frame;
}

void Swapchain::endFrame(const SwapchainFrame& frame) {
    FrameSync& sync = frames_[frame.frameSlot];
    transitionImage(frame.commandBuffer, frame.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer (frame)");

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = sync.imageAcquired;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = renderFinished_[frame.imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandInfo.commandBuffer = frame.commandBuffer;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    checkVk(vkQueueSubmit2(context_.queue(), 1, &submit, sync.inFlight), "vkQueueSubmit2 (frame)");

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[frame.imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &frame.imageIndex;
    const VkResult presented = vkQueuePresentKHR(context_.queue(), &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        recreatePending_ = true;
    } else {
        checkVk(presented, "vkQueuePresentKHR");
    }
    frameSlot_ = (frameSlot_ + 1) % kMaxFramesInFlight;
}

void Swapchain::destroy() noexcept {
    const VkDevice device = context_.device();
    if (device == VK_NULL_HANDLE) return;
    context_.waitIdle();
    destroySwapchainViews();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    for (VkSemaphore semaphore : renderFinished_) vkDestroySemaphore(device, semaphore, nullptr);
    renderFinished_.clear();
    for (FrameSync& frame : frames_) {
        if (frame.imageAcquired != VK_NULL_HANDLE) vkDestroySemaphore(device, frame.imageAcquired, nullptr);
        if (frame.inFlight != VK_NULL_HANDLE) vkDestroyFence(device, frame.inFlight, nullptr);
        if (frame.commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, frame.commandPool, nullptr);  // frees the buffer
        frame = FrameSync{};
    }
}

}  // namespace darkhouse
