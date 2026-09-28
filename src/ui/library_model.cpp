#include "ui/library_model.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

namespace darkhouse::ui {
namespace {

namespace fs = std::filesystem;

// While imports stream in, the revision changes once per file; re-reading the
// catalog that often would stall the UI on large imports.
constexpr std::chrono::milliseconds kReloadThrottle{400};
constexpr std::size_t kCollectionKinds = static_cast<std::size_t>(CollectionKind::TELEPHOTO) + 1;

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool newerCapture(const AssetRecord& a, const AssetRecord& b) {
    if (a.dateCaptured.has_value() != b.dateCaptured.has_value()) return a.dateCaptured.has_value();  // unknown last
    if (a.dateCaptured && *a.dateCaptured != *b.dateCaptured) return *a.dateCaptured > *b.dateCaptured;
    return a.fileName < b.fileName;
}

bool olderCapture(const AssetRecord& a, const AssetRecord& b) {
    if (a.dateCaptured.has_value() != b.dateCaptured.has_value()) return a.dateCaptured.has_value();
    if (a.dateCaptured && *a.dateCaptured != *b.dateCaptured) return *a.dateCaptured < *b.dateCaptured;
    return a.fileName < b.fileName;
}

// Intermediate tree keyed by path component, turned into FolderNodes afterwards.
struct FolderBuilder {
    std::map<std::string, FolderBuilder> children;
    std::size_t count = 0;   // assets at or below this folder
    std::size_t direct = 0;  // assets directly in this folder
};

FolderNode toFolderNode(const std::string& name, const fs::path& path, const FolderBuilder& builder) {
    FolderNode node;
    node.name = name;
    node.path = path.string();
    node.count = builder.count;
    for (const auto& [childName, child] : builder.children) {
        node.children.push_back(toFolderNode(childName, path / childName, child));
    }
    // Collapse chains of folders that only lead somewhere else, so a library
    // under /home/me/Pictures shows one "Pictures" level instead of four. The
    // merged node is named by its path relative to this folder's parent, or
    // by its full path at the top (root-like folders have no file name).
    if (builder.direct == 0 && node.children.size() == 1) {
        FolderNode only = std::move(node.children.front());
        only.name = path.has_filename() ? fs::path(only.path).lexically_relative(path.parent_path()).string() : only.path;
        return only;
    }
    return node;
}

}  // namespace

LibraryModel::LibraryModel() : counts_(kCollectionKinds, 0) {}

void LibraryModel::setCollection(CollectionRef collection) {
    if (collection == collection_) return;
    collection_ = std::move(collection);
    viewDirty_ = true;
}

void LibraryModel::setSortOrder(SortOrder order) {
    if (order == sort_) return;
    sort_ = order;
    viewDirty_ = true;
}

std::size_t LibraryModel::countFor(CollectionKind kind) const noexcept {
    const auto i = static_cast<std::size_t>(kind);
    return i < counts_.size() ? counts_[i] : 0;
}

std::string LibraryModel::collectionTitle() const {
    switch (collection_.kind) {
    case CollectionKind::ALL: return "All Photographs";
    case CollectionKind::IMPORTED_THIS_SESSION: return "Imported This Session";
    case CollectionKind::PICKS: return "Picks";
    case CollectionKind::REJECTED: return "Rejected";
    case CollectionKind::UNRATED: return "Unrated";
    case CollectionKind::FOLDER: return fs::path(collection_.folder).filename().string();
    case CollectionKind::FIVE_STARS: return "Five Stars";
    case CollectionKind::HIGH_ISO: return "High ISO";
    case CollectionKind::WIDE_ANGLE: return "Wide Angle";
    case CollectionKind::TELEPHOTO: return "Telephoto";
    }
    return {};
}

void LibraryModel::update(DarkHouseApp& app) {
    selectionChanged_ = selectionPending_;
    selectionPending_ = false;

    const auto now = std::chrono::steady_clock::now();
    const bool stale = app.catalogRevision() != loadedRevision_;
    const bool firstLoad = loadedRevision_ == ~std::uint64_t{0};
    if (stale && (firstLoad || app.pendingImportCount() == 0 || now - lastReload_ >= kReloadThrottle)) {
        reload(app);
    }
    if (filter_ != appliedFilter_) viewDirty_ = true;
    if (viewDirty_) applyFilter();
}

void LibraryModel::reload(DarkHouseApp& app) {
    try {
        all_ = app.assets().queryAssets("");
        lastError_.clear();
    } catch (const std::exception& e) {
        lastError_ = e.what();
    }
    sessionImports_.clear();
    sessionImports_.insert(app.sessionImports().begin(), app.sessionImports().end());
    loadedRevision_ = app.catalogRevision();
    lastReload_ = std::chrono::steady_clock::now();
    rebuildDerived();
    viewDirty_ = true;
}

void LibraryModel::rebuildDerived() {
    std::fill(counts_.begin(), counts_.end(), 0);
    std::set<std::string> cameras;
    FolderBuilder root;
    for (const AssetRecord& asset : all_) {
        for (std::size_t kind = 0; kind < kCollectionKinds; ++kind) {
            const auto k = static_cast<CollectionKind>(kind);
            if (k != CollectionKind::FOLDER && inCollection(asset, CollectionRef{k, {}})) ++counts_[kind];
        }
        if (asset.metadata.cameraModel) cameras.insert(*asset.metadata.cameraModel);

        FolderBuilder* node = &root;
        ++node->count;
        for (const fs::path& component : fs::path(asset.filePath).parent_path()) {
            node = &node->children[component.string()];
            ++node->count;
        }
        ++node->direct;
    }
    cameras_.assign(cameras.begin(), cameras.end());
    folders_ = toFolderNode("", fs::path(), root);
    if (!folders_.name.empty()) {
        // The whole library collapsed into one folder chain: keep it as the
        // single child of an unnamed root so the panel can list it uniformly.
        FolderNode top = std::move(folders_);
        folders_ = FolderNode{};
        folders_.count = top.count;
        folders_.children.push_back(std::move(top));
    }
    // Top-level entries carry their full location in `path` (shown as a
    // tooltip); the label is just the folder's own name.
    for (FolderNode& top : folders_.children) {
        const std::string leaf = fs::path(top.path).filename().string();
        if (!leaf.empty()) top.name = leaf;
    }
}

bool LibraryModel::inCollection(const AssetRecord& asset, const CollectionRef& collection) const {
    const AssetMetadata& m = asset.metadata;
    switch (collection.kind) {
    case CollectionKind::ALL: return true;
    case CollectionKind::IMPORTED_THIS_SESSION: return sessionImports_.count(asset.id) > 0;
    case CollectionKind::PICKS: return asset.flag == AssetFlag::PICKED;
    case CollectionKind::REJECTED: return asset.flag == AssetFlag::REJECTED;
    case CollectionKind::UNRATED: return asset.rating == 0;
    case CollectionKind::FOLDER: {
        const std::string parent = fs::path(asset.filePath).parent_path().string();
        const std::string& folder = collection.folder;
        if (parent.size() < folder.size() || parent.compare(0, folder.size(), folder) != 0) return false;
        return parent.size() == folder.size() || parent[folder.size()] == '/' || parent[folder.size()] == '\\' ||
               folder.back() == '/' || folder.back() == '\\';
    }
    case CollectionKind::FIVE_STARS: return asset.rating == 5;
    case CollectionKind::HIGH_ISO: return m.iso && *m.iso >= 3200;
    case CollectionKind::WIDE_ANGLE: return m.focalLength && *m.focalLength < 24.0;
    case CollectionKind::TELEPHOTO: return m.focalLength && *m.focalLength >= 135.0;
    }
    return false;
}

bool LibraryModel::matchesFilter(const AssetRecord& asset) const {
    const SearchFilter& f = filter_;
    const AssetMetadata& m = asset.metadata;
    if (asset.rating < f.minRating) return false;
    switch (f.flag) {
    case FlagFilter::ANY: break;
    case FlagFilter::PICKED: if (asset.flag != AssetFlag::PICKED) return false; break;
    case FlagFilter::UNFLAGGED: if (asset.flag != AssetFlag::UNFLAGGED) return false; break;
    case FlagFilter::REJECTED: if (asset.flag != AssetFlag::REJECTED) return false; break;
    case FlagFilter::NOT_REJECTED: if (asset.flag == AssetFlag::REJECTED) return false; break;
    }
    if (f.colorLabel >= 0 && static_cast<int>(asset.colorLabel) != f.colorLabel) return false;
    if (f.isoMin > 0 && !(m.iso && *m.iso >= f.isoMin)) return false;
    if (f.isoMax > 0 && !(m.iso && *m.iso <= f.isoMax)) return false;
    if (!f.camera.empty() && m.cameraModel.value_or("") != f.camera) return false;
    if (!f.text.empty()) {
        const std::string haystack = lowercase(asset.fileName + ' ' + m.cameraMake.value_or("") + ' ' +
                                               m.cameraModel.value_or("") + ' ' + m.lens.value_or(""));
        // Every whitespace-separated word must match somewhere.
        const std::string needle = lowercase(f.text);
        std::size_t start = 0;
        while (start < needle.size()) {
            const std::size_t end = std::min(needle.find(' ', start), needle.size());
            if (end > start && haystack.find(needle.substr(start, end - start)) == std::string::npos) return false;
            start = end + 1;
        }
    }
    return true;
}

void LibraryModel::applyFilter() {
    visible_.clear();
    for (const AssetRecord& asset : all_) {
        if (inCollection(asset, collection_) && matchesFilter(asset)) visible_.push_back(asset);
    }
    switch (sort_) {
    case SortOrder::CAPTURE_NEWEST: std::stable_sort(visible_.begin(), visible_.end(), newerCapture); break;
    case SortOrder::CAPTURE_OLDEST: std::stable_sort(visible_.begin(), visible_.end(), olderCapture); break;
    case SortOrder::FILE_NAME:
        std::stable_sort(visible_.begin(), visible_.end(), [](const AssetRecord& a, const AssetRecord& b) {
            return lowercase(a.fileName) < lowercase(b.fileName);
        });
        break;
    case SortOrder::RATING:
        std::stable_sort(visible_.begin(), visible_.end(), [](const AssetRecord& a, const AssetRecord& b) {
            return a.rating != b.rating ? a.rating > b.rating : newerCapture(a, b);
        });
        break;
    }
    appliedFilter_ = filter_;
    viewDirty_ = false;

    // Keep the selection on the same asset; otherwise start at the first one.
    selectedIndex_.reset();
    for (std::size_t i = 0; i < visible_.size(); ++i) {
        if (visible_[i].id == selectedId_) {
            selectedIndex_ = i;
            break;
        }
    }
    if (!selectedIndex_ && !visible_.empty()) select(0);
    if (visible_.empty()) selectedId_.clear();
}

const AssetRecord* LibraryModel::selected() const noexcept {
    return selectedIndex_ && *selectedIndex_ < visible_.size() ? &visible_[*selectedIndex_] : nullptr;
}

void LibraryModel::select(std::size_t visibleIndex) {
    if (visibleIndex >= visible_.size()) return;
    if (selectedIndex_ == visibleIndex && selectedId_ == visible_[visibleIndex].id) return;
    selectedIndex_ = visibleIndex;
    selectedId_ = visible_[visibleIndex].id;
    selectionPending_ = true;
}

void LibraryModel::selectOffset(int delta) {
    if (visible_.empty()) return;
    const auto last = static_cast<long long>(visible_.size()) - 1;
    const long long current = selectedIndex_ ? static_cast<long long>(*selectedIndex_) : 0;
    select(static_cast<std::size_t>(std::clamp(current + delta, 0LL, last)));
}

}  // namespace darkhouse::ui
