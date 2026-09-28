// Presentation smoke test: opens a DarkHouse window, creates a presenting
// Vulkan 1.3 context and swapchain, and clears + presents frames with dynamic
// rendering. With --resize it resizes the window halfway through to exercise
// swapchain recreation. Validation layers are enabled when installed, and any
// validation error fails the test.
//
// Exit codes: 0 pass, 1 fail, 77 skipped (no display / no Vulkan loader).

#include "platform_window.hpp"
#include "swapchain.hpp"
#include "vulkan_context.hpp"

#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string_view>

using namespace darkhouse;

int main(int argc, char** argv) {
    std::uint32_t targetFrames = 60;
    bool resize = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            targetFrames = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--resize") {
            resize = true;
        }
    }

    std::unique_ptr<PlatformWindow> window;
    try {
        WindowOptions windowOptions;
        windowOptions.title = "DarkHouse present smoke test";
        windowOptions.width = 800;
        windowOptions.height = 600;
        window = std::make_unique<PlatformWindow>(windowOptions);
    } catch (const std::exception& e) {
        std::cout << "SKIP: cannot open a window: " << e.what() << '\n';
        return 77;
    }

    try {
        VulkanContextOptions contextOptions;
        contextOptions.applicationName = "DarkHouse present smoke test";
        contextOptions.enableValidation = true;
        contextOptions.instanceExtensions = PlatformWindow::requiredInstanceExtensions();
        contextOptions.createSurface = [&](VkInstance instance) { return window->createSurface(instance); };
        VulkanContext context(contextOptions);
        std::cout << "device: " << context.deviceName() << (context.validationEnabled() ? " (validation on)" : "")
                  << '\n';

        std::uint32_t presented = 0;
        std::uint64_t swapchainBuilds = 0;
        {
            Swapchain swapchain(context, window->framebufferExtent());
            for (std::uint32_t frameIndex = 0; frameIndex < targetFrames && !window->shouldClose(); ++frameIndex) {
                PlatformWindow::pollEvents();
                if (resize && frameIndex == targetFrames / 2) glfwSetWindowSize(window->handle(), 640, 480);
                if (window->consumeFramebufferResized()) swapchain.requestRecreate();

                const auto frame = swapchain.beginFrame(window->framebufferExtent());
                if (!frame) continue;

                const float t = static_cast<float>(frameIndex) / 30.0f;
                VkRenderingAttachmentInfo color{};
                color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                color.imageView = frame->view;
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                color.clearValue.color = {{0.5f + 0.5f * std::sin(t), 0.2f, 0.5f + 0.5f * std::cos(t), 1.0f}};
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea = {{0, 0}, frame->extent};
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &color;
                vkCmdBeginRendering(frame->commandBuffer, &rendering);
                vkCmdEndRendering(frame->commandBuffer);

                swapchain.endFrame(*frame);
                ++presented;
            }
            context.waitIdle();
            swapchainBuilds = swapchain.generation();
            std::cout << "presented " << presented << "/" << targetFrames << " frames, " << swapchain.imageCount()
                      << " images, " << swapchain.extent().width << "x" << swapchain.extent().height
                      << ", swapchain builds: " << swapchainBuilds << '\n';
        }

        const std::uint32_t errors = context.validationErrorCount();
        if (errors > 0) {
            std::cout << "FAIL: " << errors << " validation error(s)\n";
            return 1;
        }
        if (presented < targetFrames / 2) {
            std::cout << "FAIL: too few frames presented\n";
            return 1;
        }
        std::cout << "PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
