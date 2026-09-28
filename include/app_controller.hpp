// DarkHouse — application controller.
//
// DarkHouseApp owns every subsystem and runs the frame loop:
//
//   pump platform events -> drain AppEvent queue -> poll finished imports
//   -> adopt a finished photo decode -> (canvas visible) upload dirty tiles,
//   evaluate the develop graph -> front-end draws and presents -> pace to the
//   target frame rate
//
// Opening an asset loads its develop stack at once and decodes its pixels on a
// worker thread (image_decoder.hpp). The decoded photo replaces the document,
// which is resized to it, so the canvas shows the photo with every develop
// edit applied live.
//
// The windowing/UI layer sits behind the FrontEnd interface. The Dear ImGui +
// Vulkan swapchain front-end (GuiEngine) plugs in there and shares the
// engine's VulkanContext. The built-in headless front-end runs the same engine
// loop for batch jobs and CI.
#pragma once

#include "ai_segmentation.hpp"
#include "asset_manager.hpp"
#include "image_decoder.hpp"
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
    std::uint32_t canvasWidth = 2048;  // the empty document before a photo is opened
    std::uint32_t canvasHeight = 2048;
    // Longest edge of an opened photo on the canvas. Larger files are
    // area-downscaled in linear light, which keeps live editing interactive;
    // 0 loads full resolution.
    std::uint32_t previewMaxDimension = 3072;
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
struct SetFlagEvent {
    std::string assetId;
    AssetFlag flag = AssetFlag::UNFLAGGED;
};
struct SetColorLabelEvent {
    std::string assetId;
    ColorLabel label = ColorLabel::NONE;
};
struct OpenAssetEvent {
    std::string assetId;  // loads its develop stack and photo; leaves CATALOG for CANVAS
};
// Replaces the parameters of one develop-stack node (same bytes as
// edit_nodes.serialized_params) and re-renders the canvas. With `persist`,
// the stack is also saved to the open asset's catalog entry; UIs send
// persist=false while a slider is dragged and true when it is released.
struct SetDevelopParamsEvent {
    std::uint32_t nodeIndex = 0;
    std::vector<std::byte> serializedParams;
    bool persist = false;
    std::string nodeType;  // when set, the event is dropped unless the node at nodeIndex is of this type
};
// Replaces the whole develop stack, e.g. to add or remove a node, rebuilds
// the develop graph and re-renders. Nodes are renumbered in order. With
// `persist`, the stack is saved to the open asset's catalog entry. A stack
// that cannot be built is rejected and the previous one kept.
struct SetDevelopStackEvent {
    std::vector<EditNodeRecord> stack;
    bool persist = true;
};
using AppEvent = std::variant<QuitEvent, SwitchModeEvent, ImportFilesEvent, SetRatingEvent, SetFlagEvent,
                              SetColorLabelEvent, OpenAssetEvent, SetDevelopParamsEvent, SetDevelopStackEvent>;

struct FrameContext {
    std::uint64_t frameIndex = 0;
    double deltaSeconds = 0.0;
    AppMode mode = AppMode::CATALOG;
    // Developed canvas after the display transform (sRGB-encoded RGBA8, straight
    // alpha), when rendered this frame.
    const GPUTexture* canvasOutput = nullptr;
    // Changes whenever canvasOutput may refer to a different image (develop
    // graph rebuilt or disabled). Image views can be recycled with the same
    // handle value, so front-ends key cached descriptors on this, not the view.
    std::uint64_t canvasGeneration = 0;
};

// The photo behind the canvas: the open asset's decoded pixels.
struct PhotoStatus {
    enum class State : std::uint8_t {
        NONE,     // no asset opened yet: an empty document
        LOADING,  // decoding on a worker thread; the canvas still shows the previous document
        READY,    // the document holds the photo
        FAILED,   // could not decode; the canvas is empty and `error` says why
    };
    State state = State::NONE;
    std::string assetId;
    std::string fileName;
    std::string format;                // decoder's description, e.g. "JPEG", "NEF embedded JPEG preview"
    std::uint32_t sourceWidth = 0;     // full-resolution size as displayed (EXIF orientation
    std::uint32_t sourceHeight = 0;    // applied), before the preview downscale
    double decodeSeconds = 0.0;        // decode + document build on the worker
    std::string error;
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
    [[nodiscard]] const PhotoStatus& photo() const noexcept { return photo_; }
    // Size of the document and canvas: the opened photo, or the configured
    // empty canvas before one is opened.
    [[nodiscard]] std::uint32_t canvasWidth() const noexcept { return canvasWidth_; }
    [[nodiscard]] std::uint32_t canvasHeight() const noexcept { return canvasHeight_; }
    // The develop stack currently on the canvas (an identity exposure node when
    // no asset is open or its stack is empty).
    [[nodiscard]] const std::vector<EditNodeRecord>& developStack() const noexcept { return developStack_; }
    // The GPU node running develop-stack entry `index`, for read-outs such as
    // DenoiseNode::statistics(); nullptr without a develop graph.
    [[nodiscard]] const ComputeNode* developNode(std::size_t index) const;
    // GPU time of each develop-graph node (display transform included) at its
    // last evaluation; empty when the device has no timestamps.
    [[nodiscard]] const std::vector<RenderPipelineGraph::NodeTiming>& developTimings() const noexcept {
        return developTimings_;
    }
    // A develop evaluation is running on the GPU, or an edit is waiting for
    // the next one (the canvas is visible). Front-ends keep drawing meanwhile.
    [[nodiscard]] bool developBusy() const noexcept;
    // Increments whenever the catalog may have changed (import finished,
    // rating, flag or label written), so views know when to re-query.
    [[nodiscard]] std::uint64_t catalogRevision() const noexcept { return catalogRevision_; }
    // Ids of assets imported by this process, in completion order (a
    // re-import of known content yields the existing id).
    [[nodiscard]] const std::vector<std::string>& sessionImports() const noexcept { return sessionImports_; }
    [[nodiscard]] std::size_t pendingImportCount() const noexcept { return pendingImports_.size(); }
    [[nodiscard]] AssetManager& assets();
    [[nodiscard]] LayerNode& document();
    [[nodiscard]] const AppConfig& config() const noexcept { return config_; }
    [[nodiscard]] const RunSummary& summary() const noexcept { return summary_; }

private:
    void initializeGpu();
    void initializeCanvas();
    void createCanvasTexture();
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
    void handle(const SetFlagEvent& event);
    void handle(const SetColorLabelEvent& event);
    void handle(const OpenAssetEvent& event);
    void handle(const SetDevelopParamsEvent& event);
    void handle(const SetDevelopStackEvent& event);
    void pollImports();
    void startPhotoLoad(const AssetRecord& asset);
    void pollPhotoLoad();
    void replaceDocument(std::unique_ptr<LayerNode> document, std::uint32_t width, std::uint32_t height);
    void uploadDirtyCanvasTiles();
    void renderFrame(FrameContext& frame);
    [[nodiscard]] bool idle() const;
    void shutdown() noexcept;

    AppConfig config_;
    std::unique_ptr<FrontEnd> frontEnd_;
    std::unique_ptr<AssetManager> assets_;
    std::unique_ptr<LayerNode> document_;
    std::uint32_t canvasWidth_ = 0;  // document size
    std::uint32_t canvasHeight_ = 0;
    std::unique_ptr<AISegmentationEngine> segmentation_;

    // GPU objects. The context must outlive the graph and the canvas texture;
    // shutdown() and disableGpu() release them in that order.
    std::unique_ptr<VulkanContext> gpu_;
    GPUTexture canvasTexture_;
    std::unique_ptr<RenderPipelineGraph> developGraph_;
    std::vector<RenderPipelineGraph::NodeTiming> developTimings_;
    VulkanContext::Submission developSubmission_;  // the develop evaluation in flight, if any
    bool graphDirty_ = true;
    std::uint64_t canvasGeneration_ = 0;
    bool frontEndAttached_ = false;

    std::vector<std::pair<std::string, std::future<AssetRecord>>> pendingImports_;
    std::string activeAssetId_;

    // Photo decoding. A superseded decode cannot be cancelled; its future
    // (std::async, whose destructor blocks) is parked until it finishes.
    struct LoadedPhoto {
        std::unique_ptr<LayerNode> document;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::string format;
        std::uint32_t sourceWidth = 0;
        std::uint32_t sourceHeight = 0;
        double seconds = 0.0;
    };
    [[nodiscard]] static LoadedPhoto loadPhoto(const std::filesystem::path& path, DecodeOptions options);
    PhotoStatus photo_;
    std::future<LoadedPhoto> photoLoad_;
    std::vector<std::future<LoadedPhoto>> abandonedPhotoLoads_;

    std::vector<EditNodeRecord> developStack_;
    std::uint64_t catalogRevision_ = 0;
    std::vector<std::string> sessionImports_;

    mutable std::mutex eventMutex_;
    std::deque<AppEvent> events_;  // guarded by eventMutex_
    std::atomic<bool> quitRequested_{false};
    std::atomic<AppMode> mode_{AppMode::CATALOG};
    bool initialized_ = false;
    bool shutDown_ = false;
    RunSummary summary_;
};

}  // namespace darkhouse
