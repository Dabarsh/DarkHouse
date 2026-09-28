#include "platform_window.hpp"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE  // vulkan.h is already included; GLFW must not pull in GL headers
#endif
#include <GLFW/glfw3.h>

#include "vulkan_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace darkhouse {
namespace {

int g_glfwUsers = 0;  // GLFW is initialized while at least one window exists (main thread only)

void onGlfwError(int code, const char* description) {
    std::clog << "[DarkHouse] warn: GLFW error " << code << ": " << (description ? description : "?") << '\n';
}

void acquireGlfw() {
    if (g_glfwUsers++ > 0) return;
    glfwSetErrorCallback(onGlfwError);
#if defined(__linux__) || defined(__FreeBSD__)
    // GLFW probes Wayland first. On a plain X11 session that only produces
    // connection errors on stderr, so go straight to X11 there.
    if (!std::getenv("WAYLAND_DISPLAY") && std::getenv("DISPLAY") && glfwPlatformSupported(GLFW_PLATFORM_X11)) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    }
#endif
    if (glfwInit() != GLFW_TRUE) {
        --g_glfwUsers;
        throw std::runtime_error("glfwInit failed (no display available?)");
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        glfwTerminate();
        --g_glfwUsers;
        throw std::runtime_error("GLFW found no Vulkan loader; install a Vulkan 1.3 driver or the Vulkan SDK");
    }
}

void releaseGlfw() noexcept {
    if (--g_glfwUsers == 0) glfwTerminate();
}

}  // namespace

PlatformWindow::PlatformWindow(const WindowOptions& options) {
    acquireGlfw();

    int width = std::max(options.width, 320);
    int height = std::max(options.height, 240);
    int workX = 0, workY = 0, workW = 0, workH = 0;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        glfwGetMonitorWorkarea(monitor, &workX, &workY, &workW, &workH);
        if (workW > 0 && workH > 0) {
            width = std::min(width, workW);
            height = std::min(height, workH);
        }
    }

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan renders; no GL context
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, options.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, options.maximized ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);  // Windows / X11: size in scaled units on HiDPI

    window_ = glfwCreateWindow(width, height, options.title.c_str(), nullptr, nullptr);
    if (!window_) {
        releaseGlfw();
        throw std::runtime_error("glfwCreateWindow failed");
    }
    if (!options.maximized && workW > 0 && workH > 0) {
        int actualW = 0, actualH = 0;
        glfwGetWindowSize(window_, &actualW, &actualH);
        glfwSetWindowPos(window_, workX + (workW - actualW) / 2, workY + (workH - actualH) / 2);
    }
    glfwSetWindowSizeLimits(window_, 640, 400, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, &PlatformWindow::onFramebufferSize);
    glfwSetDropCallback(window_, &PlatformWindow::onDrop);
    // Input counters for idle detection. Installed before the ImGui GLFW
    // backend, which chains to previously installed callbacks.
    glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double, double) { countInput(w); });
    glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int, int, int) { countInput(w); });
    glfwSetScrollCallback(window_, [](GLFWwindow* w, double, double) { countInput(w); });
    glfwSetKeyCallback(window_, [](GLFWwindow* w, int, int, int, int) { countInput(w); });
    glfwSetCharCallback(window_, [](GLFWwindow* w, unsigned int) { countInput(w); });
    glfwSetWindowFocusCallback(window_, [](GLFWwindow* w, int) { countInput(w); });
    glfwSetCursorEnterCallback(window_, [](GLFWwindow* w, int) { countInput(w); });
    glfwSetWindowRefreshCallback(window_, [](GLFWwindow* w) { countInput(w); });
}

PlatformWindow::~PlatformWindow() {
    if (window_) glfwDestroyWindow(window_);
    releaseGlfw();
}

std::vector<std::string> PlatformWindow::requiredInstanceExtensions() {
    std::uint32_t count = 0;
    const char** names = glfwGetRequiredInstanceExtensions(&count);
    if (!names) throw std::runtime_error("GLFW cannot create Vulkan surfaces on this platform");
    return {names, names + count};
}

VkSurfaceKHR PlatformWindow::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    checkVk(glfwCreateWindowSurface(instance, window_, nullptr, &surface), "glfwCreateWindowSurface");
    return surface;
}

VkExtent2D PlatformWindow::framebufferExtent() const {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    return {static_cast<std::uint32_t>(std::max(width, 0)), static_cast<std::uint32_t>(std::max(height, 0))};
}

bool PlatformWindow::minimized() const {
    const VkExtent2D extent = framebufferExtent();
    return glfwGetWindowAttrib(window_, GLFW_ICONIFIED) == GLFW_TRUE || extent.width == 0 || extent.height == 0;
}

float PlatformWindow::contentScale() const {
    float x = 1.0f, y = 1.0f;
    glfwGetWindowContentScale(window_, &x, &y);
    return std::max(1.0f, std::max(x, y));
}

bool PlatformWindow::shouldClose() const { return glfwWindowShouldClose(window_) == GLFW_TRUE; }

void PlatformWindow::requestClose() { glfwSetWindowShouldClose(window_, GLFW_TRUE); }

void PlatformWindow::show() { glfwShowWindow(window_); }

void PlatformWindow::setTitle(const std::string& title) { glfwSetWindowTitle(window_, title.c_str()); }

bool PlatformWindow::consumeFramebufferResized() noexcept {
    const bool resized = framebufferResized_;
    framebufferResized_ = false;
    return resized;
}

std::vector<std::string> PlatformWindow::takeDroppedPaths() { return std::exchange(droppedPaths_, {}); }

void PlatformWindow::pollEvents() { glfwPollEvents(); }
void PlatformWindow::waitEvents() { glfwWaitEvents(); }
void PlatformWindow::waitEventsTimeout(double seconds) { glfwWaitEventsTimeout(seconds); }
void PlatformWindow::postEmptyEvent() { glfwPostEmptyEvent(); }

void PlatformWindow::countInput(GLFWwindow* window) noexcept {
    if (auto* self = static_cast<PlatformWindow*>(glfwGetWindowUserPointer(window))) ++self->inputEvents_;
}

void PlatformWindow::onFramebufferSize(GLFWwindow* window, int, int) {
    countInput(window);
    if (auto* self = static_cast<PlatformWindow*>(glfwGetWindowUserPointer(window))) self->framebufferResized_ = true;
}

void PlatformWindow::onDrop(GLFWwindow* window, int count, const char** paths) {
    countInput(window);
    auto* self = static_cast<PlatformWindow*>(glfwGetWindowUserPointer(window));
    if (!self) return;
    for (int i = 0; i < count; ++i) self->droppedPaths_.emplace_back(paths[i]);  // GLFW frees them after the callback
}

}  // namespace darkhouse
