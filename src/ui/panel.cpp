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
    {PanelId::LAYERS, "Layers", "Layers", "Layer stack (pixel, adjustment, vector, smart object, group) with blend mode and opacity"},
    {PanelId::ADJUSTMENTS, "Adjustments", "Adjustments", "Develop: white balance, tone, colour mixer, colour grading and detail of the photo"},
    {PanelId::ENGINE, "Engine", "Engine", "GPU, swapchain and frame timing diagnostics"},
    {PanelId::MASKING, "Masking", "Masking", "Masks for local adjustments: brush, gradients, colour and luminance ranges"},
    {PanelId::PROPERTIES, "Properties", "Properties", "The selected layer: name, layer mask, content and transform"},
    {PanelId::TOOLS, "Tools", "Tools", "Move, brush, eraser, clone stamp, pen, direct selection and eyedropper, with options"},
    {PanelId::CHANNELS, "Channels", "Channels", "Show the red, green, blue or alpha channel, or the selected layer's mask"},
    {PanelId::PATHS, "Paths", "Paths", "Vector paths: draw with the pen, edit points, fill and stroke, turn into a mask"},
}};

// Menu order: catalog panels, the canvas, develop panels, compositing panels.
constexpr std::array<PanelId, kPanelCount> kAllPanels{
    PanelId::COLLECTIONS, PanelId::SEARCH,  PanelId::METADATA, PanelId::ASSET_GRID, PanelId::VIEWPORT, PanelId::FILMSTRIP,
    PanelId::ADJUSTMENTS, PanelId::MASKING, PanelId::TOOLS,    PanelId::LAYERS,     PanelId::PROPERTIES,
    PanelId::CHANNELS,    PanelId::PATHS,   PanelId::ENGINE,
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
    ImGui::Text("Develop evaluations: %llu%s", static_cast<unsigned long long>(ctx.app.developEvaluations()),
                ctx.app.developBusy() ? " (one in flight)" : "");
    if (ctx.app.canvasAvailable()) {
        ImGui::Text("Document: %u x %u", ctx.app.canvasWidth(), ctx.app.canvasHeight());
        const auto& timings = ctx.app.developTimings();
        if (timings.empty()) {
            ImGui::TextDisabled("Develop graph GPU time: not measured (no timestamp support)");
        } else {
            // One line per node, then the total: what the last re-render cost on the GPU.
            double total = 0.0;
            for (const auto& timing : timings) total += timing.milliseconds;
            const double megapixels = static_cast<double>(ctx.app.canvasWidth()) * ctx.app.canvasHeight() / 1e6;
            ImGui::Text("Develop graph GPU time: %.2f ms (%.0f MP/s)", total, total > 0.0 ? megapixels / total * 1e3 : 0.0);
            ImGui::Indent();
            for (const auto& timing : timings) ImGui::Text("%-14s %8.2f ms", timing.type.c_str(), timing.milliseconds);
            ImGui::Unindent();
        }
    }
    ImGui::Text("Imports: %zu ok, %zu failed, %zu pending", ctx.app.summary().importsSucceeded,
                ctx.app.summary().importsFailed, ctx.app.pendingImportCount());
}

}  // namespace darkhouse::ui
