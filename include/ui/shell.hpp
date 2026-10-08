// DarkHouse — application shell (the GuiLayer drawn by GuiEngine).
//
// Frame structure, top to bottom:
//   toolbar           File / View / Help, Import, the workspace switcher (Catalog,
//                     Develop, Canvas & Compositing, Split),
//                     import activity and the three panel toggles, on one row
//   workspace         WorkspaceLayoutManager dockspace for the active AppMode,
//                     with that workspace's open panels docked into it
//
// The active workspace always follows the engine's AppMode. The switcher and
// its shortcuts (Ctrl+1/2/3/4) post SwitchModeEvent, so a mode change requested
// by the UI, the command line or an OpenAssetEvent all take the same path.
#pragma once

#include "gui_engine.hpp"
#include "import_scan.hpp"
#include "ui/canvas_state.hpp"
#include "ui/library_model.hpp"
#include "ui/masking.hpp"
#include "ui/panel.hpp"
#include "ui/workspace_layout.hpp"

#include <array>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace darkhouse::ui {

class DarkHouseShell final : public GuiLayer {
public:
    DarkHouseShell();
    ~DarkHouseShell() override;

    void configureStyle(ImGuiStyle& style, ImGuiIO& io) override;
    void draw(DarkHouseApp& app, const FrameContext& frame, GuiEngine& gui) override;

private:
    void handleShortcuts(PanelContext& ctx);
    void drawToolbar(PanelContext& ctx);
    void drawMenus(PanelContext& ctx);
    void drawWorkspaceSwitcher(PanelContext& ctx);
    void drawActivityAndToggles(PanelContext& ctx);
    void drawPanels(PanelContext& ctx);
    void drawImportDialog(PanelContext& ctx);
    void drawAboutDialog(PanelContext& ctx);
    void requestMode(PanelContext& ctx, AppMode mode);

    // Import: paths are scanned on a worker thread, then posted as one ImportFilesEvent.
    void startImportScan(std::vector<std::string> paths);
    void pollImportScan(DarkHouseApp& app);

    WorkspaceLayoutManager layout_;
    LibraryModel library_;
    MaskingState masking_;  // shared by the Masking panel and the viewport's masking tools
    CanvasState canvas_;    // the selected layer, shared by the compositing panels
    std::array<std::unique_ptr<Panel>, kPanelCount> panels_;

    bool openImportDialog_ = false;
    bool openAboutDialog_ = false;
    bool showImGuiDemo_ = false;
    std::array<char, 1024> importPath_{};
    std::vector<std::future<ImportScan>> importScans_;  // one per Import / drop, in flight
    std::string importStatus_;
};

}  // namespace darkhouse::ui
