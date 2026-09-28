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

}  // namespace darkhouse::ui
