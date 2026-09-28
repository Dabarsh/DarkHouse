// DarkHouse — the dockable panels.
//
//   Left dock    CollectionsPanel, SearchPanel, MetadataPanel
//   Center dock  LibraryGridPanel, ViewportPanel, FilmstripPanel
//   Right dock   LayersPanel, AdjustmentsPanel
//
// Catalog panels share one LibraryModel (collection, filter, selection) via
// PanelContext; canvas panels work on DarkHouseApp's document and develop stack.
#pragma once

#include "ui/panel.hpp"

#include <glm/vec2.hpp>

#include <array>

namespace darkhouse::ui {

// File / collection browser: library collections, folder tree, smart collections.
class CollectionsPanel final : public Panel {
public:
    CollectionsPanel() noexcept : Panel(PanelId::COLLECTIONS) {}
    void draw(PanelContext& ctx) override;

private:
    void drawFolder(PanelContext& ctx, const FolderNode& folder, int depth);
};

// Search and filter UI over the selected collection.
class SearchPanel final : public Panel {
public:
    SearchPanel() noexcept : Panel(PanelId::SEARCH) {}
    void draw(PanelContext& ctx) override;

private:
    std::array<char, 256> text_{};
};

// Metadata inspector for the selected asset, with rating / flag / label editing.
class MetadataPanel final : public Panel {
public:
    MetadataPanel() noexcept : Panel(PanelId::METADATA) {}
    void draw(PanelContext& ctx) override;
};

// Library thumbnail grid of the visible assets.
class LibraryGridPanel final : public Panel {
public:
    LibraryGridPanel() noexcept : Panel(PanelId::ASSET_GRID) {}
    void draw(PanelContext& ctx) override;
    [[nodiscard]] ImGuiWindowFlags windowFlags() const noexcept override { return ImGuiWindowFlags_NoScrollbar; }
};

// The developed canvas (the develop graph's output texture), with zoom and pan.
class ViewportPanel final : public Panel {
public:
    ViewportPanel() noexcept : Panel(PanelId::VIEWPORT) {}
    void draw(PanelContext& ctx) override;
    [[nodiscard]] bool fullBleed() const noexcept override { return true; }
    [[nodiscard]] ImGuiWindowFlags windowFlags() const noexcept override {
        return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

private:
    float zoom_ = 0.0f;           // canvas pixels per screen pixel; 0 = fit to view
    glm::vec2 pan_{0.0f, 0.0f};   // offset of the image centre from the view centre, in screen pixels
};

// Horizontal strip of the visible assets.
class FilmstripPanel final : public Panel {
public:
    FilmstripPanel() noexcept : Panel(PanelId::FILMSTRIP) {}
    void draw(PanelContext& ctx) override;
    [[nodiscard]] ImGuiWindowFlags windowFlags() const noexcept override { return ImGuiWindowFlags_NoScrollbar; }
};

}  // namespace darkhouse::ui
