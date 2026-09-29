#include "ui/canvas_state.hpp"

#include "app_controller.hpp"

#include "adjustment_ops.hpp"
#include "image_decoder.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>

namespace darkhouse::ui {

const char* blendModeName(BlendMode mode) noexcept {
    switch (mode) {
    case BlendMode::NORMAL: return "Normal";
    case BlendMode::MULTIPLY: return "Multiply";
    case BlendMode::SCREEN: return "Screen";
    case BlendMode::OVERLAY: return "Overlay";
    case BlendMode::COLOR_DODGE: return "Color Dodge";
    case BlendMode::DARKEN: return "Darken";
    case BlendMode::LIGHTEN: return "Lighten";
    case BlendMode::COLOR_BURN: return "Color Burn";
    case BlendMode::HARD_LIGHT: return "Hard Light";
    case BlendMode::SOFT_LIGHT: return "Soft Light";
    case BlendMode::DIFFERENCE: return "Difference";
    case BlendMode::EXCLUSION: return "Exclusion";
    case BlendMode::HUE: return "Hue";
    case BlendMode::SATURATION: return "Saturation";
    case BlendMode::COLOR: return "Color";
    case BlendMode::LUMINOSITY: return "Luminosity";
    }
    return "?";
}

bool endsBlendGroup(BlendMode mode) noexcept {
    return mode == BlendMode::NORMAL || mode == BlendMode::COLOR_BURN || mode == BlendMode::COLOR_DODGE ||
           mode == BlendMode::HARD_LIGHT || mode == BlendMode::EXCLUSION;
}

bool blendModeCombo(const char* id, BlendMode& mode) {
    bool changed = false;
    if (ImGui::BeginCombo(id, blendModeName(mode))) {
        for (BlendMode candidate : kBlendModes) {
            if (ImGui::Selectable(blendModeName(candidate), candidate == mode) && candidate != mode) {
                mode = candidate;
                changed = true;
            }
            if (endsBlendGroup(candidate)) ImGui::Separator();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void invalidateComposite(LayerNode& root) { root.markCompositeDirty(); }

bool containsLayer(const LayerNode& root, const LayerNode* target) {
    bool found = false;
    root.visit([&](const LayerNode& node, std::size_t) { found = found || &node == target; });
    return found;
}

std::vector<float> compositeCanvas(const LayerNode& root, std::uint32_t width, std::uint32_t height) {
    std::vector<float> rgba(std::size_t{width} * height * 4, 0.0f);
    const std::uint32_t tilesX = (width + TILE_SIZE - 1) / TILE_SIZE;
    const std::uint32_t tilesY = (height + TILE_SIZE - 1) / TILE_SIZE;
    for (std::uint32_t ty = 0; ty < tilesY; ++ty) {
        for (std::uint32_t tx = 0; tx < tilesX; ++tx) {
            const CompositedTile tile = compositeTileCPU(root, {tx, ty}, width, height);
            for (std::uint32_t row = 0; row < tile.height; ++row) {
                const std::size_t dst = (std::size_t{ty * TILE_SIZE + row} * width + tx * TILE_SIZE) * 4;
                std::copy_n(tile.rgba.begin() + static_cast<std::ptrdiff_t>(std::size_t{row} * tile.width * 4),
                            std::size_t{tile.width} * 4, rgba.begin() + static_cast<std::ptrdiff_t>(dst));
            }
        }
    }
    return rgba;
}

void applyMaskToLayer(const LocalAdjustment& mask, LayerNode& layer, LayerNode& root, std::uint32_t canvasWidth,
                      std::uint32_t canvasHeight) {
    SparseRasterLayer* target = layer.mask();
    if (!target || target->width() != canvasWidth || target->height() != canvasHeight) {
        target = &layer.addMask(canvasWidth, canvasHeight);
    }
    // Ranges are measured on the composite as it is now (before develop).
    const std::vector<float> source =
        readsImage(mask) ? compositeCanvas(root, canvasWidth, canvasHeight) : std::vector<float>{};
    writeLayerMask(mask, *target, source);
    layer.setMaskEnabled(true);
    invalidateComposite(root);
}

void drawLayerOutline(const LayerNode& layer, ImDrawList* drawList, ImVec2 imageMin, float scale) {
    const std::optional<std::array<float, 4>> bounds = layer.contentBounds();
    if (!bounds) return;
    const Affine m = layer.transform().matrix();
    const std::array<std::array<float, 2>, 4> corners{{{(*bounds)[0], (*bounds)[1]}, {(*bounds)[2], (*bounds)[1]},
                                                       {(*bounds)[2], (*bounds)[3]}, {(*bounds)[0], (*bounds)[3]}}};
    ImVec2 points[4];
    for (std::size_t i = 0; i < 4; ++i) {
        const std::array<float, 2> p = applyAffine(m, corners[i][0], corners[i][1]);
        points[i] = ImVec2(imageMin.x + p[0] * scale, imageMin.y + p[1] * scale);
    }
    drawList->AddPolyline(points, 4, IM_COL32(0, 0, 0, 150), 3.0f, ImDrawFlags_Closed);
    drawList->AddPolyline(points, 4, IM_COL32(90, 170, 255, 230), 1.0f, ImDrawFlags_Closed);
    for (const ImVec2& p : points) {
        drawList->AddRectFilled(ImVec2(p.x - 3.5f, p.y - 3.5f), ImVec2(p.x + 3.5f, p.y + 3.5f), IM_COL32(255, 255, 255, 230));
        drawList->AddRect(ImVec2(p.x - 3.5f, p.y - 3.5f), ImVec2(p.x + 3.5f, p.y + 3.5f), IM_COL32(90, 170, 255, 255));
    }
}

void CanvasState::validate(const LayerNode& root) {
    if (selectedLayer && !containsLayer(root, selectedLayer)) selectedLayer = nullptr;
    if (maskChannel && !containsLayer(root, maskChannel)) maskChannel = nullptr;
}

void CanvasState::syncView(PanelContext& ctx) {
    LayerNode& root = ctx.app.document();
    validate(root);
    const bool canvas = ctx.frame.mode == AppMode::CANVAS;
    if (maskChannel && !maskChannel->mask()) maskChannel = nullptr;
    const DisplayChannel wanted = canvas && !maskChannel ? channel : DisplayChannel::COLOR;
    if (wanted != sentChannel_) {
        ctx.app.postEvent(SetDisplayChannelEvent{wanted});
        sentChannel_ = wanted;
    }
    root.setMaskPreview(canvas ? maskChannel : nullptr);
    if (!canvas) {
        tools.dragging = false;
        tools.penPoints.clear();
    }
}

LayerNode& CanvasState::insertLayer(LayerNode& root, std::unique_ptr<LayerNode> layer) {
    validate(root);
    LayerNode* parent = &root;
    std::size_t position = root.childCount();
    if (selectedLayer && selectedLayer != &root) {
        if (selectedLayer->isGroup()) {
            parent = selectedLayer;
            position = selectedLayer->childCount();
        } else {
            parent = selectedLayer->parent();
            for (std::size_t i = 0; i < parent->childCount(); ++i) {
                if (&parent->child(i) == selectedLayer) position = i + 1;
            }
        }
    }
    LayerNode& added = parent->insertChild(position, std::move(layer));
    selectedLayer = &added;
    return added;
}

void CanvasState::loadSmartObject(LayerNode& layer, const std::filesystem::path& file, std::vector<EditNodeRecord> editStack,
                                  std::uint32_t maxDimension) {
    auto decode = [file, stack = std::move(editStack), maxDimension]() -> std::shared_ptr<const SparseRasterLayer> {
        DecodedImage image = decodeImage(file, DecodeOptions{maxDimension});
        // The source's global point adjustments; denoise, lens corrections and
        // local masks need the GPU develop graph and are not applied here.
        for (const EditNodeRecord& record : stack) {
            const std::optional<PointAdjustment> op = PointAdjustment::create(record.nodeType, record.serializedParams);
            if (!op || op->identity()) continue;
            for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
                const Rgb out = op->apply({image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]});
                image.rgba[i] = out[0];
                image.rgba[i + 1] = out[1];
                image.rgba[i + 2] = out[2];
            }
        }
        auto pixels = std::make_shared<SparseRasterLayer>(image.width, image.height, 4, 0.0f);
        pixels->writeRegion(0, 0, image.width, image.height, image.rgba);
        return pixels;
    };
    pendingSmartObjects_.push_back({&layer, std::async(std::launch::async, std::move(decode))});
}

void CanvasState::pollSmartObjects(LayerNode& root, std::uint32_t canvasWidth, std::uint32_t canvasHeight) {
    for (auto it = pendingSmartObjects_.begin(); it != pendingSmartObjects_.end();) {
        if (it->pixels.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        std::shared_ptr<const SparseRasterLayer> pixels;
        try {
            pixels = it->pixels.get();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[DarkHouse] warn: smart object not loaded: %s\n", e.what());
        }
        LayerNode* layer = it->layer;
        it = pendingSmartObjects_.erase(it);
        if (!pixels || !containsLayer(root, layer) || !layer->smartObject()) continue;  // deleted meanwhile
        layer->smartObject()->pixels = pixels;
        // Centred, scaled down to fit half the canvas.
        const float w = static_cast<float>(pixels->width()), h = static_cast<float>(pixels->height());
        const float fit = std::min({0.5f * static_cast<float>(canvasWidth) / w, 0.5f * static_cast<float>(canvasHeight) / h, 1.0f});
        LayerTransform placement;
        placement.pivotX = w * 0.5f;
        placement.pivotY = h * 0.5f;
        placement.scaleX = placement.scaleY = fit;
        placement.translateX = static_cast<float>(canvasWidth) * 0.5f - placement.pivotX;
        placement.translateY = static_cast<float>(canvasHeight) * 0.5f - placement.pivotY;
        layer->setTransform(placement);
        layer->markCompositeDirty();
    }
}

bool CanvasState::loadingSmartObject(const LayerNode* layer) const noexcept {
    return std::any_of(pendingSmartObjects_.begin(), pendingSmartObjects_.end(),
                       [&](const PendingSmartObject& pending) { return pending.layer == layer; });
}

}  // namespace darkhouse::ui
