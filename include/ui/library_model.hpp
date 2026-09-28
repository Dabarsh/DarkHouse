// DarkHouse — library view model shared by the catalog panels.
//
// Collections, Search, Library grid, Filmstrip and Metadata all look at the
// same thing: the assets of the selected collection, narrowed by the search
// filter, in one sort order, with one selection. LibraryModel owns that state.
//
// It keeps an in-memory snapshot of the catalog, re-read only when the
// engine's catalogRevision() changes (throttled while imports stream in), and
// derives the visible list, folder tree, collection counts and camera list
// from it. Filtering in memory keeps every keystroke in the search box free of
// SQL and database round trips.
#pragma once

#include "app_controller.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace darkhouse::ui {

enum class CollectionKind : std::uint8_t {
    ALL,
    IMPORTED_THIS_SESSION,  // imported (or re-imported) by this process
    PICKS,
    REJECTED,
    UNRATED,
    FOLDER,       // CollectionRef::folder and everything below it
    FIVE_STARS,   // smart collections from here on
    HIGH_ISO,     // ISO >= 3200
    WIDE_ANGLE,   // focal length < 24 mm
    TELEPHOTO,    // focal length >= 135 mm
};

struct CollectionRef {
    CollectionKind kind = CollectionKind::ALL;
    std::string folder;  // absolute directory, for FOLDER
    bool operator==(const CollectionRef&) const = default;
};

enum class FlagFilter : std::uint8_t { ANY, PICKED, UNFLAGGED, REJECTED, NOT_REJECTED };
enum class SortOrder : std::uint8_t { CAPTURE_NEWEST, CAPTURE_OLDEST, FILE_NAME, RATING };

struct SearchFilter {
    std::string text;            // case-insensitive substring of file name, camera or lens
    int minRating = 0;           // 0..5; 0 = any
    FlagFilter flag = FlagFilter::ANY;
    int colorLabel = -1;         // -1 = any, else ColorLabel value
    int isoMin = 0;              // 0 = no bound
    int isoMax = 0;              // 0 = no bound
    std::string camera;          // exact camera model; empty = any
    bool operator==(const SearchFilter&) const = default;
    [[nodiscard]] bool active() const { return *this != SearchFilter{}; }
};

struct FolderNode {
    std::string name;      // display name (compressed chains read "Pictures/2024")
    std::string path;      // absolute directory
    std::size_t count = 0; // assets in this folder and below
    std::vector<FolderNode> children;
};

class LibraryModel {
public:
    LibraryModel();

    // Call once per frame before any panel reads the model.
    void update(DarkHouseApp& app);

    // --- Query state (setters take effect on the next update) ---------------
    [[nodiscard]] const CollectionRef& collection() const noexcept { return collection_; }
    void setCollection(CollectionRef collection);
    [[nodiscard]] SearchFilter& filter() noexcept { return filter_; }  // edited in place by the Search panel
    [[nodiscard]] SortOrder sortOrder() const noexcept { return sort_; }
    void setSortOrder(SortOrder order);

    // --- Results -------------------------------------------------------------
    [[nodiscard]] const std::vector<AssetRecord>& visible() const noexcept { return visible_; }
    [[nodiscard]] std::size_t totalCount() const noexcept { return all_.size(); }
    [[nodiscard]] std::size_t countFor(CollectionKind kind) const noexcept;
    [[nodiscard]] const FolderNode& folderTree() const noexcept { return folders_; }
    [[nodiscard]] const std::vector<std::string>& cameras() const noexcept { return cameras_; }
    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }
    [[nodiscard]] std::string collectionTitle() const;

    // --- Selection (by asset id, so it survives re-queries) ------------------
    [[nodiscard]] const AssetRecord* selected() const noexcept;
    [[nodiscard]] std::optional<std::size_t> selectedIndex() const noexcept { return selectedIndex_; }
    void select(std::size_t visibleIndex);
    void selectOffset(int delta);  // arrow-key navigation, clamped
    // Set when the selection changed this frame, so views can scroll it into view.
    [[nodiscard]] bool selectionChanged() const noexcept { return selectionChanged_; }

    // Grid thumbnail edge in pixels (Library panel zoom slider).
    float thumbnailSize = 132.0f;

private:
    void reload(DarkHouseApp& app);
    void rebuildDerived();
    void applyFilter();
    [[nodiscard]] bool inCollection(const AssetRecord& asset, const CollectionRef& collection) const;
    [[nodiscard]] bool matchesFilter(const AssetRecord& asset) const;

    std::vector<AssetRecord> all_;
    std::vector<AssetRecord> visible_;
    FolderNode folders_;
    std::vector<std::string> cameras_;
    std::vector<std::size_t> counts_;

    CollectionRef collection_;
    SearchFilter filter_;
    SearchFilter appliedFilter_;
    SortOrder sort_ = SortOrder::CAPTURE_NEWEST;
    bool viewDirty_ = true;

    std::string selectedId_;
    std::optional<std::size_t> selectedIndex_;
    bool selectionChanged_ = false;
    bool selectionPending_ = false;

    std::uint64_t loadedRevision_ = ~std::uint64_t{0};
    std::chrono::steady_clock::time_point lastReload_{};
    std::unordered_set<std::string> sessionImports_;
    std::string lastError_;
};

}  // namespace darkhouse::ui
