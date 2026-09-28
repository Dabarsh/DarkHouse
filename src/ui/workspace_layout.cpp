#include "ui/workspace_layout.hpp"

#include <imgui_internal.h>  // DockBuilder API, ImHashStr

#include <initializer_list>

namespace darkhouse::ui {
namespace {

constexpr std::array<AppMode, kWorkspaceCount> kModes{AppMode::CATALOG, AppMode::CANVAS, AppMode::HYBRID_SPLIT};

// Splits `node` and returns the new node on `dir`; `node` becomes the remainder.
ImGuiID split(ImGuiID& node, ImGuiDir dir, float ratio) {
    ImGuiID remainder = 0;
    const ImGuiID created = ImGui::DockBuilderSplitNode(node, dir, ratio, nullptr, &remainder);
    node = remainder;
    return created;
}

}  // namespace

const char* workspaceTitle(AppMode mode) noexcept {
    switch (mode) {
    case AppMode::CATALOG: return "Catalog";
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
            std::string("DarkHouse.") + workspaceTitle(mode) + ".v" + std::to_string(kLayoutVersion);
        workspace.dockspace = ImHashStr((prefix + ".DockSpace").c_str());
        for (PanelId panel : allPanels()) {
            const PanelInfo& info = panelInfo(panel);
            workspace.windowNames[index(panel)] = std::string(info.title) + "###" + prefix + "." + info.key;
        }
        applyDefaultPanelSet(workspace);
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
    case AppMode::CANVAS:
        show({PanelId::COLLECTIONS, PanelId::SEARCH, PanelId::METADATA, PanelId::VIEWPORT, PanelId::FILMSTRIP,
              PanelId::LAYERS, PanelId::ADJUSTMENTS});
        break;
    case AppMode::HYBRID_SPLIT:
        show({PanelId::COLLECTIONS, PanelId::SEARCH, PanelId::METADATA, PanelId::ASSET_GRID, PanelId::VIEWPORT,
              PanelId::FILMSTRIP, PanelId::LAYERS, PanelId::ADJUSTMENTS});
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

    // Build every missing layout up front: a keep-alive DockSpace() would
    // otherwise create an empty node and the default would never be applied.
    for (Workspace& workspace : workspaces_) {
        if (workspace.resetPending || ImGui::DockBuilderGetNode(workspace.dockspace) == nullptr) {
            if (workspace.resetPending) applyDefaultPanelSet(workspace);
            buildDefaultLayout(workspace, viewport->WorkSize);
            workspace.resetPending = false;
        }
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
        if (workspace.mode != active) {
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

    ImGuiID center = root;
    switch (workspace.mode) {
    case AppMode::CATALOG: {
        // | Search      |                 |
        // | Collections |  Library grid   |
        // | Metadata    |                 |
        ImGuiID left = split(center, ImGuiDir_Left, 0.21f);
        const ImGuiID metadata = split(left, ImGuiDir_Down, 0.42f);
        const ImGuiID search = split(left, ImGuiDir_Up, 0.52f);
        dock(PanelId::SEARCH, search);
        dock(PanelId::COLLECTIONS, left);
        dock(PanelId::METADATA, metadata);
        dock(PanelId::ASSET_GRID, center);
        break;
    }
    case AppMode::CANVAS: {
        // | Collections |  Viewport       | Layers      |
        // | Metadata    |-----------------|             |
        // |             |  Filmstrip      | Adjustments |
        ImGuiID right = split(center, ImGuiDir_Right, 0.24f);
        const ImGuiID adjustments = split(right, ImGuiDir_Down, 0.55f);
        ImGuiID left = split(center, ImGuiDir_Left, 0.19f);
        const ImGuiID metadata = split(left, ImGuiDir_Down, 0.50f);
        const ImGuiID filmstrip = split(center, ImGuiDir_Down, 0.20f);
        dock(PanelId::COLLECTIONS, left);
        dock(PanelId::SEARCH, left);
        dock(PanelId::METADATA, metadata);
        dock(PanelId::FILMSTRIP, filmstrip);
        dock(PanelId::LAYERS, right);
        dock(PanelId::ADJUSTMENTS, adjustments);
        dock(PanelId::VIEWPORT, center);
        break;
    }
    case AppMode::HYBRID_SPLIT: {
        // | Collections | Library | Viewport | Layers      |
        // | Metadata    |------------------- |             |
        // |             |      Filmstrip     | Adjustments |
        ImGuiID right = split(center, ImGuiDir_Right, 0.22f);
        const ImGuiID adjustments = split(right, ImGuiDir_Down, 0.55f);
        ImGuiID left = split(center, ImGuiDir_Left, 0.18f);
        const ImGuiID metadata = split(left, ImGuiDir_Down, 0.50f);
        const ImGuiID filmstrip = split(center, ImGuiDir_Down, 0.20f);
        const ImGuiID grid = split(center, ImGuiDir_Left, 0.42f);
        dock(PanelId::COLLECTIONS, left);
        dock(PanelId::SEARCH, left);
        dock(PanelId::METADATA, metadata);
        dock(PanelId::FILMSTRIP, filmstrip);
        dock(PanelId::ASSET_GRID, grid);
        dock(PanelId::LAYERS, right);
        dock(PanelId::ADJUSTMENTS, adjustments);
        dock(PanelId::VIEWPORT, center);
        break;
    }
    }
    ImGui::DockBuilderFinish(root);
}

}  // namespace darkhouse::ui
