// DarkHouse — the dockable panels.
//
//   Left dock    CollectionsPanel, SearchPanel, MetadataPanel
//   Center dock  LibraryGridPanel, ViewportPanel, FilmstripPanel
//   Right dock   LayersPanel, AdjustmentsPanel
//
// Catalog panels share one LibraryModel (collection, filter, selection) via
// PanelContext; canvas panels work on DarkHouseApp's document and develop stack.
#pragma once

#include "render_pipeline.hpp"
#include "ui/develop_sections.hpp"
#include "ui/panel.hpp"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <string>

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
    [[nodiscard]] bool autoHideTabBar() const noexcept override { return true; }
};

// The developed canvas (the develop graph's output texture), with zoom and pan.
class ViewportPanel final : public Panel {
public:
    ViewportPanel() noexcept : Panel(PanelId::VIEWPORT) {}
    void draw(PanelContext& ctx) override;
    [[nodiscard]] bool fullBleed() const noexcept override { return true; }
    [[nodiscard]] bool autoHideTabBar() const noexcept override { return true; }
    [[nodiscard]] ImGuiWindowFlags windowFlags() const noexcept override {
        return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

private:
    void drawPhotoStatus(PanelContext& ctx, glm::vec2 origin, glm::vec2 region);

    float zoom_ = 0.0f;           // canvas pixels per screen pixel; 0 = fit to view
    glm::vec2 pan_{0.0f, 0.0f};   // offset of the image centre from the view centre, in screen pixels
    std::string framedAssetId_;   // photo the zoom and pan belong to; a new photo opens fitted
};

// Horizontal strip of the visible assets.
class FilmstripPanel final : public Panel {
public:
    FilmstripPanel() noexcept : Panel(PanelId::FILMSTRIP) {}
    void draw(PanelContext& ctx) override;
    [[nodiscard]] ImGuiWindowFlags windowFlags() const noexcept override { return ImGuiWindowFlags_NoScrollbar; }
    [[nodiscard]] bool autoHideTabBar() const noexcept override { return true; }
};

// Unified layer stack: parametric, raster, vector, smart-object and group
// layers of the open document, with add / delete / reorder and properties.
class LayersPanel final : public Panel {
public:
    LayersPanel() noexcept : Panel(PanelId::LAYERS) {}
    void draw(PanelContext& ctx) override;

private:
    void drawLayerRow(PanelContext& ctx, LayerNode& layer, int depth);
    void drawAddMenu(PanelContext& ctx);
    void drawProperties(PanelContext& ctx, LayerNode& layer);
    void addLayer(PanelContext& ctx, std::unique_ptr<LayerNode> layer);
    [[nodiscard]] std::string nextName(const char* base);

    LayerNode* selected_ = nullptr;  // validated against the document every frame
    int nameCounter_ = 1;
};

// Develop adjustments: white balance, tone, presence, colour mixer, colour
// grading and noise reduction, all live on the GPU develop graph.
class AdjustmentsPanel final : public Panel {
public:
    AdjustmentsPanel() noexcept : Panel(PanelId::ADJUSTMENTS) {}
    void draw(PanelContext& ctx) override;

private:
    BasicSection basic_;
    ColorMixerSection mixer_;
    ColorGradingSection grading_;
    DetailSection detail_;
};

}  // namespace darkhouse::ui
