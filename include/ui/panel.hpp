// DarkHouse — dockable UI panels.
//
// A Panel draws the contents of one dockable window. The shell owns the
// window itself (Begin/End, open state, docking), so the same Panel object
// serves every workspace: each workspace docks its own window instance of it
// (see WorkspaceLayoutManager::windowName).
#pragma once

#include "gui_engine.hpp"
#include "ui/library_model.hpp"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace darkhouse::ui {

enum class PanelId : std::uint8_t {
    COLLECTIONS,  // left:   file / collection browser
    SEARCH,       // left:   search and filter
    METADATA,     // left:   metadata inspector
    ASSET_GRID,   // center: library thumbnail grid
    VIEWPORT,     // center: developed canvas
    FILMSTRIP,    // bottom: horizontal strip of the current collection
    LAYERS,       // right:  unified layer stack
    ADJUSTMENTS,  // right:  tone / HSL / colour adjustments
    ENGINE,       // floating diagnostics
    MASKING,      // right:  local-adjustment masks and masking tools
    PROPERTIES,   // right:  the selected layer's properties (Canvas & Compositing)
    TOOLS,        // left:   canvas tools and their options
    CHANNELS,     // right:  channel views of the canvas and the layer mask
    PATHS,        // right:  vector paths: pen, point editing, fill / stroke, path to mask
};
inline constexpr std::size_t kPanelCount = 14;

struct PanelInfo {
    PanelId id;
    const char* title;        // shown on the tab / title bar
    const char* key;          // stable ID fragment used in window names and settings
    const char* description;  // one line, shown in menus and tooltips
};

[[nodiscard]] const PanelInfo& panelInfo(PanelId id) noexcept;
[[nodiscard]] const std::array<PanelId, kPanelCount>& allPanels() noexcept;
[[nodiscard]] constexpr std::size_t index(PanelId id) noexcept { return static_cast<std::size_t>(id); }

struct MaskingState;  // ui/masking.hpp
struct CanvasState;   // ui/canvas_state.hpp

// What panels may ask of the shell, which owns dialogs. Handled after all
// panels have drawn.
struct ShellRequests {
    bool openImportDialog = false;
};

// Everything a panel may touch while drawing one frame.
struct PanelContext {
    DarkHouseApp& app;
    const FrameContext& frame;
    GuiEngine& gui;
    LibraryModel& library;  // collection, filter, sort and selection shared by the catalog panels
    ShellRequests& requests;
    MaskingState& masking;  // masks and masking tools, shared by the Masking panel and the viewport
    CanvasState& canvas;    // the selected layer, shared by the compositing panels
};

class Panel {
public:
    explicit Panel(PanelId id) noexcept : id_(id) {}
    virtual ~Panel() = default;

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    [[nodiscard]] PanelId id() const noexcept { return id_; }
    [[nodiscard]] const PanelInfo& info() const noexcept { return panelInfo(id_); }

    // Draws the window contents; the window is already begun and visible.
    virtual void draw(PanelContext& ctx) = 0;

    [[nodiscard]] virtual ImGuiWindowFlags windowFlags() const noexcept { return ImGuiWindowFlags_None; }
    // Edge-to-edge content (image views): the shell removes window padding.
    [[nodiscard]] virtual bool fullBleed() const noexcept { return false; }
    // Views that carry their own toolbar hide the dock tab bar while they are
    // alone in their dock node (it returns as soon as another panel joins).
    [[nodiscard]] virtual bool autoHideTabBar() const noexcept { return false; }

private:
    PanelId id_;
};

// GPU, swapchain and frame-timing diagnostics.
class EnginePanel final : public Panel {
public:
    EnginePanel() noexcept : Panel(PanelId::ENGINE) {}
    void draw(PanelContext& ctx) override;
};

}  // namespace darkhouse::ui
