// DarkHouse — swapchain and frame pacing primitives.
//
// Swapchain owns the VkSwapchainKHR of the context's window surface, its image
// views, and the per-frame synchronization used to keep up to
// kMaxFramesInFlight frames queued on the GPU:
//
//   frame slot (kMaxFramesInFlight): command pool + buffer, in-flight fence,
//                                    image-acquired semaphore
//   swapchain image:                 render-finished semaphore (the present waits on it)
//
// Usage, once per frame on the render thread:
//
//   if (auto frame = swapchain.beginFrame(window.framebufferExtent())) {
//       ... vkCmdBeginRendering(frame->commandBuffer, <frame->view>) ...
//       swapchain.endFrame(*frame);
//   }
//
// beginFrame() waits for the slot's previous submission, acquires an image and
// transitions it to COLOR_ATTACHMENT_OPTIMAL. endFrame() transitions it to
// PRESENT_SRC_KHR, submits and presents. Out-of-date and suboptimal swapchains
// are recreated transparently; a frame that cannot be drawn (minimized window,
// out-of-date surface) is skipped by returning std::nullopt.
#pragma once

#include "vulkan_context.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace darkhouse {

struct SwapchainOptions {
    // true: FIFO (tear-free, paced to the display refresh).
    // false: MAILBOX when available, else IMMEDIATE, else FIFO.
    bool vsync = true;
};

// Everything needed to record one frame. Valid between beginFrame() and endFrame().
struct SwapchainFrame {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;  // begun, ONE_TIME_SUBMIT
    VkImage image = VK_NULL_HANDLE;                  // in COLOR_ATTACHMENT_OPTIMAL
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t imageIndex = 0;
    std::uint32_t frameSlot = 0;  // 0 .. kMaxFramesInFlight-1
};

class Swapchain {
public:
    static constexpr std::uint32_t kMaxFramesInFlight = 2;

    // The context must have been created with a surface (presentationEnabled()).
    Swapchain(const VulkanContext& context, VkExtent2D framebufferExtent, const SwapchainOptions& options = {});
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // std::nullopt: skip this frame (zero-sized framebuffer, or the swapchain
    // was out of date and has just been recreated).
    [[nodiscard]] std::optional<SwapchainFrame> beginFrame(VkExtent2D framebufferExtent);
    // Must follow every beginFrame() that returned a frame, even on error paths,
    // so the acquire semaphore is always consumed.
    void endFrame(const SwapchainFrame& frame);

    // Forces a rebuild on the next beginFrame() (window resized, vsync toggled).
    void requestRecreate() noexcept { recreatePending_ = true; }
    void setVsync(bool vsync) noexcept;

    [[nodiscard]] VkFormat format() const noexcept { return surfaceFormat_.format; }
    [[nodiscard]] VkColorSpaceKHR colorSpace() const noexcept { return surfaceFormat_.colorSpace; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] VkPresentModeKHR presentMode() const noexcept { return presentMode_; }
    [[nodiscard]] std::uint32_t imageCount() const noexcept { return static_cast<std::uint32_t>(images_.size()); }
    [[nodiscard]] std::uint32_t minImageCount() const noexcept { return minImageCount_; }
    [[nodiscard]] bool vsync() const noexcept { return options_.vsync; }
    // Incremented whenever the swapchain is rebuilt (image count or format may change).
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    struct FrameSync {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;           // signalled when the slot's last submission completes
        VkSemaphore imageAcquired = VK_NULL_HANDLE;  // signalled by vkAcquireNextImageKHR
    };

    void createFrameSync();
    void createSwapchain(VkExtent2D framebufferExtent);
    void destroySwapchainViews() noexcept;
    void recreate(VkExtent2D framebufferExtent);
    void destroy() noexcept;

    const VulkanContext& context_;
    SwapchainOptions options_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surfaceFormat_{};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_{};
    VkExtent2D requestedExtent_{};  // framebuffer size passed to the last rebuild
    std::uint32_t minImageCount_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    // Indexed by image. Never shrunk on recreation: a present queued on the old
    // swapchain may still wait on them, and nothing short of
    // VK_EXT_swapchain_maintenance1 reports when that wait is over.
    std::vector<VkSemaphore> renderFinished_;
    std::array<FrameSync, kMaxFramesInFlight> frames_{};
    std::uint32_t frameSlot_ = 0;
    std::uint64_t generation_ = 0;
    bool recreatePending_ = false;
};

}  // namespace darkhouse
