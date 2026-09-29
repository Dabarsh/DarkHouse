// DarkHouse — workspace layout manager.
//
// Each AppMode is a workspace with its own dockspace and its own window
// instance of every panel, so rearranging panels in one workspace never
// disturbs another and every arrangement persists in the layout .ini:
//
//   CATALOG       asset grid: browser / search / metadata | library grid
//   DEVELOP       parametric development (the Lightroom-style module):
//                 browser / metadata | viewport over filmstrip | adjustments + masking
//   CANVAS        compositing (the Photoshop-style module): tools | viewport |
//                 properties + masking over layers + channels + paths
//   HYBRID_SPLIT  library and develop side by side:
//                 browser / metadata | grid + viewport over filmstrip | adjustments + masking
//
// Only the active workspace's dockspace is shown. The others are submitted
// with ImGuiDockNodeFlags_KeepAliveOnly, so their docked windows stay docked
// while hidden. A default layout is built with the DockBuilder when a
// workspace without a saved layout is first shown, or when the user resets it.
#pragma once

#include "app_controller.hpp"
#include "ui/panel.hpp"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace darkhouse::ui {

inline constexpr std::size_t kWorkspaceCount = 4;
[[nodiscard]] constexpr std::size_t index(AppMode mode) noexcept { return static_cast<std::size_t>(mode); }
// Every workspace, in top-bar order.
[[nodiscard]] const std::array<AppMode, kWorkspaceCount>& workspaceModes() noexcept;
// The top-bar tab, e.g. "Develop (Lightroom)".
[[nodiscard]] const char* workspaceTitle(AppMode mode) noexcept;
// "Catalog", "Develop", "Canvas", "Split": status bar, menus, narrow windows and window IDs.
[[nodiscard]] const char* workspaceShortTitle(AppMode mode) noexcept;

class WorkspaceLayoutManager {
public:
    // Bump when a default layout changes, so saved layouts from older builds
    // are replaced instead of mixing old node trees with new panels.
    static constexpr int kLayoutVersion = 7;

    WorkspaceLayoutManager();

    // Hosts every workspace dockspace over the main viewport's work area (call
    // after menu and status bars, which shrink it). Builds missing layouts.
    void submit(AppMode active);

    // Rebuilds the workspace's default layout and panel set on the next submit().
    void resetLayout(AppMode mode) noexcept;

    // "Metadata###DarkHouse.Catalog.Metadata": same label everywhere, unique ID per workspace.
    [[nodiscard]] const char* windowName(AppMode mode, PanelId panel) const noexcept;
    [[nodiscard]] bool* panelOpen(AppMode mode, PanelId panel) noexcept;
    [[nodiscard]] ImGuiID dockspaceId(AppMode mode) const noexcept;

    // After the panels are drawn: remembers which panel each tabbed dock of
    // the active workspace shows, or, for a few frames after the workspace is
    // shown (again), selects the remembered ones. ImGui rebuilds a hidden
    // dock's tab bar when it reappears and would pick a tab of its own. A new
    // or reset layout starts from each workspace's main panels.
    void updateTabs(AppMode active);

private:
    struct Workspace {
        AppMode mode = AppMode::CATALOG;
        ImGuiID dockspace = 0;
        std::array<std::string, kPanelCount> windowNames;
        std::array<bool, kPanelCount> open{};
        bool resetPending = false;
        std::array<bool, kPanelCount> front{};  // the selected tab of its dock (see updateTabs)
        int restoreFrames = 0;
    };

    void buildDefaultLayout(Workspace& workspace, ImVec2 size);
    static void applyDefaultPanelSet(Workspace& workspace);
    static void defaultFrontTabs(Workspace& workspace);

    std::array<Workspace, kWorkspaceCount> workspaces_;
    std::optional<AppMode> lastActive_;
};

}  // namespace darkhouse::ui
