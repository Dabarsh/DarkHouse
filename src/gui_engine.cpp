#include "gui_engine.hpp"

#include "vulkan_utils.hpp"

#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace darkhouse {
namespace {

// Descriptor sets retired in frame N may be referenced by up to
// kMaxFramesInFlight submitted frames; one extra frame of slack.
constexpr std::uint64_t kTextureRetireDelay = Swapchain::kMaxFramesInFlight + 1;

void onImGuiVulkanResult(VkResult result) {
    if (result == VK_SUCCESS) return;
    std::clog << "[DarkHouse] " << (result < 0 ? "error" : "warn") << ": ImGui Vulkan backend: " << vkResultName(result)
              << '\n';
}

ImTextureID toTextureId(VkDescriptorSet set) {
    // VkDescriptorSet is a pointer on 64-bit targets and a uint64_t on 32-bit
    // ones; the C-style cast is the one conversion valid for both.
    return (ImTextureID)set;  // NOLINT(google-readability-casting)
}

}  // namespace

GuiEngine::GuiEngine(GuiOptions options, std::unique_ptr<GuiLayer> layer)
    : options_(std::move(options)), layer_(std::move(layer)) {
    if (!layer_) throw std::invalid_argument("GuiEngine needs a GuiLayer");
    WindowOptions windowOptions = options_.window;
    windowOptions.visible = false;  // shown once the first frame has been presented (no blank flash)
    window_ = std::make_unique<PlatformWindow>(windowOptions);
    try {
        initImGui();
    } catch (...) {
        if (platformBackendReady_) ImGui_ImplGlfw_Shutdown();
        if (imgui_) ImGui::DestroyContext(imgui_);
        throw;
    }
}

GuiEngine::~GuiEngine() {
    detachGpu();
    if (imgui_) {
        ImGui::SetCurrentContext(imgui_);
        if (platformBackendReady_) ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    // window_ is destroyed last (member order), after every ImGui platform window.
}

void GuiEngine::initImGui() {
    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (options_.multiViewport) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;  // dragging inside a panel never tears it off

    if (options_.iniPath.empty()) {
        io.IniFilename = nullptr;
    } else {
        std::error_code ec;
        std::filesystem::create_directories(options_.iniPath.parent_path(), ec);
        iniPathUtf8_ = options_.iniPath.string();
        io.IniFilename = iniPathUtf8_.c_str();
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    layer_->configureStyle(style, io);
    const float scale = window_->contentScale();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        // Detached panels are OS windows; square, opaque windows look native.
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    if (!ImGui_ImplGlfw_InitForVulkan(window_->handle(), /*install_callbacks=*/true)) {
        throw std::runtime_error("ImGui_ImplGlfw_InitForVulkan failed");
    }
    platformBackendReady_ = true;
}

void GuiEngine::configureGpu(VulkanContextOptions& options) {
    options.applicationName = "DarkHouse";
    options.instanceExtensions = PlatformWindow::requiredInstanceExtensions();
    options.createSurface = [this](VkInstance instance) { return window_->createSurface(instance); };
}

void GuiEngine::attachGpu(DarkHouseApp&, VulkanContext& gpu) {
    gpu_ = &gpu;
    try {
        SwapchainOptions swapchainOptions;
        swapchainOptions.vsync = options_.vsync;
        swapchain_ = std::make_unique<Swapchain>(gpu, window_->framebufferExtent(), swapchainOptions);
        initRenderer();
        layer_->onAttach(*this);
    } catch (...) {
        detachGpu();
        throw;
    }
}

void GuiEngine::initRenderer() {
    ImGui::SetCurrentContext(imgui_);
    rendererFormat_ = swapchain_->format();
    rendererMinImages_ = swapchain_->minImageCount();

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = gpu_->apiVersion();
    init.Instance = gpu_->instance();
    init.PhysicalDevice = gpu_->physicalDevice();
    init.Device = gpu_->device();
    init.QueueFamily = gpu_->queueFamily();
    init.Queue = gpu_->queue();
    // Room for the backend's font atlas plus the canvas, thumbnails and filmstrip.
    init.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + 256;
    init.MinImageCount = rendererMinImages_;
    init.ImageCount = std::max(swapchain_->imageCount(), rendererMinImages_);
    init.UseDynamicRendering = true;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &rendererFormat_;
    init.CheckVkResultFn = onImGuiVulkanResult;
    if (!ImGui_ImplVulkan_Init(&init)) throw std::runtime_error("ImGui_ImplVulkan_Init failed");
    rendererReady_ = true;
}

void GuiEngine::detachGpu() noexcept {
    if (!gpu_) return;
    gpu_->waitIdle();
    ImGui::SetCurrentContext(imgui_);
    if (rendererReady_) {
        layer_->onDetach(*this);
        retireTexture(canvasSet_);
        canvasSet_ = VK_NULL_HANDLE;
        canvasView_ = VK_NULL_HANDLE;
        canvasTextureThisFrame_ = ImTextureID_Invalid;
        releaseRetiredTextures(/*all=*/true);
        ImGui_ImplVulkan_Shutdown();  // also destroys detached-panel OS windows' Vulkan resources
        rendererReady_ = false;
    }
    swapchain_.reset();
    gpu_ = nullptr;
}

bool GuiEngine::pumpPlatformEvents(DarkHouseApp& app) {
    // Background work that must keep the loop turning, but not at full rate.
    const bool engineBusy = app.pendingImportCount() > 0;
    if (window_->minimized()) {
        // Nothing to draw. Block briefly instead of spinning, but keep the
        // engine loop turning so imports still complete while minimized.
        PlatformWindow::waitEventsTimeout(0.1);
    } else if (options_.lowPowerIdle && idleFrames_ >= kSettleFrames) {
        // Idle: sleep until input arrives. Wakes at 4 Hz anyway so delayed
        // tooltips and status changes still appear, and faster while
        // imports are running so progress keeps moving.
        PlatformWindow::waitEventsTimeout(engineBusy ? 0.1 : 0.25);
    } else {
        PlatformWindow::pollEvents();
    }
    if (window_->inputEventCount() != lastInputEvents_) {
        lastInputEvents_ = window_->inputEventCount();
        idleFrames_ = 0;
    }
    if (window_->consumeFramebufferResized() && swapchain_) swapchain_->requestRecreate();
    return !window_->shouldClose();
}

void GuiEngine::drawFrame(DarkHouseApp& app, const FrameContext& frame) {
    if (!rendererReady_) return;
    ImGui::SetCurrentContext(imgui_);
    updateCanvasTexture(frame);
    if (window_->minimized()) return;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    layer_->draw(app, frame, *this);
    // Held widgets and text entry keep drawing at full rate (drag feedback,
    // caret blink); everything else counts towards going idle.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsAnyItemActive() || io.WantTextInput) {
        idleFrames_ = 0;
    } else if (idleFrames_ < kSettleFrames) {
        ++idleFrames_;
    }
    ImGui::Render();
    renderMainViewport(ImGui::GetDrawData());

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    if (!windowShown_ && presentedFrames_ > 0) {
        window_->show();
        windowShown_ = true;
    }
    releaseRetiredTextures(/*all=*/false);
}

void GuiEngine::renderMainViewport(ImDrawData* drawData) {
    const auto target = swapchain_->beginFrame(window_->framebufferExtent());
    if (!target) return;  // minimized or swapchain rebuilt; ImGui's draw data is simply dropped

    // A rebuilt swapchain may come back with another format or image count.
    if (swapchain_->format() != rendererFormat_) {
        rendererFormat_ = swapchain_->format();
        ImGui_ImplVulkan_PipelineInfo pipeline{};
        pipeline.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        pipeline.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        pipeline.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        pipeline.PipelineRenderingCreateInfo.pColorAttachmentFormats = &rendererFormat_;
        ImGui_ImplVulkan_CreateMainPipeline(&pipeline);
    }
    if (swapchain_->minImageCount() != rendererMinImages_) {
        rendererMinImages_ = swapchain_->minImageCount();
        ImGui_ImplVulkan_SetMinImageCount(rendererMinImages_);
    }

    // The canvas is written by the develop graph's compute pass earlier on the
    // same queue; RenderPipelineGraph::evaluateGraph already ends with a barrier
    // that makes those writes visible to every later stage, so no extra
    // barrier is needed before the fragment shader samples it.
    const ImVec4 background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target->view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{background.x, background.y, background.z, 1.0f}};
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, target->extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;

    try {
        vkCmdBeginRendering(target->commandBuffer, &rendering);
        ImGui_ImplVulkan_RenderDrawData(drawData, target->commandBuffer);
        vkCmdEndRendering(target->commandBuffer);
    } catch (...) {
        swapchain_->endFrame(*target);  // always consume the acquire semaphore
        throw;
    }
    swapchain_->endFrame(*target);
    ++presentedFrames_;
}

void GuiEngine::updateCanvasTexture(const FrameContext& frame) {
    canvasTextureThisFrame_ = ImTextureID_Invalid;
    if (frame.canvasGeneration != canvasGeneration_) {
        // The engine rebuilt or dropped the develop graph: the old image is gone.
        retireTexture(canvasSet_);
        canvasSet_ = VK_NULL_HANDLE;
        canvasView_ = VK_NULL_HANDLE;
        canvasGeneration_ = frame.canvasGeneration;
    }
    const GPUTexture* output = frame.canvasOutput;
    if (!output || !output->valid()) return;
    if (canvasSet_ == VK_NULL_HANDLE || canvasView_ != output->view) {
        retireTexture(canvasSet_);
        // The develop graph leaves its outputs in GENERAL, which is valid for sampling.
        canvasSet_ = ImGui_ImplVulkan_AddTexture(output->view, VK_IMAGE_LAYOUT_GENERAL);
        canvasView_ = output->view;
    }
    canvasTextureThisFrame_ = toTextureId(canvasSet_);
    canvasSize_ = ImVec2(static_cast<float>(output->width), static_cast<float>(output->height));
}

void GuiEngine::retireTexture(VkDescriptorSet set) {
    if (set != VK_NULL_HANDLE) retiredTextures_.emplace_back(presentedFrames_ + kTextureRetireDelay, set);
}

void GuiEngine::releaseRetiredTextures(bool all) noexcept {
    for (auto it = retiredTextures_.begin(); it != retiredTextures_.end();) {
        if (all || presentedFrames_ >= it->first) {
            ImGui_ImplVulkan_RemoveTexture(it->second);
            it = retiredTextures_.erase(it);
        } else {
            ++it;
        }
    }
}

void GuiEngine::setVsync(bool vsync) {
    options_.vsync = vsync;
    if (swapchain_) swapchain_->setVsync(vsync);
}

void GuiEngine::requestClose() { window_->requestClose(); }

}  // namespace darkhouse
