#include "platform_window.hpp"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE  // vulkan.h is already included; GLFW must not pull in GL headers
#endif
#include <GLFW/glfw3.h>

#include "vulkan_utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace darkhouse {
namespace {

int g_glfwUsers = 0;  // GLFW is initialized while at least one window exists (main thread only)

constexpr int kMinWidth = 640;
constexpr int kMinHeight = 400;

// The DarkHouse icon, drawn procedurally: a safelight glowing on a dark
// rounded tile. Wayland and macOS take icons from the desktop entry / app
// bundle instead, and GLFW reports an error there, so it is skipped.
void setIcon(GLFWwindow* window) {
    const int platform = glfwGetPlatform();
    if (platform == GLFW_PLATFORM_WAYLAND || platform == GLFW_PLATFORM_COCOA) return;

    constexpr std::array<int, 4> kSizes{16, 32, 48, 64};
    std::array<std::vector<unsigned char>, kSizes.size()> pixels;
    std::array<GLFWimage, kSizes.size()> images{};
    for (std::size_t i = 0; i < kSizes.size(); ++i) {
        const int size = kSizes[i];
        pixels[i].resize(static_cast<std::size_t>(size) * size * 4);
        const float half = static_cast<float>(size) * 0.5f;
        const float corner = static_cast<float>(size) * 0.22f;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float px = static_cast<float>(x) + 0.5f - half;
                const float py = static_cast<float>(y) + 0.5f - half;
                // Rounded-square coverage (signed distance, 1 px anti-aliasing).
                const float qx = std::max(std::fabs(px) - (half - corner), 0.0f);
                const float qy = std::max(std::fabs(py) - (half - corner), 0.0f);
                const float tile = std::clamp(corner - std::sqrt(qx * qx + qy * qy) + 0.5f, 0.0f, 1.0f);
                // Safelight: bright core with a soft glow.
                const float r = std::sqrt(px * px + py * py) / half;
                const float core = std::clamp((0.42f - r) * static_cast<float>(size) * 0.25f, 0.0f, 1.0f);
                const float glow = std::clamp(1.0f - r / 0.85f, 0.0f, 1.0f) * 0.55f;
                const float light = std::max(core, glow * glow);
                unsigned char* p = &pixels[i][(static_cast<std::size_t>(y) * size + x) * 4];
                p[0] = static_cast<unsigned char>(28 + (227 - 28) * light);
                p[1] = static_cast<unsigned char>(28 + (84 - 28) * light);
                p[2] = static_cast<unsigned char>(30 + (61 - 30) * light);
                p[3] = static_cast<unsigned char>(255.0f * tile);
            }
        }
        images[i] = GLFWimage{size, size, pixels[i].data()};
    }
    glfwSetWindowIcon(window, static_cast<int>(images.size()), images.data());
}

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

    int workX = 0, workY = 0, workW = 0, workH = 0;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) glfwGetMonitorWorkarea(monitor, &workX, &workY, &workW, &workH);
    const bool haveWorkArea = workW > 0 && workH > 0;
    int width = options.width;
    int height = options.height;
    if (width <= 0 || height <= 0) {
        // Automatic: most of the screen, which is what an editor wants,
        // without covering it entirely the way maximized would.
        width = haveWorkArea ? std::max(workW * 85 / 100, std::min(1280, workW)) : 1600;
        height = haveWorkArea ? std::max(workH * 85 / 100, std::min(800, workH)) : 1000;
    }
    width = std::max(width, kMinWidth);
    height = std::max(height, kMinHeight);
    if (haveWorkArea) {
        width = std::min(width, workW);
        height = std::min(height, workH);
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
    glfwSetWindowSizeLimits(window_, kMinWidth, kMinHeight, GLFW_DONT_CARE, GLFW_DONT_CARE);
    setIcon(window_);
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
