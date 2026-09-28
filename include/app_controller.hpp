// DarkHouse — application controller.
//
// DarkHouseApp owns every subsystem and runs the frame loop:
//
//   pump platform events -> drain AppEvent queue -> poll finished imports
//   -> (canvas visible) upload dirty tiles, evaluate the develop graph
//   -> front-end draws and presents -> pace to the target frame rate
//
// The windowing/UI layer sits behind the FrontEnd interface. The Dear ImGui +
// Vulkan swapchain front-end (GuiEngine) plugs in there and shares the
// engine's VulkanContext. The built-in headless front-end runs the same engine
// loop for batch jobs and CI.
#pragma once

#include "ai_segmentation.hpp"
#include "asset_manager.hpp"
#include "layer_stack.hpp"
#include "render_pipeline.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace darkhouse {

enum class AppMode : std::uint8_t {
    CATALOG,       // library grid: browse, rate, filter, import
    CANVAS,        // single-document editor: develop, composite, vector
    HYBRID_SPLIT,  // filmstrip + canvas side by side
};

[[nodiscard]] std::string_view toString(AppMode mode) noexcept;
[[nodiscard]] std::optional<AppMode> parseAppMode(std::string_view text) noexcept;
[[nodiscard]] constexpr bool showsCanvas(AppMode mode) noexcept { return mode != AppMode::CATALOG; }

struct AppConfig {
    std::filesystem::path catalogPath = "darkhouse_catalog.sqlite";
    std::filesystem::path shaderDirectory = "shaders";  // compiled *.spv
    std::filesystem::path modelDirectory;               // subject_segmentation.onnx / sky_segmentation.onnx
    AppMode initialMode = AppMode::CATALOG;
    std::uint32_t canvasWidth = 2048;
    std::uint32_t canvasHeight = 2048;
    bool enableGpu = true;
    bool enableValidationLayers = false;
    double targetFramesPerSecond = 60.0;
    std::optional<std::uint64_t> maxFrames;  // stop after this many frames
    bool exitWhenIdle = false;               // non-interactive runs: stop once events and imports drain
};

// Events can be posted from any thread and are handled on the main thread at
// the start of the next frame.
struct QuitEvent {};
struct SwitchModeEvent {
    AppMode mode = AppMode::CATALOG;
};
struct ImportFilesEvent {
    std::vector<std::string> absolutePaths;
};
struct SetRatingEvent {
    std::string assetId;
    int rating = 0;
};
struct OpenAssetEvent {
    std::string assetId;  // loads its develop stack; leaves CATALOG for CANVAS
};
using AppEvent = std::variant<QuitEvent, SwitchModeEvent, ImportFilesEvent, SetRatingEvent, OpenAssetEvent>;

struct FrameContext {
    std::uint64_t frameIndex = 0;
    double deltaSeconds = 0.0;
    AppMode mode = AppMode::CATALOG;
    const GPUTexture* canvasOutput = nullptr;  // developed canvas, when rendered this frame
    // Changes whenever canvasOutput may refer to a different image (develop
    // graph rebuilt or disabled). Image views can be recycled with the same
    // handle value, so front-ends key cached descriptors on this, not the view.
    std::uint64_t canvasGeneration = 0;
};

struct RunSummary {
    std::uint64_t frames = 0;
    std::size_t importsSucceeded = 0;
    std::size_t importsFailed = 0;
    std::size_t importsCancelled = 0;
};

class DarkHouseApp;

// The seam between the engine and the windowing/UI layer.
//
// GPU lifecycle, driven by DarkHouseApp:
//   configureGpu(options)  before the VulkanContext is created (add a surface)
//   attachGpu(app, gpu)    once the context exists (create swapchain, UI renderer)
//   ... frames ...
//   detachGpu()            before the context is destroyed (release everything)
class FrontEnd {
public:
    virtual ~FrontEnd() = default;

    // Lets the front-end request presentation support from the context.
    virtual void configureGpu(VulkanContextOptions& /*options*/) {}
    // True when the front-end cannot run without a GPU (initialize() then fails
    // instead of falling back to catalog-only mode).
    [[nodiscard]] virtual bool requiresGpu() const noexcept { return false; }
    virtual void attachGpu(DarkHouseApp& /*app*/, VulkanContext& /*gpu*/) {}
    virtual void detachGpu() noexcept {}

    // Turns pending OS/window input into AppEvents. Returns false once the user
    // has closed the window.
    virtual bool pumpPlatformEvents(DarkHouseApp& app) = 0;
    // Builds the UI for frame.mode and presents it. Runs after the engine work.
    virtual void drawFrame(DarkHouseApp& app, const FrameContext& frame) = 0;
    // False when nobody can post events, so exitWhenIdle may end the run.
    [[nodiscard]] virtual bool interactive() const noexcept = 0;
    // True when presentation already blocks to the display rate (vsync), so
    // the frame loop must not add its own sleep on top.
    [[nodiscard]] virtual bool pacesFrames() const noexcept { return false; }
};

class DarkHouseApp {
public:
    explicit DarkHouseApp(AppConfig config);
    ~DarkHouseApp();

    DarkHouseApp(const DarkHouseApp&) = delete;
    DarkHouseApp& operator=(const DarkHouseApp&) = delete;

    // Call before initialize(). Defaults to the headless front-end.
    void setFrontEnd(std::unique_ptr<FrontEnd> frontEnd);

    // Brings up the catalog, then the GPU, then AI. Failing to open the catalog
    // is fatal and throws. GPU and AI failures are logged and those features
    // are disabled.
    void initialize();

    // Runs the frame loop until quit, window close, maxFrames or idle exit.
    // Returns the process exit code: 0 = success, 2 = some imports failed.
    int run();

    void postEvent(AppEvent event);  // thread-safe
    void requestQuit() noexcept;     // async-signal-safe (a single atomic store)

    [[nodiscard]] AppMode mode() const noexcept { return mode_.load(); }
    [[nodiscard]] bool gpuAvailable() const noexcept { return gpu_ != nullptr; }
    // The develop graph and canvas texture exist (GPU up and shaders loaded).
    [[nodiscard]] bool canvasAvailable() const noexcept { return developGraph_ != nullptr; }
    [[nodiscard]] const VulkanContext* gpu() const noexcept { return gpu_.get(); }
    [[nodiscard]] const std::string& activeAssetId() const noexcept { return activeAssetId_; }
    [[nodiscard]] std::size_t pendingImportCount() const noexcept { return pendingImports_.size(); }
    [[nodiscard]] AssetManager& assets();
    [[nodiscard]] LayerNode& document();
    [[nodiscard]] const AppConfig& config() const noexcept { return config_; }
    [[nodiscard]] const RunSummary& summary() const noexcept { return summary_; }

private:
    void initializeGpu();
    void initializeCanvas();
    void initializeAi();
    void disableCanvas() noexcept;  // drops the develop graph and canvas, keeps the context
    void disableGpu() noexcept;     // detaches the front-end, then drops everything
    void rebuildDevelopGraph(const std::vector<EditNodeRecord>& editStack);

    // Frame phases
    void processEvents();
    void handle(const QuitEvent& event);
    void handle(const SwitchModeEvent& event);
    void handle(const ImportFilesEvent& event);
    void handle(const SetRatingEvent& event);
    void handle(const OpenAssetEvent& event);
    void pollImports();
    void uploadDirtyCanvasTiles();
    void renderFrame(FrameContext& frame);
    [[nodiscard]] bool idle() const;
    void shutdown() noexcept;

    AppConfig config_;
    std::unique_ptr<FrontEnd> frontEnd_;
    std::unique_ptr<AssetManager> assets_;
    std::unique_ptr<LayerNode> document_;
    std::unique_ptr<AISegmentationEngine> segmentation_;

    // GPU objects. The context must outlive the graph and the canvas texture;
    // shutdown() and disableGpu() release them in that order.
    std::unique_ptr<VulkanContext> gpu_;
    GPUTexture canvasTexture_;
    std::unique_ptr<RenderPipelineGraph> developGraph_;
    bool graphDirty_ = true;
    std::uint64_t canvasGeneration_ = 0;
    bool frontEndAttached_ = false;

    std::vector<std::pair<std::string, std::future<AssetRecord>>> pendingImports_;
    std::string activeAssetId_;

    mutable std::mutex eventMutex_;
    std::deque<AppEvent> events_;  // guarded by eventMutex_
    std::atomic<bool> quitRequested_{false};
    std::atomic<AppMode> mode_{AppMode::CATALOG};
    bool initialized_ = false;
    bool shutDown_ = false;
    RunSummary summary_;
};

}  // namespace darkhouse
