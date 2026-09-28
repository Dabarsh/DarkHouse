#include "ui/shell.hpp"

#include <imgui_internal.h>  // BeginViewportSideBar

namespace darkhouse::ui {
namespace {

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

void DarkHouseShell::draw(DarkHouseApp& app, const FrameContext& frame, GuiEngine& gui) {
    // Side bars shrink the viewport's work area, so they come before the dockspace.
    drawStatusBar(app, frame, gui);
    ImGui::DockSpaceOverViewport(ImGui::GetID("DarkHouseDockSpace"), ImGui::GetMainViewport());
    if (showEngineStats_) drawEngineStats(app, frame, gui);
}

void DarkHouseShell::drawStatusBar(DarkHouseApp& app, const FrameContext& frame, GuiEngine&) {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_MenuBar;
    const float height = ImGui::GetFrameHeight();
    if (ImGui::BeginViewportSideBar("##DarkHouseStatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::TextUnformatted("DarkHouse");
            ImGui::Separator();
            ImGui::Text("Mode: %.*s", static_cast<int>(toString(frame.mode).size()), toString(frame.mode).data());
            ImGui::Separator();
            if (app.pendingImportCount() > 0) {
                ImGui::Text("Importing %zu file(s)", app.pendingImportCount());
            } else {
                ImGui::Text("Imported %zu", app.summary().importsSucceeded);
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void DarkHouseShell::drawEngineStats(DarkHouseApp& app, const FrameContext& frame, GuiEngine& gui) {
    if (!ImGui::Begin("Engine", &showEngineStats_)) {
        ImGui::End();
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (const VulkanContext* gpu = gui.gpu()) {
        ImGui::Text("Device: %s", gpu->deviceName().c_str());
        ImGui::Text("Validation: %s", gpu->validationEnabled() ? "on" : "off");
    }
    if (const Swapchain* swapchain = gui.swapchain()) {
        ImGui::Text("Swapchain: %ux%u, %u images, %s", swapchain->extent().width, swapchain->extent().height,
                    swapchain->imageCount(), presentModeName(swapchain->presentMode()));
    }
    ImGui::Text("Frame: %.2f ms (%.0f fps)", 1000.0f / (io.Framerate > 0.0f ? io.Framerate : 1.0f), io.Framerate);
    ImGui::Text("Engine frame: %llu, dt %.2f ms", static_cast<unsigned long long>(frame.frameIndex),
                frame.deltaSeconds * 1000.0);
    ImGui::Text("Canvas: %s", app.canvasAvailable() ? "GPU develop graph" : "unavailable");
    ImGui::End();
}

}  // namespace darkhouse::ui
