// Center dock: library grid, viewport (develop output), filmstrip.

#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <glm/common.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace darkhouse::ui {
namespace {

constexpr float kCellSpacing = 6.0f;

// Right-click menu shared by grid and filmstrip cells.
void assetContextMenu(PanelContext& ctx, const AssetRecord& asset) {
    if (!ImGui::BeginPopupContextItem("##AssetMenu")) return;
    ImGui::TextDisabled("%s", asset.fileName.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Open in Canvas", "Enter")) ctx.app.postEvent(OpenAssetEvent{asset.id});
    if (ImGui::BeginMenu("Rating")) {
        for (int stars = 0; stars <= 5; ++stars) {
            const std::string label = stars == 0 ? std::string("None") : std::string(static_cast<std::size_t>(stars), '*');
            const std::string key = std::to_string(stars);
            if (ImGui::MenuItem(label.c_str(), key.c_str(), asset.rating == stars)) {
                ctx.app.postEvent(SetRatingEvent{asset.id, stars});
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Pick", "P", asset.flag == AssetFlag::PICKED)) ctx.app.postEvent(SetFlagEvent{asset.id, AssetFlag::PICKED});
    if (ImGui::MenuItem("Reject", "X", asset.flag == AssetFlag::REJECTED)) ctx.app.postEvent(SetFlagEvent{asset.id, AssetFlag::REJECTED});
    if (ImGui::MenuItem("Unflag", "U", asset.flag == AssetFlag::UNFLAGGED)) ctx.app.postEvent(SetFlagEvent{asset.id, AssetFlag::UNFLAGGED});
    if (ImGui::BeginMenu("Colour Label")) {
        for (int value = 0; value <= static_cast<int>(ColorLabel::PURPLE); ++value) {
            const auto label = static_cast<ColorLabel>(value);
            if (ImGui::MenuItem(theme::colorLabelName(label), nullptr, asset.colorLabel == label)) {
                ctx.app.postEvent(SetColorLabelEvent{asset.id, label});
            }
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(asset.filePath.c_str());
    ImGui::EndPopup();
}

void assetTooltip(const AssetRecord& asset) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) return;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(asset.fileName.c_str());
    const AssetMetadata& m = asset.metadata;
    if (m.cameraModel) ImGui::TextDisabled("%s", m.cameraModel->c_str());
    std::string exposure;
    if (m.shutterSpeed) exposure += formatShutter(*m.shutterSpeed) + "  ";
    if (m.aperture) {
        char aperture[16];
        std::snprintf(aperture, sizeof aperture, "f/%.1f  ", *m.aperture);
        exposure += aperture;
    }
    if (m.iso) exposure += "ISO " + std::to_string(*m.iso);
    if (!exposure.empty()) ImGui::TextDisabled("%s", exposure.c_str());
    if (asset.dateCaptured) ImGui::TextDisabled("%s", formatDateTime(*asset.dateCaptured).c_str());
    ImGui::EndTooltip();
}

// One interactive asset cell at the current cursor position.
void assetCell(PanelContext& ctx, std::size_t index, glm::vec2 size, bool caption) {
    const AssetRecord& asset = ctx.library.visible()[index];
    ImGui::PushID(static_cast<int>(index));
    ImGui::InvisibleButton("##cell", size);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ctx.library.select(index);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ctx.app.postEvent(OpenAssetEvent{asset.id});
    const bool selected = ctx.library.selectedIndex() == index;
    drawAssetCard(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), asset, selected,
                  hovered, caption);
    assetTooltip(asset);
    assetContextMenu(ctx, asset);
    ImGui::PopID();
}

void centeredText(const char* text) {
    const glm::vec2 available = ImGui::GetContentRegionAvail();
    const glm::vec2 size = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(glm::vec2(ImGui::GetCursorPos()) + glm::max((available - size) * 0.5f, glm::vec2(0.0f)));
    ImGui::TextDisabled("%s", text);
}

}  // namespace

// -----------------------------------------------------------------------------
// Library grid
// -----------------------------------------------------------------------------

void LibraryGridPanel::draw(PanelContext& ctx) {
    LibraryModel& library = ctx.library;
    const std::vector<AssetRecord>& assets = library.visible();

    // Toolbar: collection, count, thumbnail size.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(library.collectionTitle().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu photo%s", assets.size(), assets.size() == 1 ? "" : "s");
    const float sliderWidth = ImGui::GetFontSize() * 8.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - sliderWidth));
    ImGui::SetNextItemWidth(sliderWidth);
    ImGui::SliderFloat("##ThumbSize", &library.thumbnailSize, 72.0f, 280.0f, "");
    ImGui::SetItemTooltip("Thumbnail size");
    ImGui::Separator();

    if (!ImGui::BeginChild("##Grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs)) {
        ImGui::EndChild();
        return;
    }
    if (assets.empty()) {
        if (library.totalCount() == 0) {
            centeredText("No photos yet. Import some with File > Import Photos... or drop a folder here.");
        } else {
            centeredText("No photos match this collection and filter.");
            if (library.filter().active()) {
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 120.0f) * 0.5f);
                if (ImGui::Button("Clear filters", ImVec2(120.0f, 0.0f))) library.filter() = SearchFilter{};
            }
        }
        ImGui::EndChild();
        return;
    }

    // Columns fill the width; cells stretch to share the leftover space.
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((available + kCellSpacing) / (library.thumbnailSize + kCellSpacing)));
    const float cellWidth = std::floor((available - kCellSpacing * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    const glm::vec2 cell(cellWidth, cellWidth * 0.82f + ImGui::GetTextLineHeight());
    const float rowHeight = cell.y + kCellSpacing;
    const int rows = static_cast<int>((assets.size() + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));

    handleAssetKeys(ctx, columns);
    if (library.selectionChanged() && library.selectedIndex()) {
        const float top = static_cast<float>(*library.selectedIndex() / static_cast<std::size_t>(columns)) * rowHeight;
        const float scroll = ImGui::GetScrollY();
        const float view = ImGui::GetWindowHeight();
        if (top < scroll) ImGui::SetScrollY(top);
        if (top + cell.y > scroll + view) ImGui::SetScrollY(top + cell.y - view);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kCellSpacing, kCellSpacing));
    ImGuiListClipper clipper;
    clipper.Begin(rows, rowHeight);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int column = 0; column < columns; ++column) {
                const std::size_t index = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                                          static_cast<std::size_t>(column);
                if (index >= assets.size()) break;
                if (column > 0) ImGui::SameLine();
                assetCell(ctx, index, cell, /*caption=*/true);
            }
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// -----------------------------------------------------------------------------
// Viewport
// -----------------------------------------------------------------------------

void ViewportPanel::draw(PanelContext& ctx) {
    const glm::vec2 origin = ImGui::GetCursorScreenPos();
    const glm::vec2 region = glm::max(glm::vec2(ImGui::GetContentRegionAvail()), glm::vec2(1.0f));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, origin + region, IM_COL32(22, 22, 24, 255));

    const ImTextureID texture = ctx.gui.canvasTexture();
    if (texture == ImTextureID_Invalid) {
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::Dummy(region);
        const char* message = !ctx.app.canvasAvailable()
                                  ? "Canvas rendering is unavailable (GPU develop graph disabled; see the log)."
                                  : "The canvas renders in the Canvas and Split workspaces.";
        const glm::vec2 size = ImGui::CalcTextSize(message);
        drawList->AddText(origin + (region - size) * 0.5f, ImGui::GetColorU32(ImGuiCol_TextDisabled), message);
        return;
    }

    // Interaction surface covering the whole view.
    ImGui::InvisibleButton("##Canvas", region,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();

    const glm::vec2 imageSize = ctx.gui.canvasSize();
    const float fitScale = std::min(region.x / imageSize.x, region.y / imageSize.y) * 0.94f;
    float scale = zoom_ > 0.0f ? zoom_ : fitScale;
    const glm::vec2 viewCenter = origin + region * 0.5f;

    if (hovered && io.MouseWheel != 0.0f) {
        // Zoom around the cursor: the canvas point under it stays put.
        const float newScale = std::clamp(scale * std::pow(1.15f, io.MouseWheel), 0.02f, 32.0f);
        const glm::vec2 cursor = io.MousePos;
        const glm::vec2 center = viewCenter + pan_;
        const glm::vec2 canvasPoint = (cursor - center) / scale;
        pan_ = cursor - canvasPoint * newScale - viewCenter;
        scale = zoom_ = newScale;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        pan_ += glm::vec2(io.MouseDelta);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        zoom_ = zoom_ > 0.0f ? 0.0f : 1.0f;  // fit <-> 100 %
        scale = zoom_ > 0.0f ? zoom_ : fitScale;
        pan_ = glm::vec2(0.0f);
    }

    const glm::vec2 displaySize = imageSize * scale;
    const glm::vec2 imageMin = viewCenter + pan_ - displaySize * 0.5f;
    const glm::vec2 imageMax = imageMin + displaySize;

    drawList->PushClipRect(origin, origin + region, true);
    // Transparency checkerboard, only over the visible part of the canvas.
    const glm::vec2 visibleMin = glm::max(imageMin, origin);
    const glm::vec2 visibleMax = glm::min(imageMax, origin + region);
    constexpr float kChecker = 12.0f;
    if (visibleMin.x < visibleMax.x && visibleMin.y < visibleMax.y) {
        drawList->AddRectFilled(visibleMin, visibleMax, IM_COL32(58, 58, 60, 255));
        const glm::vec2 firstCell = glm::floor((visibleMin - imageMin) / kChecker);
        for (float y = firstCell.y; imageMin.y + y * kChecker < visibleMax.y; y += 1.0f) {
            for (float x = firstCell.x; imageMin.x + x * kChecker < visibleMax.x; x += 1.0f) {
                if ((static_cast<int>(x) + static_cast<int>(y)) % 2 != 0) continue;
                const glm::vec2 a = glm::max(imageMin + glm::vec2(x, y) * kChecker, visibleMin);
                const glm::vec2 b = glm::min(imageMin + glm::vec2(x + 1.0f, y + 1.0f) * kChecker, visibleMax);
                drawList->AddRectFilled(a, b, IM_COL32(44, 44, 46, 255));
            }
        }
    }
    drawList->AddImage(texture, imageMin, imageMax);
    drawList->AddRect(imageMin - glm::vec2(1.0f), imageMax + glm::vec2(1.0f), IM_COL32(0, 0, 0, 160));
    drawList->PopClipRect();

    // Toolbar overlay (top-left).
    ImGui::SetCursorScreenPos(origin + glm::vec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
    if (ImGui::SmallButton("Fit")) {
        zoom_ = 0.0f;
        pan_ = glm::vec2(0.0f);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("100%")) {
        zoom_ = 1.0f;
        pan_ = glm::vec2(0.0f);
    }
    ImGui::SameLine();
    ImGui::Text("%.0f%%", scale * 100.0f);
    ImGui::PopStyleVar();

    // Info overlay (bottom-left): document, develop stack, cursor position.
    std::string info = std::to_string(static_cast<int>(imageSize.x)) + " x " +
                       std::to_string(static_cast<int>(imageSize.y)) + "  RGBA16F linear";
    if (const AssetRecord* asset = ctx.library.findAsset(ctx.app.activeAssetId())) {
        info += "  |  develop stack of " + asset->fileName;
    }
    info += "  |  " + std::to_string(ctx.app.developStack().size()) + " develop node(s)";
    if (hovered) {
        const glm::vec2 pixel = glm::floor((glm::vec2(io.MousePos) - imageMin) / scale);
        if (pixel.x >= 0.0f && pixel.y >= 0.0f && pixel.x < imageSize.x && pixel.y < imageSize.y) {
            info += "  |  x " + std::to_string(static_cast<int>(pixel.x)) + "  y " + std::to_string(static_cast<int>(pixel.y));
        }
    }
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const glm::vec2 infoPos = origin + glm::vec2(8.0f, region.y - lineHeight - 4.0f);
    const glm::vec2 infoSize = ImGui::CalcTextSize(info.c_str());
    drawList->AddRectFilled(infoPos - glm::vec2(4.0f, 2.0f), infoPos + infoSize + glm::vec2(4.0f, 2.0f),
                            IM_COL32(0, 0, 0, 140), 3.0f);
    drawList->AddText(infoPos, ImGui::GetColorU32(ImGuiCol_Text, 0.85f), info.c_str());
}

// -----------------------------------------------------------------------------
// Filmstrip
// -----------------------------------------------------------------------------

void FilmstripPanel::draw(PanelContext& ctx) {
    LibraryModel& library = ctx.library;
    const std::vector<AssetRecord>& assets = library.visible();

    ImGui::TextUnformatted(library.collectionTitle().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu photo%s", assets.size(), assets.size() == 1 ? "" : "s");
    if (const AssetRecord* selected = library.selected()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|  %s", selected->fileName.c_str());
    }

    const float stripHeight = ImGui::GetContentRegionAvail().y;
    const float cellHeight = std::max(stripHeight - ImGui::GetStyle().ScrollbarSize - kCellSpacing, 24.0f);
    const glm::vec2 cell(std::floor(cellHeight * 1.3f), cellHeight);
    const float pitch = cell.x + kCellSpacing;
    ImGui::SetNextWindowContentSize(ImVec2(pitch * static_cast<float>(assets.size()), 0.0f));
    if (!ImGui::BeginChild("##Strip", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                           ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                               ImGuiWindowFlags_NoNavInputs)) {
        ImGui::EndChild();
        return;
    }
    if (assets.empty()) {
        centeredText("No photos in this collection.");
        ImGui::EndChild();
        return;
    }

    handleAssetKeys(ctx, 1);
    const float viewWidth = ImGui::GetWindowWidth();
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * pitch);  // wheel scrolls sideways
    }
    if (library.selectionChanged() && library.selectedIndex()) {
        const float left = static_cast<float>(*library.selectedIndex()) * pitch;
        if (left < ImGui::GetScrollX()) ImGui::SetScrollX(left);
        if (left + cell.x > ImGui::GetScrollX() + viewWidth) ImGui::SetScrollX(left + cell.x - viewWidth);
    }

    // Only the cells in view are submitted.
    const float scroll = ImGui::GetScrollX();
    const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor(scroll / pitch)));
    const auto last = std::min(assets.size(), static_cast<std::size_t>(std::ceil((scroll + viewWidth) / pitch)) + 1);
    const bool caption = cell.y > 96.0f;
    for (std::size_t index = first; index < last; ++index) {
        ImGui::SetCursorPos(ImVec2(static_cast<float>(index) * pitch, 0.0f));
        assetCell(ctx, index, cell, caption);
    }
    ImGui::EndChild();
}

}  // namespace darkhouse::ui
