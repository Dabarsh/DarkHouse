#include "ui/panel.hpp"

namespace darkhouse::ui {
namespace {

constexpr std::array<PanelInfo, kPanelCount> kPanels{{
    {PanelId::COLLECTIONS, "Collections", "Collections", "Browse folders, collections and smart collections"},
    {PanelId::SEARCH, "Search", "Search", "Search and filter the library by rating, flags, camera and exposure"},
    {PanelId::METADATA, "Metadata", "Metadata", "Capture and file metadata of the selected asset"},
    {PanelId::ASSET_GRID, "Library", "Library", "Thumbnail grid of the current collection"},
    {PanelId::VIEWPORT, "Viewport", "Viewport", "The developed canvas"},
    {PanelId::FILMSTRIP, "Filmstrip", "Filmstrip", "The current collection as a strip, for quick navigation"},
    {PanelId::LAYERS, "Layers", "Layers", "Unified stack of parametric, raster, vector and smart-object layers"},
    {PanelId::ADJUSTMENTS, "Adjustments", "Adjustments", "Tone, HSL and colour adjustments of the active layer"},
    {PanelId::ENGINE, "Engine", "Engine", "GPU, swapchain and frame timing diagnostics"},
}};

constexpr std::array<PanelId, kPanelCount> kAllPanels{
    PanelId::COLLECTIONS, PanelId::SEARCH, PanelId::METADATA,    PanelId::ASSET_GRID, PanelId::VIEWPORT,
    PanelId::FILMSTRIP,   PanelId::LAYERS, PanelId::ADJUSTMENTS, PanelId::ENGINE,
};

const char* presentModeName(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    default: return "other";
    }
}

}  // namespace

const PanelInfo& panelInfo(PanelId id) noexcept { return kPanels[index(id)]; }

const std::array<PanelId, kPanelCount>& allPanels() noexcept { return kAllPanels; }

void EnginePanel::draw(PanelContext& ctx) {
    const ImGuiIO& io = ImGui::GetIO();
    if (const VulkanContext* gpu = ctx.gui.gpu()) {
        ImGui::Text("Device: %s", gpu->deviceName().c_str());
        ImGui::Text("Validation: %s (%u errors)", gpu->validationEnabled() ? "on" : "off", gpu->validationErrorCount());
    }
    if (const Swapchain* swapchain = ctx.gui.swapchain()) {
        ImGui::Text("Swapchain: %ux%u, %u images, %s", swapchain->extent().width, swapchain->extent().height,
                    swapchain->imageCount(), presentModeName(swapchain->presentMode()));
    }
    ImGui::Text("Frame: %.2f ms (%.0f fps)%s", 1000.0f / (io.Framerate > 0.0f ? io.Framerate : 1.0f), io.Framerate,
                ctx.gui.idle() ? ", idle" : "");
    ImGui::Text("Engine frame: %llu, dt %.2f ms", static_cast<unsigned long long>(ctx.frame.frameIndex),
                ctx.frame.deltaSeconds * 1000.0);
    ImGui::Text("Canvas: %s", ctx.app.canvasAvailable() ? "GPU develop graph" : "unavailable");
    ImGui::Text("Imports: %zu ok, %zu failed, %zu pending", ctx.app.summary().importsSucceeded,
                ctx.app.summary().importsFailed, ctx.app.pendingImportCount());
}

}  // namespace darkhouse::ui
