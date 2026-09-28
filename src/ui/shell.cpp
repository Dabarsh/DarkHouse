#include "ui/shell.hpp"

#include "ui/panels.hpp"
#include "ui/theme.hpp"

#include <imgui_internal.h>  // BeginViewportSideBar

#include <chrono>
#include <cstdio>
#include <iostream>
#include <utility>

#ifndef DARKHOUSE_VERSION
#define DARKHOUSE_VERSION "dev"
#endif

namespace darkhouse::ui {
namespace {

using theme::kAccent;
using theme::kAccentActive;
using theme::kAccentHovered;

const std::array<AppMode, kWorkspaceCount>& kModes = workspaceModes();

const char* workspaceHint(AppMode mode) {
    switch (mode) {
    case AppMode::CATALOG: return "Library grid: browse, rate, filter and import";
    case AppMode::DEVELOP:
        return "Parametric photo development: white balance, tone, colour mixer, colour grading, detail and local masks";
    case AppMode::CANVAS:
        return "Compositing: layer stack, blend modes and opacity, layer masks and the selected layer's properties";
    case AppMode::HYBRID_SPLIT: return "Library grid and the develop view side by side";
    }
    return "";
}

ImGuiKey workspaceKey(AppMode mode) {
    switch (mode) {
    case AppMode::CATALOG: return ImGuiKey_1;
    case AppMode::DEVELOP: return ImGuiKey_2;
    case AppMode::CANVAS: return ImGuiKey_3;
    case AppMode::HYBRID_SPLIT: return ImGuiKey_4;
    }
    return ImGuiKey_None;
}

const char* workspaceShortcut(AppMode mode) {
    switch (mode) {
    case AppMode::CATALOG: return "Ctrl+1";
    case AppMode::DEVELOP: return "Ctrl+2";
    case AppMode::CANVAS: return "Ctrl+3";
    case AppMode::HYBRID_SPLIT: return "Ctrl+4";
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
    case PanelId::MASKING: return std::make_unique<MaskingPanel>();
    case PanelId::PROPERTIES: return std::make_unique<PropertiesPanel>();
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
    PanelContext ctx{app, frame, gui, library_, requests, masking_, canvas_};

    handleShortcuts(ctx);
    // Bars shrink the main viewport's work area, so they come before the dockspace.
    drawMainMenuBar(ctx);
    drawStatusBar(ctx);
    layout_.submit(frame.mode);
    masking_.panelVisible = false;
    drawPanels(ctx);
    syncMaskOverlay(ctx);
    canvas_.pollSmartObjects(app.document(), app.canvasWidth(), app.canvasHeight());
    if (requests.openImportDialog) openImportDialog_ = true;

    drawImportDialog(ctx);
    drawAboutDialog(ctx);
    if (showImGuiDemo_) ImGui::ShowDemoWindow(&showImGuiDemo_);
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
// Main menu bar and workspace switcher
// -----------------------------------------------------------------------------

void DarkHouseShell::drawMainMenuBar(PanelContext& ctx) {
    if (!ImGui::BeginMainMenuBar()) return;

    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("DARKHOUSE");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Import Photos...", "Ctrl+I")) openImportDialog_ = true;
        ImGui::Separator();
        ImGui::BeginDisabled();
        const std::string catalog = "Catalog: " + ctx.app.config().catalogPath.filename().string();
        ImGui::MenuItem(catalog.c_str());
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", ctx.app.config().catalogPath.string().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) ctx.gui.requestClose();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        const AppMode active = ctx.frame.mode;
        ImGui::SeparatorText("Workspace");
        for (AppMode mode : kModes) {
            if (ImGui::MenuItem(workspaceTitle(mode), workspaceShortcut(mode), active == mode)) requestMode(ctx, mode);
        }
        ImGui::SeparatorText("Panels");
        for (PanelId id : allPanels()) {
            ImGui::MenuItem(panelInfo(id).title, nullptr, layout_.panelOpen(active, id));
            ImGui::SetItemTooltip("%s", panelInfo(id).description);
        }
        ImGui::Separator();
        const std::string reset = std::string("Reset ") + workspaceShortTitle(active) + " Layout";
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

    drawWorkspaceSwitcher(ctx);
    ImGui::EndMainMenuBar();
}

void DarkHouseShell::drawWorkspaceSwitcher(PanelContext& ctx) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float gap = 2.0f;
    auto switcherWidth = [&](const char* (*title)(AppMode)) {
        float width = 0.0f;
        for (AppMode mode : kModes) width += ImGui::CalcTextSize(title(mode)).x + style.FramePadding.x * 4.0f + gap;
        return width;
    };
    // Full tab titles when they fit beside the menus, short ones otherwise.
    const char* (*title)(AppMode) = workspaceTitle;
    float width = switcherWidth(title);
    if (ImGui::GetCursorPosX() + width + style.ItemSpacing.x * 4.0f > ImGui::GetWindowWidth()) {
        title = workspaceShortTitle;
        width = switcherWidth(title);
    }

    // Centred in the bar, unless the menus already reach past the centre.
    const float centred = (ImGui::GetWindowWidth() - width) * 0.5f;
    if (centred > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(centred);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, style.ItemSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 2.0f, style.FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    for (AppMode mode : kModes) {
        const bool active = ctx.frame.mode == mode;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? kAccent : style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? kAccentHovered : style.Colors[ImGuiCol_FrameBgHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
        ImGui::PushID(static_cast<int>(mode));
        if (ImGui::Button(title(mode))) requestMode(ctx, mode);
        ImGui::PopID();
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("%s  (%s)", workspaceHint(mode), workspaceShortcut(mode));
    }
    ImGui::PopStyleVar(3);
}

// -----------------------------------------------------------------------------
// Status bar and panels
// -----------------------------------------------------------------------------

void DarkHouseShell::drawStatusBar(PanelContext& ctx) {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##DarkHouseStatusBar", ImGui::GetMainViewport(), ImGuiDir_Down,
                                    ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::TextUnformatted(workspaceShortTitle(ctx.frame.mode));
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::TextUnformatted(ctx.app.config().catalogPath.filename().string().c_str());
            ImGui::Separator();
            const RunSummary& summary = ctx.app.summary();
            if (!importScans_.empty()) {
                ImGui::TextUnformatted("Scanning folders...");
            } else if (ctx.app.pendingImportCount() > 0) {
                ImGui::Text("Importing %zu file(s)...", ctx.app.pendingImportCount());
            } else if (!importStatus_.empty()) {
                ImGui::TextUnformatted(importStatus_.c_str());
            } else {
                ImGui::Text("%zu imported this session", summary.importsSucceeded);
            }
            if (summary.importsFailed > 0) {
                ImGui::Separator();
                ImGui::TextColored(kAccent, "%zu import(s) failed", summary.importsFailed);
            }

            // Right-aligned: GPU and frame rate.
            char right[160];
            const VulkanContext* gpu = ctx.gui.gpu();
            std::snprintf(right, sizeof right, "%s  |  %.0f fps", gpu ? gpu->deviceName().c_str() : "no GPU",
                          ImGui::GetIO().Framerate);
            const float rightWidth = ImGui::CalcTextSize(right).x + ImGui::GetStyle().ItemSpacing.x;
            if (ImGui::GetWindowWidth() - rightWidth > ImGui::GetCursorPosX()) {
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - rightWidth);
            }
            ImGui::TextDisabled("%s", right);
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void DarkHouseShell::drawPanels(PanelContext& ctx) {
    const AppMode mode = ctx.frame.mode;
    for (PanelId id : allPanels()) {
        bool* open = layout_.panelOpen(mode, id);
        if (!*open) continue;
        Panel& panel = *panels_[index(id)];
        // Panels outside the default layout open as floating windows of a sensible size.
        ImGui::SetNextWindowSize(ImVec2(420.0f, 320.0f), ImGuiCond_FirstUseEver);
        if (panel.autoHideTabBar()) {
            ImGuiWindowClass windowClass;
            windowClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
            ImGui::SetNextWindowClass(&windowClass);
        }
        if (panel.fullBleed()) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ImGui::Begin(layout_.windowName(mode, id), open, panel.windowFlags());
        if (panel.fullBleed()) ImGui::PopStyleVar();
        if (visible) panel.draw(ctx);
        ImGui::End();
    }
    // Focusing a docked window selects its tab.
    if (const std::optional<PanelId> front = layout_.takeFrontTab(mode)) ImGui::SetWindowFocus(layout_.windowName(mode, *front));
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
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Import Photos", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::TextWrapped(
        "Enter a file or folder. Folders are scanned recursively for RAW, JPEG, TIFF, PNG, HEIF, AVIF, WebP and "
        "EXR images. Originals are never modified. You can also drop files and folders onto the window.");
    ImGui::Spacing();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool submitted = ImGui::InputTextWithHint("##ImportPath", "/path/to/photos", importPath_.data(),
                                                    importPath_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();

    const bool hasPath = importPath_[0] != '\0';
    ImGui::BeginDisabled(!hasPath);
    if (ImGui::Button("Import", ImVec2(120.0f, 0.0f)) || (submitted && hasPath)) {
        startImportScan({std::string(importPath_.data())});
        importPath_[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DarkHouseShell::drawAboutDialog(PanelContext& ctx) {
    if (openAboutDialog_) {
        ImGui::OpenPopup("About DarkHouse");
        openAboutDialog_ = false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("About DarkHouse", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("DARKHOUSE " DARKHOUSE_VERSION);
    ImGui::PopStyleColor();
    ImGui::TextUnformatted("RAW development, asset management, compositing and vector design.");
    ImGui::Separator();
    if (const VulkanContext* gpu = ctx.gui.gpu()) ImGui::Text("GPU: %s (Vulkan 1.3)", gpu->deviceName().c_str());
    ImGui::Text("Dear ImGui %s (docking)", IMGUI_VERSION);
    ImGui::Text("Layout: %s", ctx.gui.iniPath().empty() ? "not saved" : ctx.gui.iniPath().string().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace darkhouse::ui
