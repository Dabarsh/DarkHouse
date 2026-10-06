// Left dock: collection browser and metadata inspector; the floating filter panel.

#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace darkhouse::ui {
namespace {

using theme::u32;

// --- Source list -----------------------------------------------------------------
// Rows are tree nodes, so arrow keys, type-to-open and docking behave as usual,
// but their highlight is ours: a rounded bar across the whole row. The list is
// drawn on draw-list channel 1 and the bars on channel 0, behind it.

constexpr ImGuiTreeNodeFlags kRowFlags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding;

struct SourceList {
    SourceList() {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));  // rows abut
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::GetWindowDrawList()->ChannelsSplit(2);
        ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
    }
    ~SourceList() {
        ImGui::GetWindowDrawList()->ChannelsMerge();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }
    SourceList(const SourceList&) = delete;
    SourceList& operator=(const SourceList&) = delete;
};

// Highlight and trailing count for the tree row just submitted.
void decorateRow(bool selected, std::size_t count) {
    const float s = theme::scale();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (selected || ImGui::IsItemHovered()) {
        drawList->ChannelsSetCurrent(0);
        drawList->AddRectFilled(min, max, u32(selected ? theme::kSelected : theme::kFillControl),
                                ImGui::GetStyle().FrameRounding);
        drawList->ChannelsSetCurrent(1);
    }
    char text[32];
    std::snprintf(text, sizeof text, "%zu", count);
    const SmallText small;
    const ImVec2 size = ImGui::CalcTextSize(text);
    drawList->AddText(ImVec2(max.x - size.x - 8.0f * s, min.y + (max.y - min.y - size.y) * 0.5f),
                      u32(count > 0 ? theme::kTextSecondary : theme::kTextTertiary), text);
}

void collectionRow(LibraryModel& library, const char* label, CollectionKind kind) {
    const CollectionRef ref{kind, {}};
    const bool selected = library.collection() == ref;
    ImGuiTreeNodeFlags flags = kRowFlags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::TreeNodeEx(label, flags);
    if (ImGui::IsItemClicked()) library.setCollection(ref);
    decorateRow(selected, library.countFor(kind));
}

// --- Forms -----------------------------------------------------------------------

// Two-column form row: secondary label, then the widget filling the rest.
void formLabel(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kTextSecondary, "%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// --- Metadata --------------------------------------------------------------------

// The longest prefix or suffix of `text` that fits `width` with "..." added.
std::string elide(const std::string& text, float width, bool keepTail) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    // Keeps `bytes` bytes of the text, moved off the middle of a UTF-8 sequence.
    const auto shortened = [&](std::size_t bytes) {
        std::size_t cut = keepTail ? text.size() - bytes : bytes;
        while (cut > 0 && cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            cut += keepTail ? 1 : static_cast<std::size_t>(-1);
        }
        return keepTail ? "..." + text.substr(cut) : text.substr(0, cut) + "...";
    };
    std::size_t low = 0;
    std::size_t high = text.size();
    while (low < high) {
        const std::size_t mid = (low + high + 1) / 2;
        if (ImGui::CalcTextSize(shortened(mid).c_str()).x <= width) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return shortened(low);
}

// One line of the inspector: secondary label, value clipped to the panel. The
// full value is in the tooltip. Call inside a SmallText scope.
void metadataRow(const char* label, const std::string& value, bool keepTail = false) {
    const float s = theme::scale();
    const float line = ImGui::GetFontSize() + 5.0f * s;
    const float labelWidth = ImGui::GetFontSize() * 5.6f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(width, line));
    const float valueWidth = std::max(width - labelWidth - 6.0f * s, 1.0f);
    const std::string shown = elide(value, valueWidth, keepTail);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddText(ImVec2(pos.x + 6.0f * s, pos.y + 2.0f * s), u32(theme::kTextSecondary), label);
    drawList->AddText(ImVec2(pos.x + labelWidth, pos.y + 2.0f * s), u32(theme::kText), shown.c_str());
    if (shown.size() != value.size()) ImGui::SetItemTooltip("%s", value.c_str());
}

template <class T, class F>
std::string orDash(const std::optional<T>& value, F format) {
    return value ? format(*value) : std::string("-");
}

std::string formatNumber(const char* format, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, format, value);
    return buffer;
}

// "NIKON Z 8", not "NIKON CORPORATION NIKON Z 8": most models repeat the make.
std::string cameraName(const AssetMetadata& m) {
    if (!m.cameraModel) return m.cameraMake.value_or("-");
    if (!m.cameraMake) return *m.cameraModel;
    const std::string firstWord = m.cameraMake->substr(0, m.cameraMake->find(' '));
    const auto sameIgnoringCase = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    const bool repeated = m.cameraModel->size() >= firstWord.size() &&
                          std::equal(firstWord.begin(), firstWord.end(), m.cameraModel->begin(), sameIgnoringCase);
    return repeated ? *m.cameraModel : *m.cameraMake + " " + *m.cameraModel;
}

// ISO, focal length, aperture and shutter on one strip instead of four rows.
void exposureStrip(const AssetMetadata& m) {
    if (!m.iso && !m.focalLength && !m.aperture && !m.shutterSpeed) return;
    const std::array<std::string, 4> cells{
        orDash(m.iso, [](std::int32_t v) { return "ISO " + std::to_string(v); }),
        orDash(m.focalLength, [](double v) { return formatNumber("%.0f mm", v); }),
        orDash(m.aperture, [](double v) { return formatNumber("f/%.1f", v); }),
        orDash(m.shutterSpeed, [](double v) { return formatShutter(v); }),
    };
    const float s = theme::scale();
    const float height = ImGui::GetFrameHeight() + 2.0f * s;
    const float rounding = theme::kRadiusGroup * s;
    const SmallText small;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), u32(theme::kFillGroup), rounding);
    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), u32(theme::kSeparator), rounding);
    const float cell = width / static_cast<float>(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const float x = pos.x + cell * static_cast<float>(i);
        const ImVec2 size = ImGui::CalcTextSize(cells[i].c_str());
        drawList->PushClipRect(ImVec2(x, pos.y), ImVec2(x + cell, pos.y + height), true);
        drawList->AddText(ImVec2(x + std::max((cell - size.x) * 0.5f, 2.0f * s), pos.y + (height - size.y) * 0.5f),
                          u32(theme::kText), cells[i].c_str());
        drawList->PopClipRect();
        if (i > 0) {
            drawList->AddLine(ImVec2(std::floor(x), pos.y + 5.0f * s), ImVec2(std::floor(x), pos.y + height - 5.0f * s),
                              u32(theme::kSeparator));
        }
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// Collections
// -----------------------------------------------------------------------------

void CollectionsPanel::draw(PanelContext& ctx) {
    LibraryModel& library = ctx.library;
    if (!library.lastError().empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(theme::kDanger, "Catalog error: %s", library.lastError().c_str());
        ImGui::PopTextWrapPos();
    }
    if (library.totalCount() == 0) {
        ImGui::TextWrapped("Your catalog is empty.");
        if (labelButton("##ImportEmpty", Icon::IMPORT, "Import Photos...", /*primary=*/true)) {
            ctx.requests.openImportDialog = true;
        }
        ImGui::TextColored(theme::kTextSecondary, "or drop folders onto the window");
    }

    const SourceList list;
    sectionLabel("Library");
    collectionRow(library, "All Photographs", CollectionKind::ALL);
    collectionRow(library, "Imported This Session", CollectionKind::IMPORTED_THIS_SESSION);
    collectionRow(library, "Picks", CollectionKind::PICKS);
    collectionRow(library, "Rejected", CollectionKind::REJECTED);
    collectionRow(library, "Unrated", CollectionKind::UNRATED);

    sectionLabel("Folders");
    if (library.folderTree().children.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::kTextTertiary, "   No folders yet");
    }
    for (const FolderNode& folder : library.folderTree().children) drawFolder(ctx, folder, 0);

    sectionLabel("Smart Collections");
    collectionRow(library, "Five Stars", CollectionKind::FIVE_STARS);
    collectionRow(library, "High ISO (3200+)", CollectionKind::HIGH_ISO);
    collectionRow(library, "Wide Angle (< 24 mm)", CollectionKind::WIDE_ANGLE);
    collectionRow(library, "Telephoto (135 mm+)", CollectionKind::TELEPHOTO);
}

void CollectionsPanel::drawFolder(PanelContext& ctx, const FolderNode& folder, int depth) {
    LibraryModel& library = ctx.library;
    const CollectionRef ref{CollectionKind::FOLDER, folder.path};
    const bool selected = library.collection() == ref;
    ImGuiTreeNodeFlags flags = kRowFlags | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (folder.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (depth == 0) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);

    const bool open = ImGui::TreeNodeEx(folder.path.c_str(), flags, "%s", folder.name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) library.setCollection(ref);
    ImGui::SetItemTooltip("%s", folder.path.c_str());
    decorateRow(selected, folder.count);
    if (open) {
        for (const FolderNode& child : folder.children) drawFolder(ctx, child, depth + 1);
        ImGui::TreePop();
    }
}

// -----------------------------------------------------------------------------
// Filters (PanelId::SEARCH)
// -----------------------------------------------------------------------------

void SearchPanel::draw(PanelContext& ctx) {
    LibraryModel& library = ctx.library;
    SearchFilter& filter = library.filter();

    // Keep the text box in sync when the filter is changed elsewhere (the filter bar).
    if (filter.text != text_.data()) {
        const std::size_t length = std::min(filter.text.size(), text_.size() - 1);
        std::memcpy(text_.data(), filter.text.data(), length);
        text_[length] = '\0';
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##SearchText", "Search file name, camera, lens", text_.data(), text_.size())) {
        filter.text = text_.data();
    }
    // Result count first, so it stays visible when the panel is short.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kTextSecondary, "Showing %zu of %zu", library.visible().size(), library.totalCount());
    if (filter.active()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear Filters")) filter = SearchFilter{};
    }

    if (ImGui::BeginTable("##SearchFilters", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        formLabel("Rating");
        ratingWidget("##MinRating", filter.minRating);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::kTextSecondary, filter.minRating > 0 ? "& up" : "any");

        formLabel("Flag");
        static constexpr const char* kFlags[] = {"Any", "Picked", "Unflagged", "Rejected", "Not rejected"};
        int flag = static_cast<int>(filter.flag);
        if (ImGui::Combo("##Flag", &flag, kFlags, IM_ARRAYSIZE(kFlags))) filter.flag = static_cast<FlagFilter>(flag);

        formLabel("Label");
        colorLabelPicker("##Label", filter.colorLabel, /*allowAny=*/true);

        formLabel("Camera");
        if (ImGui::BeginCombo("##Camera", filter.camera.empty() ? "Any" : filter.camera.c_str())) {
            if (ImGui::Selectable("Any", filter.camera.empty())) filter.camera.clear();
            for (const std::string& camera : library.cameras()) {
                if (ImGui::Selectable(camera.c_str(), filter.camera == camera)) filter.camera = camera;
            }
            ImGui::EndCombo();
        }

        formLabel("ISO");
        ImGui::DragIntRange2("##Iso", &filter.isoMin, &filter.isoMax, 25.0f, 0, 409600,
                             filter.isoMin > 0 ? "from %d" : "any", filter.isoMax > 0 ? "to %d" : "any",
                             ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Drag or double-click to type; 0 means no bound");

        formLabel("Sort");
        static constexpr const char* kSorts[] = {"Capture time (newest)", "Capture time (oldest)", "File name",
                                                 "Rating"};
        int sort = static_cast<int>(library.sortOrder());
        if (ImGui::Combo("##Sort", &sort, kSorts, IM_ARRAYSIZE(kSorts))) library.setSortOrder(static_cast<SortOrder>(sort));
        ImGui::EndTable();
    }
}

// -----------------------------------------------------------------------------
// Metadata
// -----------------------------------------------------------------------------

void MetadataPanel::draw(PanelContext& ctx) {
    const AssetRecord* asset = ctx.library.selected();
    if (!asset) {
        ImGui::TextColored(theme::kTextSecondary, "No photo selected");
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();

    // Title with the panel's actions on the same row, so they never scroll away.
    const float actions = 2.0f * iconButtonWidth() + 2.0f * s;
    const float titleWidth = std::max(ImGui::GetContentRegionAvail().x - actions - style.ItemSpacing.x, 1.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(elide(asset->fileName, titleWidth, /*keepTail=*/false).c_str());
    ImGui::SetItemTooltip("%s", asset->fileName.c_str());
    alignRight(actions);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * s, style.ItemSpacing.y));
    if (iconButton("##Open", Icon::OPEN, "Open in Canvas  (Enter)")) ctx.app.postEvent(OpenAssetEvent{asset->id});
    ImGui::SameLine();
    if (iconButton("##CopyPath", Icon::COPY, "Copy Path")) ImGui::SetClipboardText(asset->filePath.c_str());
    ImGui::PopStyleVar();

    // Culling on one row: rating, flag, colour label. Written through engine events.
    int rating = asset->rating;
    if (ratingWidget("##Rating", rating)) ctx.app.postEvent(SetRatingEvent{asset->id, rating});
    ImGui::SameLine(0.0f, 6.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * s, style.ItemSpacing.y));
    const auto flagButton = [&](const char* id, Icon icon, const char* tooltip, AssetFlag flag) {
        const bool active = asset->flag == flag;
        if (iconButton(id, icon, tooltip, active)) {
            ctx.app.postEvent(SetFlagEvent{asset->id, active ? AssetFlag::UNFLAGGED : flag});
        }
    };
    flagButton("##Pick", Icon::FLAG, "Pick  (P)", AssetFlag::PICKED);
    ImGui::SameLine();
    flagButton("##Reject", Icon::XMARK, "Reject  (X)", AssetFlag::REJECTED);
    ImGui::PopStyleVar();
    // The labels join the row when the panel is wide enough for them.
    ImGui::SameLine(0.0f, 6.0f * s);
    if (ImGui::GetContentRegionAvail().x < colorLabelPickerWidth(false)) ImGui::NewLine();
    int label = static_cast<int>(asset->colorLabel);
    if (colorLabelPicker("##ColorLabel", label, /*allowAny=*/false)) {
        ctx.app.postEvent(SetColorLabelEvent{asset->id, static_cast<ColorLabel>(label)});
    }

    const AssetMetadata& m = asset->metadata;
    exposureStrip(m);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
    sectionLabel("Camera");
    {
        const SmallText small;
        metadataRow("Body", cameraName(m));
        metadataRow("Lens", m.lens.value_or("-"));
    }
    sectionLabel("Dates");
    {
        const SmallText small;
        metadataRow("Captured", orDash(asset->dateCaptured, [](std::int64_t v) { return formatDateTime(v); }));
        metadataRow("Imported", formatDateTime(asset->dateImported) + " UTC");
    }
    if (m.gpsLatitude && m.gpsLongitude) {
        sectionLabel("Location");
        const SmallText small;
        char position[96];
        std::snprintf(position, sizeof position, "%.5f\xC2\xB0 %c, %.5f\xC2\xB0 %c", std::fabs(*m.gpsLatitude),
                      *m.gpsLatitude >= 0 ? 'N' : 'S', std::fabs(*m.gpsLongitude), *m.gpsLongitude >= 0 ? 'E' : 'W');
        metadataRow("GPS", position);
    }
    sectionLabel("File");
    {
        const SmallText small;
        const std::string size = asset->width > 0
                                     ? std::to_string(asset->width) + " \xC3\x97 " + std::to_string(asset->height)
                                     : std::string("unknown");
        metadataRow("Size", size + "  \xC2\xB7  " + fileExtensionTag(asset->fileName));
        metadataRow("Path", asset->filePath, /*keepTail=*/true);
        metadataRow("Hash", asset->fileHash);
        metadataRow("Asset ID", asset->id);
    }
    ImGui::PopStyleVar();
}

}  // namespace darkhouse::ui
