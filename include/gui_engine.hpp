// DarkHouse — Dear ImGui desktop front-end.
//
// GuiEngine is the FrontEnd that puts DarkHouse on screen. It owns the GLFW
// window, the swapchain and the Dear ImGui context (GLFW platform backend +
// Vulkan renderer backend using dynamic rendering), and it shares the engine's
// VulkanContext, so develop-graph outputs are sampled straight into the UI.
//
// Per frame (called from DarkHouseApp::run):
//   pumpPlatformEvents  poll GLFW, track resizes, report window close
//   drawFrame           ImGui new frame -> GuiLayer::draw -> render into the
//                       acquired swapchain image -> present (+ OS windows for
//                       detached panels when multi-viewport is enabled)
//
// What is drawn is up to the GuiLayer (the DarkHouse shell); the engine only
// provides the frame, textures and window services.
#pragma once

#include "app_controller.hpp"
#include "platform_window.hpp"
#include "swapchain.hpp"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace darkhouse {

class GuiEngine;

// Application UI drawn by the GuiEngine every frame.
class GuiLayer {
public:
    virtual ~GuiLayer() = default;
    // ImGui context and renderer exist; load fonts, styles, textures here.
    virtual void onAttach(GuiEngine& /*gui*/) {}
    // Renderer is about to shut down; release any ImTextureID created through the engine.
    virtual void onDetach(GuiEngine& /*gui*/) {}
    // Builds the UI for this frame. Called between ImGui::NewFrame and ImGui::Render.
    virtual void draw(DarkHouseApp& app, const FrameContext& frame, GuiEngine& gui) = 0;
};

struct GuiOptions {
    WindowOptions window;
    bool vsync = true;               // FIFO presentation; the frame loop then skips its own sleep
    bool multiViewport = false;      // panels can be dragged out into their own OS windows
    // Block for input when nothing changes instead of redrawing every
    // vsync: an idle DarkHouse window then costs ~4 frames per second.
    bool lowPowerIdle = true;
    std::filesystem::path iniPath;   // persisted docking layout; empty = don't persist
};

class GuiEngine final : public FrontEnd {
public:
    // Opens the window and creates the ImGui context. Throws std::runtime_error
    // when no window can be created (no display, no Vulkan loader).
    GuiEngine(GuiOptions options, std::unique_ptr<GuiLayer> layer);
    ~GuiEngine() override;

    GuiEngine(const GuiEngine&) = delete;
    GuiEngine& operator=(const GuiEngine&) = delete;

    // FrontEnd
    void configureGpu(VulkanContextOptions& options) override;
    [[nodiscard]] bool requiresGpu() const noexcept override { return true; }
    void attachGpu(DarkHouseApp& app, VulkanContext& gpu) override;
    void detachGpu() noexcept override;
    bool pumpPlatformEvents(DarkHouseApp& app) override;
    void drawFrame(DarkHouseApp& app, const FrameContext& frame) override;
    [[nodiscard]] bool interactive() const noexcept override { return true; }
    [[nodiscard]] bool pacesFrames() const noexcept override { return options_.vsync; }

    // --- Services for the GuiLayer ------------------------------------------

    // The developed canvas for this frame, or ImTextureID_Invalid when the
    // canvas was not rendered this frame (CATALOG mode, GPU canvas disabled).
    [[nodiscard]] ImTextureID canvasTexture() const noexcept { return canvasTextureThisFrame_; }
    [[nodiscard]] ImVec2 canvasSize() const noexcept { return canvasSize_; }

    [[nodiscard]] PlatformWindow& window() noexcept { return *window_; }
    [[nodiscard]] const VulkanContext* gpu() const noexcept { return gpu_; }
    [[nodiscard]] const Swapchain* swapchain() const noexcept { return swapchain_.get(); }
    [[nodiscard]] bool vsync() const noexcept { return options_.vsync; }
    void setVsync(bool vsync);
    [[nodiscard]] bool multiViewport() const noexcept { return options_.multiViewport; }
    [[nodiscard]] bool lowPowerIdle() const noexcept { return options_.lowPowerIdle; }
    void setLowPowerIdle(bool enabled) noexcept { options_.lowPowerIdle = enabled; }
    // True while the UI is waiting for input between frames (for diagnostics).
    [[nodiscard]] bool idle() const noexcept { return idleFrames_ >= kSettleFrames; }
    [[nodiscard]] const std::filesystem::path& iniPath() const noexcept { return options_.iniPath; }
    [[nodiscard]] std::uint64_t presentedFrames() const noexcept { return presentedFrames_; }
    void requestClose();

private:
    void initImGui();
    void initRenderer();
    void updateCanvasTexture(const FrameContext& frame);
    void retireTexture(VkDescriptorSet set);
    void releaseRetiredTextures(bool all) noexcept;
    void renderMainViewport(ImDrawData* drawData);

    GuiOptions options_;
    std::string iniPathUtf8_;  // storage for io.IniFilename
    std::unique_ptr<PlatformWindow> window_;
    std::unique_ptr<GuiLayer> layer_;
    ImGuiContext* imgui_ = nullptr;
    bool platformBackendReady_ = false;

    VulkanContext* gpu_ = nullptr;  // not owned; valid between attachGpu and detachGpu
    std::unique_ptr<Swapchain> swapchain_;
    bool rendererReady_ = false;
    VkFormat rendererFormat_ = VK_FORMAT_UNDEFINED;  // colour format the ImGui pipeline was built for
    std::uint32_t rendererMinImages_ = 0;
    bool windowShown_ = false;

    // Develop output registered with the ImGui backend, keyed on the engine's
    // canvas generation (see FrameContext::canvasGeneration).
    VkDescriptorSet canvasSet_ = VK_NULL_HANDLE;
    VkImageView canvasView_ = VK_NULL_HANDLE;
    std::uint64_t canvasGeneration_ = ~std::uint64_t{0};
    ImTextureID canvasTextureThisFrame_ = ImTextureID_Invalid;
    ImVec2 canvasSize_{0.0f, 0.0f};
    // Descriptor sets still referenced by frames in flight: (release after frame, set).
    std::vector<std::pair<std::uint64_t, VkDescriptorSet>> retiredTextures_;

    std::uint64_t presentedFrames_ = 0;

    // Idle pacing: frames drawn since the last input or activity. ImGui needs
    // a few frames after an event to settle hover state and layout.
    static constexpr std::uint32_t kSettleFrames = 4;
    std::uint32_t idleFrames_ = 0;
    std::uint64_t lastInputEvents_ = 0;
};

}  // namespace darkhouse
