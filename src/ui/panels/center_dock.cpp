// Center dock: library grid, viewport (develop output), filmstrip.

#include "ui/canvas_state.hpp"
#include "ui/canvas_tools.hpp"
#include "ui/masking.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <glm/common.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>

namespace darkhouse::ui {
namespace {

using theme::u32;

constexpr float kCellSpacing = 6.0f;

// Right-click menu shared by grid and filmstrip cells.
void assetContextMenu(PanelContext& ctx, const AssetRecord& asset) {
    if (!ImGui::BeginPopupContextItem("##AssetMenu")) return;
    {
        const SmallText small;
        ImGui::TextColored(theme::kTextSecondary, "%s", asset.fileName.c_str());
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Open", "Enter")) ctx.app.postEvent(OpenAssetEvent{asset.id});
    if (ImGui::BeginMenu("Rating")) {
        for (int stars = 0; stars <= 5; ++stars) {
            const std::string label = stars == 0 ? std::string("None")
                                                 : std::to_string(stars) + (stars == 1 ? " star" : " stars");
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
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);
    const AssetMetadata& m = asset.metadata;
    if (m.cameraModel) ImGui::TextUnformatted(m.cameraModel->c_str());
    std::string exposure;
    if (m.shutterSpeed) exposure += formatShutter(*m.shutterSpeed) + "  ";
    if (m.aperture) {
        char aperture[16];
        std::snprintf(aperture, sizeof aperture, "f/%.1f  ", *m.aperture);
        exposure += aperture;
    }
    if (m.iso) exposure += "ISO " + std::to_string(*m.iso);
    if (!exposure.empty()) ImGui::TextUnformatted(exposure.c_str());
    if (asset.dateCaptured) ImGui::TextUnformatted(formatDateTime(*asset.dateCaptured).c_str());
    ImGui::PopStyleColor();
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
    // The culling keys go to the focused grid or filmstrip; the ring shows which.
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    drawAssetCard(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), asset, selected,
                  hovered, caption, focused);
    assetTooltip(asset);
    assetContextMenu(ctx, asset);
    ImGui::PopID();
}

void centeredText(const char* text) {
    const glm::vec2 available = ImGui::GetContentRegionAvail();
    const glm::vec2 size = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(glm::vec2(ImGui::GetCursorPos()) + glm::max((available - size) * 0.5f, glm::vec2(0.0f)));
    ImGui::TextColored(theme::kTextSecondary, "%s", text);
}

// Filter bar above the grid and the filmstrip: the collection, how many photos
// are showing, and the filters used while culling. With `flow` it wraps onto
// further rows when the panel is narrow (grid). Without, it stays on one row
// and drops controls from the right (filmstrip); the "all filters" button at
// the end opens the Filters panel, which always has every control. The grid
// passes its thumbnail size, which gets a slider at the right end.
void filterBar(PanelContext& ctx, bool flow, float* thumbnailSize = nullptr) {
    LibraryModel& library = ctx.library;
    SearchFilter& filter = library.filter();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();
    const float em = ImGui::GetFontSize();
    const float gap = 12.0f * s;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float sliderWidth = thumbnailSize ? em * 6.5f + gap : 0.0f;
    // The trailing controls are always shown; the first row keeps room for them.
    const float buttons = sliderWidth + iconButtonWidth() * (filter.active() ? 2.0f : 1.0f) + gap;

    // Whether `width` more pixels fit after the item just submitted; wraps or declines if not.
    bool firstRow = true;
    const auto place = [&](float width, float spacing) {
        const float limit = right - (firstRow ? buttons : 0.0f);
        if (ImGui::GetItemRectMax().x + spacing + width <= limit) {
            ImGui::SameLine(0.0f, spacing);
            return true;
        }
        if (flow) firstRow = false;  // the item starts a new row
        return flow;
    };

    ImGui::PushID("##FilterBar");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(library.collectionTitle().c_str());
    {
        char count[48];
        if (filter.active()) {
            std::snprintf(count, sizeof count, "%zu of %zu", library.visible().size(), library.totalCount());
        } else {
            std::snprintf(count, sizeof count, "%zu", library.visible().size());
        }
        ImGui::SameLine(0.0f, 6.0f * s);
        const SmallText small;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::kTextSecondary, "%s", count);
    }

    const float starSize = 13.0f * s;
    float upWidth = 0.0f;
    {
        const SmallText small;
        upWidth = ImGui::CalcTextSize("& up").x;
    }
    if (place(starSize * 5.5f + 4.0f * s + upWidth, gap + 4.0f * s)) {
        ratingWidget("##MinRating", filter.minRating, starSize);
        ImGui::SameLine(0.0f, 4.0f * s);
        const SmallText small;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(filter.minRating > 0 ? theme::kTextSecondary : theme::kTextTertiary, "& up");
    }

    const float flagWidth = em * 7.6f;
    if (place(flagWidth, gap)) {
        static constexpr const char* kFlags[] = {"Any flag", "Picked", "Unflagged", "Rejected", "Not rejected"};
        int flag = static_cast<int>(filter.flag);
        ImGui::SetNextItemWidth(flagWidth);
        if (ImGui::Combo("##Flag", &flag, kFlags, IM_ARRAYSIZE(kFlags))) filter.flag = static_cast<FlagFilter>(flag);
    }

    if (place(colorLabelPickerWidth(true), gap)) colorLabelPicker("##Label", filter.colorLabel, /*allowAny=*/true);

    const float sortWidth = em * 11.0f;
    if (flow && place(sortWidth, gap)) {
        static constexpr const char* kSorts[] = {"Capture time (newest)", "Capture time (oldest)", "File name", "Rating"};
        int sort = static_cast<int>(library.sortOrder());
        ImGui::SetNextItemWidth(sortWidth);
        if (ImGui::Combo("##Sort", &sort, kSorts, IM_ARRAYSIZE(kSorts))) library.setSortOrder(static_cast<SortOrder>(sort));
    }

    // The search field takes what is left of the row, within sensible bounds.
    const float searchMin = em * 7.0f;
    if (place(searchMin, gap)) {
        const float limit = right - (firstRow ? buttons : 0.0f);
        const float width = std::clamp(limit - ImGui::GetCursorScreenPos().x, searchMin, em * 16.0f);
        char text[256];
        std::snprintf(text, sizeof text, "%s", filter.text.c_str());
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(26.0f * s, style.FramePadding.y));
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputTextWithHint("##Search", "Search", text, sizeof text)) filter.text = text;
        ImGui::PopStyleVar();
        ImGui::SetItemTooltip("File name, camera or lens");
        drawIcon(ImGui::GetWindowDrawList(), Icon::SEARCH, ImVec2(pos.x + 13.0f * s, pos.y + ImGui::GetFrameHeight() * 0.5f),
                 u32(theme::kTextSecondary));
    }

    // At the right end of the first row; after a wrap, of the last row if they still fit there.
    if (firstRow || ImGui::GetItemRectMax().x + buttons <= right) alignRight(buttons - gap);
    if (thumbnailSize) {
        // A slim track: the native slider supplies the knob and the behaviour.
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = sliderWidth - gap;
        const float centerY = std::floor(pos.y + ImGui::GetFrameHeight() * 0.5f);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + 2.0f, centerY - 2.0f * s),
                                                  ImVec2(pos.x + width - 2.0f, centerY + 2.0f * s),
                                                  u32(theme::kFillActive), 2.0f * s);
        const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, clear);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clear);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, clear);
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("##ThumbSize", thumbnailSize, 72.0f, 280.0f, "");
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("Thumbnail size");
        ImGui::SameLine(0.0f, gap);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, style.ItemSpacing.y));
    const bool advanced = !filter.camera.empty() || filter.isoMin > 0 || filter.isoMax > 0;
    if (iconButton("##AllFilters", Icon::SLIDERS, "All filters: camera, ISO range, sort order", advanced)) {
        ctx.requests.toggleSearchPanel = true;
    }
    if (filter.active()) {
        ImGui::SameLine();
        if (iconButton("##ClearFilters", Icon::XMARK, "Clear filters")) filter = SearchFilter{};
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
}

}  // namespace

// -----------------------------------------------------------------------------
// Library grid
// -----------------------------------------------------------------------------

void LibraryGridPanel::draw(PanelContext& ctx) {
    LibraryModel& library = ctx.library;
    const std::vector<AssetRecord>& assets = library.visible();
    const float s = theme::scale();
    const float spacing = kCellSpacing * s;

    // In Split the grid is narrow and the filmstrip below carries the same bar
    // in full, so the grid keeps to one row instead of wrapping onto four.
    filterBar(ctx, /*flow=*/ctx.frame.mode != AppMode::HYBRID_SPLIT, &library.thumbnailSize);

    if (!ImGui::BeginChild("##Grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs)) {
        ImGui::EndChild();
        return;
    }
    if (assets.empty()) {
        if (library.totalCount() == 0) {
            centeredText("No photos yet. Import some with the Import button, or drop a folder here.");
        } else {
            centeredText("No photos match this collection and filter.");
            if (library.filter().active()) {
                const float width = 120.0f * s;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
                if (ImGui::Button("Clear Filters", ImVec2(width, 0.0f))) library.filter() = SearchFilter{};
            }
        }
        ImGui::EndChild();
        return;
    }

    // Columns fill the width; cells stretch to share the leftover space.
    const float thumbnail = library.thumbnailSize * s;
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((available + spacing) / (thumbnail + spacing)));
    const float cellWidth = std::floor((available - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    const glm::vec2 cell(cellWidth, cellWidth * 0.82f + ImGui::GetTextLineHeight());
    const float rowHeight = cell.y + spacing;
    const int rows = static_cast<int>((assets.size() + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));

    handleAssetKeys(ctx, columns);
    if (library.selectionChanged() && library.selectedIndex()) {
        const float top = static_cast<float>(*library.selectedIndex() / static_cast<std::size_t>(columns)) * rowHeight;
        const float scroll = ImGui::GetScrollY();
        const float view = ImGui::GetWindowHeight();
        if (top < scroll) ImGui::SetScrollY(top);
        if (top + cell.y > scroll + view) ImGui::SetScrollY(top + cell.y - view);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
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
    const float s = theme::scale();
    const glm::vec2 origin = ImGui::GetCursorScreenPos();
    const glm::vec2 region = glm::max(glm::vec2(ImGui::GetContentRegionAvail()), glm::vec2(1.0f));
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const ImTextureID texture = ctx.gui.canvasTexture();
    if (texture == ImTextureID_Invalid) {
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::Dummy(region);
        const char* message = !ctx.app.canvasAvailable()
                                  ? "Canvas rendering is unavailable (GPU develop graph disabled; see the log)."
                                  : "The canvas renders in the Develop, Canvas and Split workspaces.";
        const glm::vec2 size = ImGui::CalcTextSize(message);
        drawList->AddText(origin + (region - size) * 0.5f, u32(theme::kTextSecondary), message);
        return;
    }

    const PhotoStatus& photo = ctx.app.photo();
    if (photo.state == PhotoStatus::State::READY && photo.assetId != framedAssetId_) {
        framedAssetId_ = photo.assetId;  // a newly opened photo starts fitted to the view
        zoom_ = 0.0f;
        pan_ = glm::vec2(0.0f);
    }

    // Interaction surface covering the whole view. The controls drawn over it
    // later must still get the mouse, so it allows overlap.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##Canvas", region,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();

    const glm::vec2 imageSize = ctx.gui.canvasSize();
    const float fitScale = std::min(region.x / imageSize.x, region.y / imageSize.y) * 0.94f;
    float scale = zoom_ > 0.0f ? zoom_ : fitScale;
    const glm::vec2 viewCenter = origin + region * 0.5f;

    // Develop previews zoom from 2 % to 3200 %; the compositing canvas goes
    // much further both ways, for pixel work and for large documents.
    const bool compositing = ctx.frame.mode == AppMode::CANVAS;
    const float minZoom = compositing ? 0.005f : 0.02f;
    const float maxZoom = compositing ? 256.0f : 32.0f;
    if (zoom_ > 0.0f) zoom_ = std::clamp(zoom_, minZoom, maxZoom);  // back from a deeper canvas zoom
    if (hovered && io.MouseWheel != 0.0f) {
        // Zoom around the cursor: the canvas point under it stays put.
        const float newScale = std::clamp(scale * std::pow(1.15f, io.MouseWheel), minZoom, maxZoom);
        const glm::vec2 cursor = io.MousePos;
        const glm::vec2 center = viewCenter + pan_;
        const glm::vec2 canvasPoint = (cursor - center) / scale;
        pan_ = cursor - canvasPoint * newScale - viewCenter;
        scale = zoom_ = newScale;
    }
    // An active masking tool (brush, gradient, eyedropper) takes the left
    // button; the view then pans with the middle button only.
    const CanvasView toolView{viewCenter + pan_ - imageSize * scale * 0.5f, scale, imageSize};
    bool toolUsed = maskingViewportInput(ctx, toolView, hovered, ImGui::IsItemActive());
    if (compositing && ctx.masking.tool == MaskTool::NONE) {
        toolUsed = canvasViewportInput(ctx, toolView, hovered, ImGui::IsItemActive()) || toolUsed;
    }
    if (ImGui::IsItemActive() && ((!toolUsed && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        pan_ += glm::vec2(io.MouseDelta);
    }
    if (!toolUsed && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
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
    const float checker = 12.0f * s;
    if (visibleMin.x < visibleMax.x && visibleMin.y < visibleMax.y) {
        drawList->AddRectFilled(visibleMin, visibleMax, IM_COL32(58, 58, 58, 255));
        const glm::vec2 firstCell = glm::floor((visibleMin - imageMin) / checker);
        for (float y = firstCell.y; imageMin.y + y * checker < visibleMax.y; y += 1.0f) {
            for (float x = firstCell.x; imageMin.x + x * checker < visibleMax.x; x += 1.0f) {
                if ((static_cast<int>(x) + static_cast<int>(y)) % 2 != 0) continue;
                const glm::vec2 a = glm::max(imageMin + glm::vec2(x, y) * checker, visibleMin);
                const glm::vec2 b = glm::min(imageMin + glm::vec2(x + 1.0f, y + 1.0f) * checker, visibleMax);
                drawList->AddRectFilled(a, b, IM_COL32(44, 44, 44, 255));
            }
        }
    }
    drawList->AddImage(texture, imageMin, imageMax);
    if (compositing && scale >= 8.0f) {
        // Pixel grid over the visible part of the canvas.
        const ImU32 gridColor = IM_COL32(0, 0, 0, static_cast<int>(std::min(scale * 4.0f, 70.0f)));
        const glm::vec2 first = glm::ceil((visibleMin - imageMin) / scale);
        for (float x = imageMin.x + first.x * scale; x < visibleMax.x; x += scale) {
            drawList->AddLine(ImVec2(x, visibleMin.y), ImVec2(x, visibleMax.y), gridColor);
        }
        for (float y = imageMin.y + first.y * scale; y < visibleMax.y; y += scale) {
            drawList->AddLine(ImVec2(visibleMin.x, y), ImVec2(visibleMax.x, y), gridColor);
        }
    }
    // A hairline around the canvas, so a dark photo still shows where it ends on black.
    drawList->AddRect(imageMin - glm::vec2(1.0f), imageMax + glm::vec2(1.0f), u32(theme::kSeparator));
    drawMaskingOverlay(ctx, CanvasView{imageMin, scale, imageSize}, hovered);
    if (compositing) {
        ctx.canvas.validate(ctx.app.document());
        if (const LayerNode* layer = ctx.canvas.selectedLayer) drawLayerOutline(*layer, drawList, imageMin, scale);
        if (ctx.masking.tool == MaskTool::NONE) drawCanvasToolOverlay(ctx, CanvasView{imageMin, scale, imageSize}, hovered);
    }
    drawList->PopClipRect();
    drawPhotoStatus(ctx, origin, region);

    // Zoom controls: one floating group, top-left.
    {
        const float pad = 4.0f * s;
        const float segment = 48.0f * s;
        float percentSlot = 0.0f;
        {
            const SmallText small;
            percentSlot = ImGui::CalcTextSize("25600%").x + 12.0f * s;  // the widest zoom, so the group never resizes
        }
        const glm::vec2 pos = origin + glm::vec2(8.0f * s);
        const glm::vec2 size(pad * 2.0f + segment * 2.0f + percentSlot, pad * 2.0f + ImGui::GetFrameHeight());
        hudBackground(drawList, pos, pos + size);
        ImGui::SetCursorScreenPos(pos + glm::vec2(pad));
        int mode = zoom_ <= 0.0f ? 0 : zoom_ == 1.0f ? 1 : -1;
        if (segmented("##Zoom", mode, {"Fit", "100%"}, segment)) {
            zoom_ = mode == 0 ? 0.0f : 1.0f;
            pan_ = glm::vec2(0.0f);
        }
        ImGui::SetItemTooltip("Wheel zooms around the cursor, drag pans, double-click switches Fit and 100%%");
        char percent[16];
        std::snprintf(percent, sizeof percent, "%.0f%%", static_cast<double>(scale) * 100.0);
        const SmallText small;
        const glm::vec2 textSize = ImGui::CalcTextSize(percent);
        drawList->AddText(ImVec2(pos.x + size.x - pad - 6.0f * s - textSize.x, pos.y + (size.y - textSize.y) * 0.5f),
                          u32(theme::kTextSecondary), percent);
    }

    // Read-out, bottom-left: photo, canvas, develop stack, cursor position. The
    // cursor slots have a fixed width, so nothing shifts while the pointer moves.
    {
        const SmallText small;
        std::string canvas = std::to_string(static_cast<int>(imageSize.x)) + " \xC3\x97 " +
                             std::to_string(static_cast<int>(imageSize.y));
        std::string source;
        if (photo.state == PhotoStatus::State::READY) {
            source = photo.fileName + "  " + photo.format + " " + std::to_string(photo.sourceWidth) + " \xC3\x97 " +
                     std::to_string(photo.sourceHeight);
            if (photo.sourceWidth != ctx.app.canvasWidth() || photo.sourceHeight != ctx.app.canvasHeight()) {
                canvas += " preview";
            }
        }
        const std::size_t nodes = ctx.app.developStack().size();
        const std::string stack = std::to_string(nodes) + (nodes == 1 ? " node" : " nodes");
        char cursorX[16] = "";
        char cursorY[16] = "";
        if (hovered) {
            const glm::vec2 pixel = glm::floor((glm::vec2(io.MousePos) - imageMin) / scale);
            if (pixel.x >= 0.0f && pixel.y >= 0.0f && pixel.x < imageSize.x && pixel.y < imageSize.y) {
                std::snprintf(cursorX, sizeof cursorX, "%d", static_cast<int>(pixel.x));
                std::snprintf(cursorY, sizeof cursorY, "%d", static_cast<int>(pixel.y));
            }
        }

        const float gap = 14.0f * s;
        const float padX = 10.0f * s;
        const float padY = 5.0f * s;
        const float line = ImGui::GetFontSize();
        const float keyWidth = ImGui::CalcTextSize("x ").x;
        const float digits = ImGui::CalcTextSize("00000").x;
        const auto width = [](const std::string& text) { return text.empty() ? 0.0f : ImGui::CalcTextSize(text.c_str()).x; };
        const float total = padX * 2.0f + (source.empty() ? 0.0f : width(source) + gap) + width(canvas) + gap +
                            width(stack) + gap + (keyWidth + digits) * 2.0f + gap;
        const glm::vec2 pos = origin + glm::vec2(8.0f * s, region.y - line - padY * 2.0f - 8.0f * s);
        const float available = region.x - 16.0f * s;
        hudBackground(drawList, pos, pos + glm::vec2(std::min(total, available), line + padY * 2.0f));
        drawList->PushClipRect(pos, pos + glm::vec2(std::min(total, available) - padX * 0.5f, line + padY * 2.0f), true);
        float x = pos.x + padX;
        const float y = pos.y + padY;
        const auto put = [&](const std::string& text, const ImVec4& color) {
            if (text.empty()) return;
            drawList->AddText(ImVec2(x, y), u32(color), text.c_str());
            x += width(text) + gap;
        };
        put(source, theme::kText);
        put(canvas, source.empty() ? theme::kText : theme::kTextSecondary);
        put(stack, theme::kTextSecondary);
        const auto slot = [&](const char* key, const char* value) {
            drawList->AddText(ImVec2(x, y), u32(theme::kTextTertiary), key);
            const float valueWidth = ImGui::CalcTextSize(value).x;
            drawList->AddText(ImVec2(x + keyWidth + digits - valueWidth, y), u32(theme::kText), value);  // right-aligned
            x += keyWidth + digits + gap;
        };
        slot("x", cursorX);
        slot("y", cursorY);
        drawList->PopClipRect();
    }
}

// Loading spinner, decode error, or how to open a photo, over the canvas.
void ViewportPanel::drawPhotoStatus(PanelContext& ctx, glm::vec2 origin, glm::vec2 region) {
    const PhotoStatus& photo = ctx.app.photo();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float s = theme::scale();
    const glm::vec2 center = origin + region * 0.5f;
    const float fontSize = ImGui::GetFontSize();

    // A rounded panel holding `lines` (first line in `titleColor`), centred in the view.
    auto card = [&](std::initializer_list<std::string> lines, ImU32 titleColor, float topPadding) {
        const float wrap = std::min(region.x - 48.0f * s, fontSize * 34.0f);
        glm::vec2 size(0.0f, topPadding);
        for (const std::string& line : lines) {
            const glm::vec2 lineSize = ImGui::CalcTextSize(line.c_str(), nullptr, false, wrap);
            size = glm::vec2(std::max(size.x, lineSize.x), size.y + lineSize.y + 4.0f * s);
        }
        const glm::vec2 padding(fontSize * 1.2f, fontSize * 0.8f);
        const glm::vec2 min = center - size * 0.5f - padding;
        hudBackground(drawList, min, center + size * 0.5f + padding);
        glm::vec2 cursor(center.x, min.y + padding.y + topPadding);
        bool first = true;
        for (const std::string& line : lines) {
            const glm::vec2 lineSize = ImGui::CalcTextSize(line.c_str(), nullptr, false, wrap);
            drawList->AddText(ImGui::GetFont(), fontSize, glm::vec2(cursor.x - lineSize.x * 0.5f, cursor.y),
                              first ? titleColor : u32(theme::kTextSecondary), line.c_str(), nullptr, wrap);
            cursor.y += lineSize.y + 4.0f * s;
            first = false;
        }
        return min;
    };

    switch (photo.state) {
    case PhotoStatus::State::LOADING: {
        drawList->AddRectFilled(origin, origin + region, IM_COL32(0, 0, 0, 150));  // dim the previous photo
        const float radius = fontSize * 0.9f;
        const glm::vec2 top = card({"Loading " + photo.fileName, "decoding the photo for the canvas"},
                                   u32(theme::kText), radius * 2.0f + 10.0f * s);
        const float angle = static_cast<float>(ImGui::GetTime()) * 5.0f;
        const glm::vec2 spinner(center.x, top.y + fontSize * 0.8f + radius + 2.0f * s);
        drawList->PathArcTo(spinner, radius, angle, angle + 4.4f, 24);
        drawList->PathStroke(u32(theme::kAccentBright), 2.5f * s);
        break;
    }
    case PhotoStatus::State::FAILED:
        card({"Cannot display " + photo.fileName, photo.error}, u32(theme::kDanger), 0.0f);
        break;
    case PhotoStatus::State::NONE:
        // Only over the untouched empty document: once layers are added there is work to look at.
        if (ctx.app.document().childCount() <= 1) {
            card({"No photo open", "Double-click a photo in the Library or Filmstrip, or select it and press Enter."},
                 u32(theme::kText), 0.0f);
        }
        break;
    case PhotoStatus::State::READY: break;
    }
}

// -----------------------------------------------------------------------------
// Filmstrip
// -----------------------------------------------------------------------------

void FilmstripPanel::draw(PanelContext& ctx) {
    LibraryModel& library = ctx.library;
    const std::vector<AssetRecord>& assets = library.visible();
    const float s = theme::scale();
    const float spacing = kCellSpacing * s;

    filterBar(ctx, /*flow=*/false);

    const float stripHeight = ImGui::GetContentRegionAvail().y;
    const float cellHeight = std::max(stripHeight - ImGui::GetStyle().ScrollbarSize - spacing, 24.0f * s);
    const glm::vec2 cell(std::floor(cellHeight * 1.3f), cellHeight);
    const float pitch = cell.x + spacing;
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
    const bool caption = cell.y > 96.0f * s;
    for (std::size_t index = first; index < last; ++index) {
        ImGui::SetCursorPos(ImVec2(static_cast<float>(index) * pitch, 0.0f));
        assetCell(ctx, index, cell, caption);
    }
    ImGui::EndChild();
}

}  // namespace darkhouse::ui
