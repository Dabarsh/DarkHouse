// DarkHouse — the dockable panels.
//
//   Catalog      CollectionsPanel, SearchPanel, MetadataPanel, LibraryGridPanel, FilmstripPanel
//   Canvas view  ViewportPanel (every workspace but Catalog)
//   Develop      AdjustmentsPanel, MaskingPanel
//   Compositing  LayersPanel, PropertiesPanel
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
// layers of the open document, with the selected layer's blend mode and
// opacity, add / delete / reorder.
class LayersPanel final : public Panel {
public:
    LayersPanel() noexcept : Panel(PanelId::LAYERS) {}
    void draw(PanelContext& ctx) override;

private:
    void drawLayerRow(PanelContext& ctx, LayerNode& layer, int depth);
    void drawAddMenu(PanelContext& ctx);
    void addLayer(PanelContext& ctx, std::unique_ptr<LayerNode> layer);
    [[nodiscard]] std::string nextName(const char* base);

    int nameCounter_ = 1;
};

// The selected layer (CanvasState): name, layer mask (including one made
// from a develop mask) and its content's settings.
class PropertiesPanel final : public Panel {
public:
    PropertiesPanel() noexcept : Panel(PanelId::PROPERTIES) {}
    void draw(PanelContext& ctx) override;
};

// Develop adjustments: white balance, tone, presence, tone curve, colour
// mixer, colour grading and noise reduction, all live on the GPU develop graph.
class AdjustmentsPanel final : public Panel {
public:
    AdjustmentsPanel() noexcept : Panel(PanelId::ADJUSTMENTS) {}
    void draw(PanelContext& ctx) override;

private:
    BasicSection basic_;
    ToneCurveSection curve_;
    ColorMixerSection mixer_;
    ColorGradingSection grading_;
    DetailSection detail_;
};

// Local adjustments: the photo's mask stack (brush, gradients, ranges and the
// AI placeholders), each mask's components with add / subtract / intersect,
// invert and opacity, the brush options and the edits each mask applies.
class MaskingPanel final : public Panel {
public:
    MaskingPanel() noexcept : Panel(PanelId::MASKING) {}
    void draw(PanelContext& ctx) override;

private:
    int maskCounter_ = 0;
};

}  // namespace darkhouse::ui
