// Left dock: file / collection browser, search filter, metadata inspector.

#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace darkhouse::ui {
namespace {

// Right-aligns a dimmed count on the current line.
void trailingCount(std::size_t count) {
    char text[32];
    std::snprintf(text, sizeof text, "%zu", count);
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width));
    ImGui::TextDisabled("%s", text);
}

void collectionRow(LibraryModel& library, const char* label, CollectionKind kind) {
    const CollectionRef ref{kind, {}};
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (library.collection() == ref) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::TreeNodeEx(label, flags);
    if (ImGui::IsItemClicked()) library.setCollection(ref);
    trailingCount(library.countFor(kind));
}

// Two-column form row: dimmed label, then the widget filling the rest.
void formLabel(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void metadataRow(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", value.c_str());
}

void metadataSection(const char* title) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kAccent);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
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

}  // namespace

// -----------------------------------------------------------------------------
// Collections
// -----------------------------------------------------------------------------

void CollectionsPanel::draw(PanelContext& ctx) {
    LibraryModel& library = ctx.library;
    if (!library.lastError().empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(theme::kReject, "Catalog error: %s", library.lastError().c_str());
        ImGui::PopTextWrapPos();
    }
    if (library.totalCount() == 0) {
        ImGui::Spacing();
        ImGui::TextWrapped("Your catalog is empty.");
        if (ImGui::Button("Import Photos...")) ctx.requests.openImportDialog = true;
        ImGui::TextDisabled("or drop folders onto the window");
        ImGui::Spacing();
    }

    ImGui::SeparatorText("Library");
    collectionRow(library, "All Photographs", CollectionKind::ALL);
    collectionRow(library, "Imported This Session", CollectionKind::IMPORTED_THIS_SESSION);
    collectionRow(library, "Picks", CollectionKind::PICKS);
    collectionRow(library, "Rejected", CollectionKind::REJECTED);
    collectionRow(library, "Unrated", CollectionKind::UNRATED);

    ImGui::SeparatorText("Folders");
    if (library.folderTree().children.empty()) ImGui::TextDisabled("No folders yet");
    for (const FolderNode& folder : library.folderTree().children) drawFolder(ctx, folder, 0);

    ImGui::SeparatorText("Smart Collections");
    collectionRow(library, "Five Stars", CollectionKind::FIVE_STARS);
    collectionRow(library, "High ISO (3200+)", CollectionKind::HIGH_ISO);
    collectionRow(library, "Wide Angle (< 24 mm)", CollectionKind::WIDE_ANGLE);
    collectionRow(library, "Telephoto (135 mm+)", CollectionKind::TELEPHOTO);
}

void CollectionsPanel::drawFolder(PanelContext& ctx, const FolderNode& folder, int depth) {
    LibraryModel& library = ctx.library;
    const CollectionRef ref{CollectionKind::FOLDER, folder.path};
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (folder.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (library.collection() == ref) flags |= ImGuiTreeNodeFlags_Selected;
    if (depth == 0) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);

    const bool open = ImGui::TreeNodeEx(folder.path.c_str(), flags, "%s", folder.name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) library.setCollection(ref);
    ImGui::SetItemTooltip("%s", folder.path.c_str());
    trailingCount(folder.count);
    if (open) {
        for (const FolderNode& child : folder.children) drawFolder(ctx, child, depth + 1);
        ImGui::TreePop();
    }
}

// -----------------------------------------------------------------------------
// Search
// -----------------------------------------------------------------------------

void SearchPanel::draw(PanelContext& ctx) {
    LibraryModel& library = ctx.library;
    SearchFilter& filter = library.filter();

    // Keep the text box in sync when the filter is cleared elsewhere.
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
    ImGui::TextDisabled("Showing %zu of %zu", library.visible().size(), library.totalCount());
    if (filter.active()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear filters")) filter = SearchFilter{};
    }

    if (ImGui::BeginTable("##SearchFilters", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        formLabel("Rating");
        ratingWidget("##MinRating", filter.minRating);
        ImGui::SameLine();
        ImGui::TextDisabled(filter.minRating > 0 ? "& up" : "any");

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
        ImGui::SetItemTooltip("Drag or Ctrl+click to type; 0 means no bound");

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
        ImGui::TextDisabled("No photo selected");
        return;
    }

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(asset->fileName.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s  |  %s", fileExtensionTag(asset->fileName).c_str(),
                        asset->width > 0 ? (std::to_string(asset->width) + " x " + std::to_string(asset->height)).c_str()
                                         : "size unknown");

    // Culling: rating, flag, colour label. Written through engine events.
    int rating = asset->rating;
    if (ratingWidget("##Rating", rating)) ctx.app.postEvent(SetRatingEvent{asset->id, rating});
    ImGui::SameLine(0.0f, ImGui::GetFontSize());
    auto flagButton = [&](const char* label, AssetFlag flag, const ImVec4& color) {
        const bool active = asset->flag == flag;
        ImGui::PushStyleColor(ImGuiCol_Text, active ? color : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::SmallButton(label)) {
            ctx.app.postEvent(SetFlagEvent{asset->id, active ? AssetFlag::UNFLAGGED : flag});
        }
        ImGui::PopStyleColor();
    };
    flagButton("Pick", AssetFlag::PICKED, theme::kPick);
    ImGui::SameLine();
    flagButton("Reject", AssetFlag::REJECTED, theme::kReject);
    int label = static_cast<int>(asset->colorLabel);
    if (colorLabelPicker("##ColorLabel", label, /*allowAny=*/false)) {
        ctx.app.postEvent(SetColorLabelEvent{asset->id, static_cast<ColorLabel>(label)});
    }

    const AssetMetadata& m = asset->metadata;
    if (ImGui::BeginTable("##Metadata", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        metadataSection("Camera");
        metadataRow("Make", m.cameraMake.value_or("-"));
        metadataRow("Model", m.cameraModel.value_or("-"));
        metadataRow("Lens", m.lens.value_or("-"));

        metadataSection("Exposure");
        metadataRow("ISO", orDash(m.iso, [](std::int32_t v) { return std::to_string(v); }));
        metadataRow("Aperture", orDash(m.aperture, [](double v) { return formatNumber("f/%.1f", v); }));
        metadataRow("Shutter", orDash(m.shutterSpeed, [](double v) { return formatShutter(v); }));
        metadataRow("Focal length", orDash(m.focalLength, [](double v) { return formatNumber("%.0f mm", v); }));

        metadataSection("Dates");
        metadataRow("Captured", orDash(asset->dateCaptured, [](std::int64_t v) { return formatDateTime(v); }));
        metadataRow("Imported", formatDateTime(asset->dateImported) + " UTC");

        metadataSection("Location");
        if (m.gpsLatitude && m.gpsLongitude) {
            char position[96];
            std::snprintf(position, sizeof position, "%.5f\xC2\xB0 %c, %.5f\xC2\xB0 %c", std::fabs(*m.gpsLatitude),
                          *m.gpsLatitude >= 0 ? 'N' : 'S', std::fabs(*m.gpsLongitude), *m.gpsLongitude >= 0 ? 'E' : 'W');
            metadataRow("GPS", position);
        } else {
            metadataRow("GPS", "-");
        }

        metadataSection("File");
        metadataRow("Path", asset->filePath);
        metadataRow("Hash", asset->fileHash);
        metadataRow("Asset ID", asset->id);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Open Photo")) ctx.app.postEvent(OpenAssetEvent{asset->id});
    ImGui::SameLine();
    if (ImGui::Button("Copy Path")) ImGui::SetClipboardText(asset->filePath.c_str());
}

}  // namespace darkhouse::ui
