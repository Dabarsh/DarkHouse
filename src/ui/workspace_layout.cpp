#include "ui/workspace_layout.hpp"

#include <imgui_internal.h>  // DockBuilder API, ImHashStr

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace darkhouse::ui {
namespace {

constexpr std::array<AppMode, kWorkspaceCount> kModes{AppMode::CATALOG, AppMode::DEVELOP, AppMode::CANVAS,
                                                      AppMode::HYBRID_SPLIT};

// Splits `node` and returns the new node on `dir`; `node` becomes the remainder.
ImGuiID split(ImGuiID& node, ImGuiDir dir, float ratio) {
    ImGuiID remainder = 0;
    const ImGuiID created = ImGui::DockBuilderSplitNode(node, dir, ratio, nullptr, &remainder);
    node = remainder;
    return created;
}

}  // namespace

const std::array<AppMode, kWorkspaceCount>& workspaceModes() noexcept { return kModes; }

const char* workspaceTitle(AppMode mode) noexcept {
    switch (mode) {
    case AppMode::CATALOG: return "Catalog";
    case AppMode::DEVELOP: return "Develop (Lightroom)";
    case AppMode::CANVAS: return "Canvas & Compositing (Photoshop)";
    case AppMode::HYBRID_SPLIT: return "Split";
    }
    return "?";
}

const char* workspaceShortTitle(AppMode mode) noexcept {
    switch (mode) {
    case AppMode::CATALOG: return "Catalog";
    case AppMode::DEVELOP: return "Develop";
    case AppMode::CANVAS: return "Canvas";
    case AppMode::HYBRID_SPLIT: return "Split";
    }
    return "?";
}

WorkspaceLayoutManager::WorkspaceLayoutManager() {
    for (AppMode mode : kModes) {
        Workspace& workspace = workspaces_[index(mode)];
        workspace.mode = mode;
        const std::string prefix =
            std::string("DarkHouse.") + workspaceShortTitle(mode) + ".v" + std::to_string(kLayoutVersion);
        workspace.dockspace = ImHashStr((prefix + ".DockSpace").c_str());
        for (PanelId panel : allPanels()) {
            const PanelInfo& info = panelInfo(panel);
            workspace.windowNames[index(panel)] = std::string(info.title) + "###" + prefix + "." + info.key;
        }
        applyDefaultPanelSet(workspace);
        bringFrontTabs(workspace);
    }
}

void WorkspaceLayoutManager::applyDefaultPanelSet(Workspace& workspace) {
    workspace.open.fill(false);
    auto show = [&](std::initializer_list<PanelId> panels) {
        for (PanelId panel : panels) workspace.open[index(panel)] = true;
    };
    switch (workspace.mode) {
    case AppMode::CATALOG:
        show({PanelId::COLLECTIONS, PanelId::SEARCH, PanelId::METADATA, PanelId::ASSET_GRID});
        break;
    case AppMode::DEVELOP:
        show({PanelId::COLLECTIONS, PanelId::SEARCH, PanelId::METADATA, PanelId::VIEWPORT, PanelId::FILMSTRIP,
              PanelId::ADJUSTMENTS, PanelId::MASKING});
        break;
    case AppMode::CANVAS:
        show({PanelId::TOOLS, PanelId::VIEWPORT, PanelId::LAYERS, PanelId::CHANNELS, PanelId::PATHS, PanelId::PROPERTIES,
              PanelId::MASKING});
        break;
    case AppMode::HYBRID_SPLIT:
        show({PanelId::COLLECTIONS, PanelId::SEARCH, PanelId::METADATA, PanelId::ASSET_GRID, PanelId::VIEWPORT,
              PanelId::FILMSTRIP, PanelId::ADJUSTMENTS, PanelId::MASKING});
        break;
    }
}

void WorkspaceLayoutManager::resetLayout(AppMode mode) noexcept { workspaces_[index(mode)].resetPending = true; }

const char* WorkspaceLayoutManager::windowName(AppMode mode, PanelId panel) const noexcept {
    return workspaces_[index(mode)].windowNames[index(panel)].c_str();
}

bool* WorkspaceLayoutManager::panelOpen(AppMode mode, PanelId panel) noexcept {
    return &workspaces_[index(mode)].open[index(panel)];
}

ImGuiID WorkspaceLayoutManager::dockspaceId(AppMode mode) const noexcept { return workspaces_[index(mode)].dockspace; }

void WorkspaceLayoutManager::submit(AppMode active) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // A workspace's default layout is built when it is first shown (or
    // reset), so its windows appear right after and open on the intended
    // tabs. Until then it has no dock node and gets no keep-alive DockSpace()
    // below, which would create an empty node that hides the missing layout.
    Workspace& current = workspaces_[index(active)];
    if (current.resetPending || ImGui::DockBuilderGetNode(current.dockspace) == nullptr) {
        if (current.resetPending) applyDefaultPanelSet(current);
        buildDefaultLayout(current, viewport->WorkSize);
        current.resetPending = false;
    }

    // Same host window as ImGui::DockSpaceOverViewport(), but it also carries
    // the keep-alive dockspaces of the inactive workspaces.
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DarkHouseWorkspaceHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(dockspaceId(active), ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    for (const Workspace& workspace : workspaces_) {
        if (workspace.mode != active && ImGui::DockBuilderGetNode(workspace.dockspace) != nullptr) {
            ImGui::DockSpace(workspace.dockspace, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_KeepAliveOnly);
        }
    }
    ImGui::End();
}

void WorkspaceLayoutManager::buildDefaultLayout(Workspace& workspace, ImVec2 size) {
    const ImGuiID root = workspace.dockspace;
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, size);

    auto dock = [&](PanelId panel, ImGuiID node) {
        ImGui::DockBuilderDockWindow(workspace.windowNames[index(panel)].c_str(), node);
    };
    // Side columns and the filmstrip are sized in (DPI-scaled) pixels rather
    // than fractions: a fixed fraction is too wide on a 4K display and too
    // cramped on a laptop. The ratio is clamped so small windows stay usable.
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    float width = size.x;
    float height = size.y;
    auto splitWidth = [&](ImGuiID& node, ImGuiDir dir, float pixels, float minRatio, float maxRatio) {
        const float ratio = std::clamp(pixels * dpi / std::max(width, 1.0f), minRatio, maxRatio);
        width *= 1.0f - ratio;
        return split(node, dir, ratio);
    };
    auto splitHeight = [&](ImGuiID& node, ImGuiDir dir, float pixels, float minRatio, float maxRatio) {
        const float ratio = std::clamp(pixels * dpi / std::max(height, 1.0f), minRatio, maxRatio);
        height *= 1.0f - ratio;
        return split(node, dir, ratio);
    };

    ImGuiID center = root;
    switch (workspace.mode) {
    case AppMode::CATALOG: {
        // | Search      |                 |
        // | Collections |  Library grid   |
        // | Metadata    |                 |
        ImGuiID left = splitWidth(center, ImGuiDir_Left, 320.0f, 0.16f, 0.30f);
        const ImGuiID metadata = split(left, ImGuiDir_Down, 0.36f);
        const ImGuiID search = split(left, ImGuiDir_Up, 0.47f);
        dock(PanelId::SEARCH, search);
        dock(PanelId::COLLECTIONS, left);
        dock(PanelId::METADATA, metadata);
        dock(PanelId::ASSET_GRID, center);
        break;
    }
    case AppMode::DEVELOP: {
        // | Collections |  Viewport       | Adjustments |
        // | Metadata    |-----------------|  (Masking)  |
        // |             |  Filmstrip      |             |
        const ImGuiID right = splitWidth(center, ImGuiDir_Right, 380.0f, 0.20f, 0.32f);
        ImGuiID left = splitWidth(center, ImGuiDir_Left, 280.0f, 0.14f, 0.26f);
        const ImGuiID metadata = split(left, ImGuiDir_Down, 0.50f);
        const ImGuiID filmstrip = splitHeight(center, ImGuiDir_Down, 150.0f, 0.12f, 0.28f);
        dock(PanelId::COLLECTIONS, left);
        dock(PanelId::SEARCH, left);
        dock(PanelId::METADATA, metadata);
        dock(PanelId::FILMSTRIP, filmstrip);
        dock(PanelId::ADJUSTMENTS, right);
        dock(PanelId::MASKING, right);
        dock(PanelId::VIEWPORT, center);
        break;
    }
    case AppMode::CANVAS: {
        // | Tools |                          | Properties (Masking)       |
        // |       |  Viewport                |----------------------------|
        // |       |                          | Layers (Channels, Paths)   |
        ImGuiID right = splitWidth(center, ImGuiDir_Right, 360.0f, 0.18f, 0.30f);
        const ImGuiID layers = split(right, ImGuiDir_Down, 0.55f);
        const ImGuiID tools = splitWidth(center, ImGuiDir_Left, 210.0f, 0.10f, 0.18f);
        dock(PanelId::TOOLS, tools);
        dock(PanelId::PROPERTIES, right);
        dock(PanelId::MASKING, right);
        dock(PanelId::LAYERS, layers);
        dock(PanelId::CHANNELS, layers);
        dock(PanelId::PATHS, layers);
        dock(PanelId::VIEWPORT, center);
        break;
    }
    case AppMode::HYBRID_SPLIT: {
        // | Collections | Library | Viewport | Adjustments |
        // | Metadata    |--------------------|  (Masking)  |
        // |             |      Filmstrip     |             |
        const ImGuiID right = splitWidth(center, ImGuiDir_Right, 340.0f, 0.17f, 0.28f);
        ImGuiID left = splitWidth(center, ImGuiDir_Left, 280.0f, 0.14f, 0.26f);
        const ImGuiID metadata = split(left, ImGuiDir_Down, 0.50f);
        const ImGuiID filmstrip = splitHeight(center, ImGuiDir_Down, 150.0f, 0.12f, 0.28f);
        const ImGuiID grid = split(center, ImGuiDir_Left, 0.42f);
        dock(PanelId::COLLECTIONS, left);
        dock(PanelId::SEARCH, left);
        dock(PanelId::METADATA, metadata);
        dock(PanelId::FILMSTRIP, filmstrip);
        dock(PanelId::ASSET_GRID, grid);
        dock(PanelId::ADJUSTMENTS, right);
        dock(PanelId::MASKING, right);
        dock(PanelId::VIEWPORT, center);
        break;
    }
    }
    ImGui::DockBuilderFinish(root);
    bringFrontTabs(workspace);
}

void WorkspaceLayoutManager::bringFrontTabs(Workspace& workspace) {
    // Tabbed docks open on these panels. Left alone, a dock node shows
    // whichever of its windows began last, even over a selection saved in
    // the .ini, so the main panel of each group is brought to the front once
    // per session (and after a reset); later tab choices are kept.
    switch (workspace.mode) {
    case AppMode::CATALOG: workspace.frontTabs = {PanelId::COLLECTIONS}; break;
    case AppMode::DEVELOP:
    case AppMode::HYBRID_SPLIT: workspace.frontTabs = {PanelId::COLLECTIONS, PanelId::ADJUSTMENTS}; break;
    case AppMode::CANVAS: workspace.frontTabs = {PanelId::PROPERTIES, PanelId::LAYERS}; break;
    }
    workspace.frontTabDelay = 2;  // windows dock on their first frame; focus them after that
}

std::optional<PanelId> WorkspaceLayoutManager::takeFrontTab(AppMode mode) noexcept {
    Workspace& workspace = workspaces_[index(mode)];
    if (workspace.frontTabs.empty()) return std::nullopt;
    if (workspace.frontTabDelay > 0) {
        --workspace.frontTabDelay;
        return std::nullopt;
    }
    const PanelId panel = workspace.frontTabs.front();
    workspace.frontTabs.erase(workspace.frontTabs.begin());
    workspace.frontTabDelay = 1;  // one focus change per frame: the tab bar follows the focused window
    return panel;
}

}  // namespace darkhouse::ui
