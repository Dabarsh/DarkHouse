// DarkHouse — GLFW platform window.
//
// Owns one GLFW window created for Vulkan (no client API) and the GLFW library
// lifetime. The window hands out what the Vulkan context needs to present:
// the instance extensions GLFW requires and a surface factory.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

namespace darkhouse {

struct WindowOptions {
    std::string title = "DarkHouse";
    int width = 1600;   // screen coordinates; clamped to the primary monitor's work area
    int height = 1000;
    bool maximized = false;
    bool visible = true;  // false: create hidden and call show() once the first frame is ready
};

class PlatformWindow {
public:
    // Initializes GLFW and creates the window. Throws std::runtime_error when
    // GLFW cannot initialize, has no Vulkan loader, or window creation fails.
    explicit PlatformWindow(const WindowOptions& options);
    ~PlatformWindow();

    PlatformWindow(const PlatformWindow&) = delete;
    PlatformWindow& operator=(const PlatformWindow&) = delete;

    [[nodiscard]] GLFWwindow* handle() const noexcept { return window_; }

    // Instance extensions GLFW needs for vkCreateInstance (surface + platform surface).
    [[nodiscard]] static std::vector<std::string> requiredInstanceExtensions();
    // Creates a VkSurfaceKHR for this window. The caller owns and destroys it.
    [[nodiscard]] VkSurfaceKHR createSurface(VkInstance instance) const;

    // Framebuffer size in pixels (differs from the window size on HiDPI displays).
    [[nodiscard]] VkExtent2D framebufferExtent() const;
    // True while the window is iconified or has a zero-sized framebuffer.
    [[nodiscard]] bool minimized() const;
    // Display scale of the monitor the window is on (1.0 = 96 dpi / non-Retina).
    [[nodiscard]] float contentScale() const;

    [[nodiscard]] bool shouldClose() const;
    void requestClose();
    void show();
    void setTitle(const std::string& title);

    // Returns true once after the framebuffer was resized, then resets.
    [[nodiscard]] bool consumeFramebufferResized() noexcept;
    // Files and folders dropped onto the window since the last call (UTF-8 paths).
    [[nodiscard]] std::vector<std::string> takeDroppedPaths();
    // Number of input events (keys, text, mouse, scroll, focus, drops,
    // resizes) received so far. Frame pacing uses it to tell idle from busy.
    [[nodiscard]] std::uint64_t inputEventCount() const noexcept { return inputEvents_; }

    // Event pumping. Must be called from the main thread.
    static void pollEvents();
    static void waitEvents();
    static void waitEventsTimeout(double seconds);
    static void postEmptyEvent();  // wakes waitEvents*() from any thread

private:
    static void onFramebufferSize(GLFWwindow* window, int width, int height);
    static void onDrop(GLFWwindow* window, int count, const char** paths);
    static void countInput(GLFWwindow* window) noexcept;

    GLFWwindow* window_ = nullptr;
    bool framebufferResized_ = false;
    std::vector<std::string> droppedPaths_;
    std::uint64_t inputEvents_ = 0;
};

}  // namespace darkhouse
