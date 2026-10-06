// DarkHouse — workspace layout manager.
//
// Each AppMode is a workspace with its own dockspace and its own window
// instance of every panel, so rearranging panels in one workspace never
// disturbs another and every arrangement persists in the layout .ini:
//
//   CATALOG       asset grid focus: collections / metadata | library grid
//   CANVAS        layer stack focus: collections / metadata | viewport over filmstrip | layers / adjustments
//   HYBRID_SPLIT  dual view: collections / metadata | grid + viewport over filmstrip | layers / adjustments
//
// Only the active workspace's dockspace is shown. The others are submitted
// with ImGuiDockNodeFlags_KeepAliveOnly, so their docked windows stay docked
// while hidden. A default layout is built with the DockBuilder the first time
// a workspace has no saved layout, or when the user resets it.
#pragma once

#include "app_controller.hpp"
#include "ui/panel.hpp"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <string>

namespace darkhouse::ui {

inline constexpr std::size_t kWorkspaceCount = 3;
[[nodiscard]] constexpr std::size_t index(AppMode mode) noexcept { return static_cast<std::size_t>(mode); }
[[nodiscard]] const char* workspaceTitle(AppMode mode) noexcept;  // "Catalog", "Canvas", "Split"

class WorkspaceLayoutManager {
public:
    // Bump when a default layout changes, so saved layouts from older builds
    // are replaced instead of mixing old node trees with new panels.
    static constexpr int kLayoutVersion = 5;

    WorkspaceLayoutManager();

    // Hosts every workspace dockspace over the main viewport's work area (call
    // after menu and status bars, which shrink it). Builds missing layouts.
    void submit(AppMode active);

    // Rebuilds the workspace's default layout and panel set on the next submit().
    void resetLayout(AppMode mode) noexcept;

    // "Metadata###DarkHouse.Catalog.Metadata": same label everywhere, unique ID per workspace.
    [[nodiscard]] const char* windowName(AppMode mode, PanelId panel) const noexcept;
    [[nodiscard]] bool* panelOpen(AppMode mode, PanelId panel) noexcept;
    // Whether the panel is docked in the workspace's default layout. The
    // others (Filters, Engine) open as floating windows.
    [[nodiscard]] static bool inDefaultLayout(AppMode mode, PanelId panel) noexcept;
    [[nodiscard]] ImGuiID dockspaceId(AppMode mode) const noexcept;

private:
    struct Workspace {
        AppMode mode = AppMode::CATALOG;
        ImGuiID dockspace = 0;
        std::array<std::string, kPanelCount> windowNames;
        std::array<bool, kPanelCount> open{};
        bool resetPending = false;
    };

    void buildDefaultLayout(Workspace& workspace, ImVec2 size);
    static void applyDefaultPanelSet(Workspace& workspace);

    std::array<Workspace, kWorkspaceCount> workspaces_;
};

}  // namespace darkhouse::ui
