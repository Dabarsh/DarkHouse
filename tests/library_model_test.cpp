// LibraryModel integration test: runs the real engine headless against a
// temporary catalog, imports a small folder tree, and checks collections,
// the folder tree, search, sorting, rating/flag round trips and selection.
//
// Usage: darkhouse_library_model_test <scratch directory>

#include "app_controller.hpp"
#include "import_scan.hpp"
#include "ui/library_model.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace darkhouse;
using namespace darkhouse::ui;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::cout << "FAIL line " << __LINE__ << ": " #condition << '\n';              \
        }                                                                                  \
    } while (false)

void writeFile(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << contents;  // distinct contents: imports dedupe by hash
}

const AssetRecord* findByName(const std::vector<AssetRecord>& assets, const std::string& name) {
    for (const AssetRecord& asset : assets) {
        if (asset.fileName == name) return &asset;
    }
    return nullptr;
}

// Runs one scripted stage per engine frame until the stage reports done.
class ScriptedFrontEnd final : public FrontEnd {
public:
    using Stage = std::function<bool(DarkHouseApp&, LibraryModel&)>;
    explicit ScriptedFrontEnd(std::vector<Stage> stages) : stages_(std::move(stages)) {}

    bool pumpPlatformEvents(DarkHouseApp&) override { return true; }
    void drawFrame(DarkHouseApp& app, const FrameContext&) override {
        model_.update(app);
        if (next_ < stages_.size() && stages_[next_](app, model_)) ++next_;
        if (next_ == stages_.size()) app.requestQuit();
    }
    [[nodiscard]] bool interactive() const noexcept override { return false; }
    [[nodiscard]] bool finished() const noexcept { return next_ == stages_.size(); }
    [[nodiscard]] std::size_t stage() const noexcept { return next_; }

private:
    std::vector<Stage> stages_;
    std::size_t next_ = 0;
    LibraryModel model_;
};

}  // namespace

int main(int argc, char** argv) {
    const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "darkhouse_library_model_test";
    std::error_code ec;
    fs::remove_all(work, ec);
    const fs::path photos = fs::absolute(work / "photos");
    writeFile(photos / "2024" / "trip" / "a.jpg", "alpha");
    writeFile(photos / "2024" / "trip" / "b.jpg", "bravo");
    writeFile(photos / "2024" / "c.nef", "charlie");
    writeFile(photos / "2023" / "d.png", "delta");
    writeFile(photos / "notes.txt", "not an image");

    std::uint64_t revisionBeforeWrites = 0;
    std::vector<ScriptedFrontEnd::Stage> stages;

    // 1. Wait for the four imports to land in the model.
    stages.emplace_back([](DarkHouseApp& app, LibraryModel& model) {
        return app.pendingImportCount() == 0 && model.totalCount() == 4;
    });
    // 2. Collections and folder tree.
    stages.emplace_back([&](DarkHouseApp&, LibraryModel& model) {
        CHECK(model.visible().size() == 4);
        CHECK(model.countFor(CollectionKind::ALL) == 4);
        CHECK(model.countFor(CollectionKind::IMPORTED_THIS_SESSION) == 4);
        CHECK(model.countFor(CollectionKind::UNRATED) == 4);
        CHECK(model.countFor(CollectionKind::PICKS) == 0);

        const FolderNode& root = model.folderTree();
        CHECK(root.children.size() == 1);
        if (root.children.size() == 1) {
            const FolderNode& top = root.children.front();
            CHECK(fs::path(top.path) == photos);
            CHECK(top.count == 4);
            CHECK(top.children.size() == 2);
            if (top.children.size() == 2) {
                CHECK(top.children[0].name == "2023");
                CHECK(top.children[1].name == "2024");
                CHECK(top.children[1].count == 3);
                CHECK(top.children[1].children.size() == 1 && top.children[1].children[0].count == 2);
            }
        }
        CHECK(model.selected() != nullptr);  // first asset auto-selected
        model.setCollection(CollectionRef{CollectionKind::FOLDER, (photos / "2024").string()});
        return true;
    });
    // 3. Folder collection includes subfolders; search narrows it.
    stages.emplace_back([](DarkHouseApp&, LibraryModel& model) {
        CHECK(model.visible().size() == 3);
        CHECK(findByName(model.visible(), "d.png") == nullptr);
        model.filter().text = "B.JPG";  // case-insensitive
        return true;
    });
    stages.emplace_back([](DarkHouseApp&, LibraryModel& model) {
        CHECK(model.visible().size() == 1 && model.visible()[0].fileName == "b.jpg");
        model.filter().text = "jpg zzz";  // every word must match
        return true;
    });
    stages.emplace_back([](DarkHouseApp&, LibraryModel& model) {
        CHECK(model.visible().empty());
        CHECK(model.selected() == nullptr);
        model.filter() = SearchFilter{};
        model.setCollection(CollectionRef{});
        model.setSortOrder(SortOrder::FILE_NAME);
        return true;
    });
    // 4. Sorting, selection clamping, and catalog writes through engine events.
    stages.emplace_back([&](DarkHouseApp& app, LibraryModel& model) {
        CHECK(model.visible().size() == 4);
        if (model.visible().size() == 4) {
            CHECK(model.visible()[0].fileName == "a.jpg" && model.visible()[3].fileName == "d.png");
        }
        model.select(1);
        model.selectOffset(+10);
        CHECK(model.selectedIndex() == std::optional<std::size_t>(3));
        model.selectOffset(-10);
        CHECK(model.selectedIndex() == std::optional<std::size_t>(0));

        revisionBeforeWrites = app.catalogRevision();
        const AssetRecord* b = findByName(model.visible(), "b.jpg");
        const AssetRecord* c = findByName(model.visible(), "c.nef");
        CHECK(b && c);
        if (b && c) {
            app.postEvent(SetRatingEvent{b->id, 5});
            app.postEvent(SetFlagEvent{b->id, AssetFlag::PICKED});
            app.postEvent(SetFlagEvent{c->id, AssetFlag::REJECTED});
            app.postEvent(SetColorLabelEvent{c->id, ColorLabel::GREEN});
        }
        return true;
    });
    stages.emplace_back([&](DarkHouseApp& app, LibraryModel& model) {
        if (app.catalogRevision() < revisionBeforeWrites + 4) return false;  // events land next frame
        CHECK(model.countFor(CollectionKind::FIVE_STARS) == 1);
        CHECK(model.countFor(CollectionKind::PICKS) == 1);
        CHECK(model.countFor(CollectionKind::REJECTED) == 1);
        CHECK(model.countFor(CollectionKind::UNRATED) == 3);
        model.filter().flag = FlagFilter::NOT_REJECTED;
        model.filter().colorLabel = -1;
        return true;
    });
    stages.emplace_back([](DarkHouseApp&, LibraryModel& model) {
        CHECK(model.visible().size() == 3);
        model.filter() = SearchFilter{};
        model.filter().colorLabel = static_cast<int>(ColorLabel::GREEN);
        model.setSortOrder(SortOrder::RATING);
        return true;
    });
    stages.emplace_back([](DarkHouseApp&, LibraryModel& model) {
        CHECK(model.visible().size() == 1 && model.visible()[0].fileName == "c.nef");
        return true;
    });

    auto frontEnd = std::make_unique<ScriptedFrontEnd>(std::move(stages));
    ScriptedFrontEnd* script = frontEnd.get();
    try {
        AppConfig config;
        config.catalogPath = work / "catalog.sqlite";
        config.enableGpu = false;
        config.targetFramesPerSecond = 1000.0;
        config.maxFrames = 5000;  // safety net: a stuck stage fails instead of hanging
        DarkHouseApp app(config);
        app.setFrontEnd(std::move(frontEnd));
        app.initialize();
        const ImportScan scan = scanImportPaths({photos.string()});
        CHECK(scan.files.size() == 4);  // notes.txt is skipped
        app.postEvent(ImportFilesEvent{scan.files});
        app.run();
        if (!script->finished()) {
            ++g_failures;
            std::cout << "FAIL: stuck in stage " << script->stage() << '\n';
        }
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        return 1;
    }

    std::cout << (g_failures == 0 ? "PASS" : "FAILED") << '\n';
    return g_failures == 0 ? 0 : 1;
}
