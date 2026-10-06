#include "ui/shell.hpp"

#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <utility>

#ifndef DARKHOUSE_VERSION
#define DARKHOUSE_VERSION "dev"
#endif

namespace darkhouse::ui {
namespace {

constexpr std::array<AppMode, kWorkspaceCount> kModes{AppMode::CATALOG, AppMode::CANVAS, AppMode::HYBRID_SPLIT};

ImGuiKey workspaceKey(AppMode mode) {
    switch (mode) {
    case AppMode::CATALOG: return ImGuiKey_1;
    case AppMode::CANVAS: return ImGuiKey_2;
    case AppMode::HYBRID_SPLIT: return ImGuiKey_3;
    }
    return ImGuiKey_None;
}

const char* workspaceDigit(AppMode mode) {
    switch (mode) {
    case AppMode::CATALOG: return "1";
    case AppMode::CANVAS: return "2";
    case AppMode::HYBRID_SPLIT: return "3";
    }
    return "";
}

std::unique_ptr<Panel> makePanel(PanelId id) {
    switch (id) {
    case PanelId::COLLECTIONS: return std::make_unique<CollectionsPanel>();
    case PanelId::SEARCH: return std::make_unique<SearchPanel>();
    case PanelId::METADATA: return std::make_unique<MetadataPanel>();
    case PanelId::ASSET_GRID: return std::make_unique<LibraryGridPanel>();
    case PanelId::VIEWPORT: return std::make_unique<ViewportPanel>();
    case PanelId::FILMSTRIP: return std::make_unique<FilmstripPanel>();
    case PanelId::LAYERS: return std::make_unique<LayersPanel>();
    case PanelId::ADJUSTMENTS: return std::make_unique<AdjustmentsPanel>();
    case PanelId::ENGINE: return std::make_unique<EnginePanel>();
    }
    return nullptr;
}

}  // namespace

DarkHouseShell::DarkHouseShell() {
    for (PanelId id : allPanels()) panels_[index(id)] = makePanel(id);
}

// A future from std::async joins its task on destruction, so a folder scan
// still running at exit finishes before the shell goes away.
DarkHouseShell::~DarkHouseShell() = default;

void DarkHouseShell::configureStyle(ImGuiStyle& style, ImGuiIO& io) {
    theme::applyStyle(style);
    theme::loadFonts(io, style);
}

void DarkHouseShell::draw(DarkHouseApp& app, const FrameContext& frame, GuiEngine& gui) {
    std::vector<std::string> dropped = gui.window().takeDroppedPaths();
    if (!dropped.empty()) startImportScan(std::move(dropped));
    pollImportScan(app);
    library_.update(app);
    ShellRequests requests;
    PanelContext ctx{app, frame, gui, library_, requests};

    handleShortcuts(ctx);
    // The toolbar shrinks the main viewport's work area, so it comes before the dockspace.
    drawToolbar(ctx);
    layout_.submit(frame.mode);
    drawPanels(ctx);
    if (requests.openImportDialog) openImportDialog_ = true;
    if (requests.toggleSearchPanel) {
        bool* open = layout_.panelOpen(frame.mode, PanelId::SEARCH);
        *open = !*open;
    }

    drawImportDialog(ctx);
    drawAboutDialog(ctx);
    if (showImGuiDemo_) ImGui::ShowDemoWindow(&showImGuiDemo_);
    drawPopupShadows();
}

void DarkHouseShell::requestMode(PanelContext& ctx, AppMode mode) {
    if (mode != ctx.frame.mode) ctx.app.postEvent(SwitchModeEvent{mode});
}

void DarkHouseShell::handleShortcuts(PanelContext& ctx) {
    constexpr ImGuiInputFlags kGlobal = ImGuiInputFlags_RouteGlobal;
    for (AppMode mode : kModes) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | workspaceKey(mode), kGlobal)) requestMode(ctx, mode);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_I, kGlobal)) openImportDialog_ = true;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, kGlobal)) ctx.gui.requestClose();
}

// -----------------------------------------------------------------------------
// Toolbar
// -----------------------------------------------------------------------------

void DarkHouseShell::drawToolbar(PanelContext& ctx) {
    const ImGuiStyle& style = ImGui::GetStyle();
    // One row for menus and controls. It is taller than a control, so every
    // item gets a toolbar-high target and draws itself centred in it.
    const float padding = (theme::kToolbarHeight * theme::scale() - ImGui::GetFontSize()) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, std::max(padding, style.FramePadding.y)));
    if (ImGui::BeginMainMenuBar()) {
        drawMenus(ctx);
        // The catalog's primary action; filled with the accent while the catalog is empty.
        if (labelButton("##Import", Icon::IMPORT, "Import", ctx.library.totalCount() == 0)) openImportDialog_ = true;
        ImGui::SetItemTooltip("Import photos or folders  (%s)", shortcutLabel("I").c_str());
        drawWorkspaceSwitcher(ctx);
        drawActivityAndToggles(ctx);
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
}

void DarkHouseShell::drawMenus(PanelContext& ctx) {
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Import Photos...", shortcutLabel("I").c_str())) openImportDialog_ = true;
        ImGui::Separator();
        ImGui::BeginDisabled();
        const std::string catalog = "Catalog: " + ctx.app.config().catalogPath.filename().string();
        ImGui::MenuItem(catalog.c_str());
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", ctx.app.config().catalogPath.string().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", shortcutLabel("Q").c_str())) ctx.gui.requestClose();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        const AppMode active = ctx.frame.mode;
        ImGui::SeparatorText("Workspace");
        for (AppMode mode : kModes) {
            if (ImGui::MenuItem(workspaceTitle(mode), shortcutLabel(workspaceDigit(mode)).c_str(), active == mode)) {
                requestMode(ctx, mode);
            }
        }
        ImGui::SeparatorText("Panels");
        for (PanelId id : allPanels()) {
            ImGui::MenuItem(panelInfo(id).title, nullptr, layout_.panelOpen(active, id));
            ImGui::SetItemTooltip("%s", panelInfo(id).description);
        }
        ImGui::Separator();
        const std::string reset = std::string("Reset ") + workspaceTitle(active) + " Layout";
        if (ImGui::MenuItem(reset.c_str())) layout_.resetLayout(active);
        bool vsync = ctx.gui.vsync();
        if (ImGui::MenuItem("Vertical Sync", nullptr, &vsync)) ctx.gui.setVsync(vsync);
        bool lowPower = ctx.gui.lowPowerIdle();
        if (ImGui::MenuItem("Low-Power Idle", nullptr, &lowPower)) ctx.gui.setLowPowerIdle(lowPower);
        ImGui::SetItemTooltip("Wait for input instead of redrawing every frame while nothing changes");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About DarkHouse")) openAboutDialog_ = true;
#ifndef NDEBUG
        ImGui::MenuItem("Dear ImGui Demo", nullptr, &showImGuiDemo_);
#endif
        ImGui::EndMenu();
    }
}

void DarkHouseShell::drawWorkspaceSwitcher(PanelContext& ctx) {
    const float segment = 76.0f * theme::scale();
    // Centred in the bar, unless the menus already reach past the centre.
    const float centred = (ImGui::GetWindowWidth() - segment * static_cast<float>(kModes.size())) * 0.5f;
    if (centred > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(centred);

    int active = static_cast<int>(index(ctx.frame.mode));
    if (segmented("##Workspace", active,
                  {workspaceTitle(kModes[0]), workspaceTitle(kModes[1]), workspaceTitle(kModes[2])}, segment,
                  /*prominent=*/true)) {
        requestMode(ctx, kModes[static_cast<std::size_t>(active)]);
    }
    ImGui::SetItemTooltip("Catalog: browse, rate, filter and import\nCanvas: develop, composite and design\n"
                          "Split: library grid and canvas side by side\n%s / 2 / 3",
                          shortcutLabel("1").c_str());
}

// Right end of the toolbar: what the importer is doing, then one toggle per
// side of the workspace.
void DarkHouseShell::drawActivityAndToggles(PanelContext& ctx) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();
    const float height = ImGui::GetFrameHeight();
    const AppMode mode = ctx.frame.mode;
    const RunSummary& summary = ctx.app.summary();

    char activity[160] = "";
    bool busy = false;
    if (!importScans_.empty()) {
        std::snprintf(activity, sizeof activity, "Scanning folders...");
        busy = true;
    } else if (ctx.app.pendingImportCount() > 0) {
        std::snprintf(activity, sizeof activity, "Importing %zu file%s...", ctx.app.pendingImportCount(),
                      ctx.app.pendingImportCount() == 1 ? "" : "s");
        busy = true;
    } else if (!importStatus_.empty()) {
        std::snprintf(activity, sizeof activity, "%s", importStatus_.c_str());
    } else if (summary.importsSucceeded > 0) {
        std::snprintf(activity, sizeof activity, "%zu imported this session", summary.importsSucceeded);
    }
    char failed[48] = "";
    if (summary.importsFailed > 0) std::snprintf(failed, sizeof failed, "%zu failed", summary.importsFailed);

    const float gap = style.ItemSpacing.x;
    const float spinner = busy ? height * 0.5f + gap : 0.0f;
    float activityWidth = 0.0f;
    {
        const SmallText small;
        activityWidth = activity[0] ? ImGui::CalcTextSize(activity).x + gap : 0.0f;
    }
    const float failedWidth = failed[0] ? pillWidth(failed) + gap : 0.0f;
    const float togglesWidth = 3.0f * iconButtonWidth() + 2.0f * 2.0f * s;
    const float right = ImGui::GetWindowWidth() - style.WindowPadding.x;

    // The read-outs give way first when the window is narrow; the toggles stay.
    float x = right - togglesWidth - failedWidth - activityWidth - spinner;
    const bool showActivity = x > ImGui::GetCursorPosX() + gap;
    if (!showActivity) x = std::max(right - togglesWidth, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(x);

    if (showActivity && busy) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float radius = height * 0.22f;
        ImGui::Dummy(ImVec2(spinner - gap, height));
        const float angle = static_cast<float>(ImGui::GetTime()) * 5.0f;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PathArcTo(ImVec2(pos.x + (spinner - gap) * 0.5f, pos.y + height * 0.5f), radius, angle, angle + 4.4f, 20);
        drawList->PathStroke(theme::u32(theme::kAccentBright), 1.6f * s);
    }
    if (showActivity && activity[0]) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const SmallText small;
        const ImVec2 size = ImGui::CalcTextSize(activity);
        ImGui::Dummy(ImVec2(size.x, height));
        ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, pos.y + (height - size.y) * 0.5f),
                                            theme::u32(theme::kTextSecondary), activity);
    }
    if (showActivity && failed[0]) {
        pill(failed, theme::kDanger);
        ImGui::SetItemTooltip("Imports that could not be read; see the log for each file");
    }

    const auto toggle = [&](const char* id, Icon icon, const char* tooltip, std::initializer_list<PanelId> panels) {
        bool available = true;
        bool anyOpen = false;
        for (PanelId panel : panels) {
            available = available && WorkspaceLayoutManager::inDefaultLayout(mode, panel);
            anyOpen = anyOpen || *layout_.panelOpen(mode, panel);
        }
        if (iconButton(id, icon, tooltip, available && anyOpen, available)) {
            for (PanelId panel : panels) *layout_.panelOpen(mode, panel) = !anyOpen;
        }
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * s, style.ItemSpacing.y));
    toggle("##LeftPanels", Icon::SIDEBAR_LEFT, "Collections and Metadata", {PanelId::COLLECTIONS, PanelId::METADATA});
    toggle("##BottomPanels", Icon::PANEL_BOTTOM, "Filmstrip", {PanelId::FILMSTRIP});
    toggle("##RightPanels", Icon::SIDEBAR_RIGHT, "Layers and Adjustments", {PanelId::LAYERS, PanelId::ADJUSTMENTS});
    ImGui::PopStyleVar();
}

// -----------------------------------------------------------------------------
// Panels
// -----------------------------------------------------------------------------

void DarkHouseShell::drawPanels(PanelContext& ctx) {
    const AppMode mode = ctx.frame.mode;
    for (PanelId id : allPanels()) {
        bool* open = layout_.panelOpen(mode, id);
        if (!*open) continue;
        Panel& panel = *panels_[index(id)];
        // Panels outside the default layout open as floating windows of a
        // sensible size, in the middle of the workspace.
        if (!WorkspaceLayoutManager::inDefaultLayout(mode, id)) {
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(420.0f, 320.0f) * theme::scale(), ImGuiCond_FirstUseEver);
        }
        if (panel.autoHideTabBar()) {
            ImGuiWindowClass windowClass;
            windowClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
            ImGui::SetNextWindowClass(&windowClass);
        }
        if (panel.canvasBackground()) ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kCanvas);
        if (panel.fullBleed()) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ImGui::Begin(layout_.windowName(mode, id), open, panel.windowFlags());
        if (panel.fullBleed()) ImGui::PopStyleVar();
        if (visible) panel.draw(ctx);
        ImGui::End();
        if (panel.canvasBackground()) ImGui::PopStyleColor();
    }
}

// -----------------------------------------------------------------------------
// Import
// -----------------------------------------------------------------------------

void DarkHouseShell::startImportScan(std::vector<std::string> paths) {
    importStatus_.clear();
    importScans_.push_back(std::async(std::launch::async, [paths = std::move(paths)] { return scanImportPaths(paths); }));
}

void DarkHouseShell::pollImportScan(DarkHouseApp& app) {
    for (auto it = importScans_.begin(); it != importScans_.end();) {
        if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        ImportScan scan = it->get();
        it = importScans_.erase(it);
        for (const std::string& warning : scan.warnings) std::clog << "[DarkHouse] warn: import: " << warning << '\n';
        if (scan.files.empty()) {
            importStatus_ = "No supported images found";
            continue;
        }
        importStatus_ = "Queued " + std::to_string(scan.files.size()) + " file(s) for import";
        app.postEvent(ImportFilesEvent{std::move(scan.files)});
    }
}

void DarkHouseShell::drawImportDialog(PanelContext&) {
    if (openImportDialog_) {
        ImGui::OpenPopup("Import Photos");
        openImportDialog_ = false;
    }
    const float s = theme::scale();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * s, 0.0f), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f) * s);
    const bool open = ImGui::BeginPopupModal("Import Photos", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!open) return;

    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);
    ImGui::TextWrapped(
        "Enter a file or folder. Folders are scanned recursively for RAW, JPEG, TIFF, PNG, HEIF, AVIF, WebP and "
        "EXR images. Originals are never modified. You can also drop files and folders onto the window.");
    ImGui::PopStyleColor();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool submitted = ImGui::InputTextWithHint("##ImportPath", "/path/to/photos", importPath_.data(),
                                                    importPath_.size(), ImGuiInputTextFlags_EnterReturnsTrue);

    // Default action on the right, Cancel beside it.
    const ImVec2 button(104.0f * s, 0.0f);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    alignRight(button.x * 2.0f + ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    const bool hasPath = importPath_[0] != '\0';
    ImGui::BeginDisabled(!hasPath);
    if (primaryButton("Import", button) || (submitted && hasPath)) {
        startImportScan({std::string(importPath_.data())});
        importPath_[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void DarkHouseShell::drawAboutDialog(PanelContext& ctx) {
    if (openAboutDialog_) {
        ImGui::OpenPopup("About DarkHouse");
        openAboutDialog_ = false;
    }
    const float s = theme::scale();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f) * s);
    const bool open = ImGui::BeginPopupModal("About DarkHouse", nullptr,
                                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!open) return;

    ImGui::TextUnformatted("DarkHouse " DARKHOUSE_VERSION);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);
    ImGui::TextUnformatted("RAW development, asset management, compositing and vector design.");
    ImGui::Separator();
    if (const VulkanContext* gpu = ctx.gui.gpu()) ImGui::Text("GPU: %s (Vulkan 1.3)", gpu->deviceName().c_str());
    ImGui::Text("Dear ImGui %s (docking)", IMGUI_VERSION);
    ImGui::Text("Layout: %s", ctx.gui.iniPath().empty() ? "not saved" : ctx.gui.iniPath().string().c_str());
    ImGui::PopStyleColor();
    const ImVec2 button(104.0f * s, 0.0f);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    alignRight(button.x);
    if (primaryButton("Close", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace darkhouse::ui
