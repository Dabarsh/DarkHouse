#include "app_controller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace darkhouse {
namespace {

template <class... Args>
void logLine(const char* level, const Args&... args) {
    std::ostringstream line;
    line << "[DarkHouse] " << level << ": ";
    (line << ... << args);
    line << '\n';
    std::clog << line.str();
}

class HeadlessFrontEnd final : public FrontEnd {
public:
    bool pumpPlatformEvents(DarkHouseApp&) override { return true; }
    void drawFrame(DarkHouseApp&, const FrameContext&) override {}
    [[nodiscard]] bool interactive() const noexcept override { return false; }
};

}  // namespace

std::string_view toString(AppMode mode) noexcept {
    switch (mode) {
    case AppMode::CATALOG: return "catalog";
    case AppMode::CANVAS: return "canvas";
    case AppMode::HYBRID_SPLIT: return "split";
    }
    return "unknown";
}

std::optional<AppMode> parseAppMode(std::string_view text) noexcept {
    if (text == "catalog") return AppMode::CATALOG;
    if (text == "canvas") return AppMode::CANVAS;
    if (text == "split" || text == "hybrid") return AppMode::HYBRID_SPLIT;
    return std::nullopt;
}

DarkHouseApp::DarkHouseApp(AppConfig config) : config_(std::move(config)), mode_(config_.initialMode) {}

DarkHouseApp::~DarkHouseApp() { shutdown(); }

void DarkHouseApp::setFrontEnd(std::unique_ptr<FrontEnd> frontEnd) {
    if (initialized_) throw std::logic_error("setFrontEnd must be called before initialize()");
    frontEnd_ = std::move(frontEnd);
}

AssetManager& DarkHouseApp::assets() {
    if (!assets_) throw std::logic_error("DarkHouseApp is not initialized");
    return *assets_;
}

LayerNode& DarkHouseApp::document() {
    if (!document_) throw std::logic_error("DarkHouseApp is not initialized");
    return *document_;
}

void DarkHouseApp::postEvent(AppEvent event) {
    std::lock_guard lock(eventMutex_);
    events_.push_back(std::move(event));
}

void DarkHouseApp::requestQuit() noexcept { quitRequested_.store(true); }

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

void DarkHouseApp::initialize() {
    if (initialized_) throw std::logic_error("DarkHouseApp::initialize called twice");
    if (!frontEnd_) frontEnd_ = std::make_unique<HeadlessFrontEnd>();

    // 1. Catalog: required, because nothing works without it.
    assets_ = std::make_unique<AssetManager>();
    assets_->initializeCatalog(config_.catalogPath.string());
    logLine("info", "catalog: ", config_.catalogPath.string(), " (WAL)");

    // 2. Document: a group root with one empty background layer. Tiles are
    //    sparse, so an untouched canvas uses no pixel memory.
    document_ = LayerNode::createGroup("Document");
    document_->addChild(LayerNode::createRaster("Background", config_.canvasWidth, config_.canvasHeight));

    // 3. GPU and AI are optional, and each degrades on its own.
    if (config_.enableGpu) {
        initializeGpu();
    } else if (frontEnd_->requiresGpu()) {
        throw std::runtime_error("the desktop UI needs the GPU; use --headless together with --no-gpu");
    } else {
        logLine("info", "GPU disabled by configuration");
    }
    initializeAi();

    initialized_ = true;
    logLine("info", "ready in ", toString(mode()), " mode");
}

void DarkHouseApp::initializeGpu() {
    try {
        VulkanContextOptions options;
        options.enableValidation = config_.enableValidationLayers;
        frontEnd_->configureGpu(options);
        gpu_ = std::make_unique<VulkanContext>(options);
        if (config_.enableValidationLayers && !gpu_->validationEnabled()) {
            logLine("warn", "validation requested but VK_LAYER_KHRONOS_validation could not be loaded; continuing without it");
        }
        logLine("info", "GPU: ", gpu_->deviceName(), gpu_->validationEnabled() ? " (validation layers on)" : "",
            gpu_->presentationEnabled() ? ", presenting" : ", headless");
    } catch (const std::exception& e) {
        if (frontEnd_->requiresGpu()) {
            throw std::runtime_error(std::string("the desktop UI needs a Vulkan 1.3 device that can present to a window: ") +
                                     e.what());
        }
        logLine("warn", "GPU unavailable, canvas rendering disabled: ", e.what());
        disableGpu();
        return;
    }

    // Canvas and develop graph are optional on top of a working context: a
    // missing shader should cost the canvas, not the whole UI.
    initializeCanvas();

    // The front-end sets up its swapchain and renderer last. If that fails the
    // context is still torn down cleanly by the caller's exception path.
    frontEnd_->attachGpu(*this, *gpu_);
    frontEndAttached_ = true;
}

void DarkHouseApp::initializeCanvas() {
    try {
        canvasTexture_ = gpu_->createTexture(config_.canvasWidth, config_.canvasHeight,
                                             PixelFormat::R16G16B16A16_SFLOAT,
                                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        gpu_->clearTexture(canvasTexture_, 0.0f, 0.0f, 0.0f, 0.0f);

        developGraph_ = std::make_unique<RenderPipelineGraph>();
        rebuildDevelopGraph({});
        logLine("info", "canvas ", config_.canvasWidth, "x", config_.canvasHeight, " RGBA16F");
    } catch (const std::exception& e) {
        logLine("warn", "canvas rendering disabled: ", e.what());
        disableCanvas();
    }
}

void DarkHouseApp::initializeAi() {
    try {
        segmentation_ = createSegmentationEngine();
        if (!segmentation_) {
            logLine("info", "AI segmentation not built (configure with -DDARKHOUSE_ONNXRUNTIME=ON)");
            return;
        }
        if (config_.modelDirectory.empty()) {
            logLine("info", "AI segmentation: ", segmentation_->backendName(), ", no model directory configured");
            return;
        }
        const std::array<std::pair<SegmentationTask, const char*>, 2> models{{
            {SegmentationTask::SUBJECT, "subject_segmentation.onnx"},
            {SegmentationTask::SKY, "sky_segmentation.onnx"},
        }};
        for (const auto& [task, fileName] : models) {
            const std::filesystem::path path = config_.modelDirectory / fileName;
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                logLine("info", "AI model not found (feature disabled): ", path.string());
                continue;
            }
            segmentation_->loadModel(task, path, SegmentationModelConfig{});
            logLine("info", "AI model loaded: ", path.string());
        }
    } catch (const std::exception& e) {
        logLine("warn", "AI segmentation disabled: ", e.what());
        segmentation_.reset();
    }
}

void DarkHouseApp::disableCanvas() noexcept {
    if (gpu_) gpu_->waitIdle();  // in-flight UI frames may still sample the canvas
    developGraph_.reset();
    if (gpu_) gpu_->destroyTexture(canvasTexture_);
    ++canvasGeneration_;
}

void DarkHouseApp::disableGpu() noexcept {
    if (frontEndAttached_ && frontEnd_) frontEnd_->detachGpu();
    frontEndAttached_ = false;
    disableCanvas();
    gpu_.reset();
}

void DarkHouseApp::rebuildDevelopGraph(const std::vector<EditNodeRecord>& editStack) {
    if (!gpu_ || !developGraph_) return;
    // Clearing destroys the nodes' output images, which UI frames still in
    // flight may be sampling.
    gpu_->waitIdle();
    developGraph_->clear();
    ++canvasGeneration_;

    // A develop stack is a linear chain: canvas -> node 0 -> node 1 -> ...
    // An empty stack still gets an identity exposure node, so the canvas always
    // has a developed output.
    std::vector<EditNodeRecord> stack = editStack;
    if (stack.empty()) stack.push_back({0, std::string(ExposureNode::kTypeName), ExposureNode::pack(ExposureParams{})});
    developStack_ = stack;

    std::optional<RenderPipelineGraph::NodeId> previous;
    for (const EditNodeRecord& record : stack) {
        std::unique_ptr<ComputeNode> node = createComputeNode(record.nodeType, *gpu_, config_.shaderDirectory);
        node->updateUniforms(record.serializedParams);
        const RenderPipelineGraph::NodeId id = developGraph_->addNode(std::move(node));
        if (previous) {
            developGraph_->connectNodes(*previous, id, 0);
        } else {
            developGraph_->bindExternalInput(id, 0, canvasTexture_);
        }
        previous = id;
    }
    graphDirty_ = true;
}

// -----------------------------------------------------------------------------
// Frame loop
// -----------------------------------------------------------------------------

int DarkHouseApp::run() {
    if (!initialized_) throw std::logic_error("DarkHouseApp::run called before initialize()");
    using Clock = std::chrono::steady_clock;
    const auto framePeriod = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / std::max(1.0, config_.targetFramesPerSecond)));

    auto previousStart = Clock::now();
    auto nextDeadline = previousStart;
    while (!quitRequested_.load()) {
        const auto frameStart = Clock::now();
        FrameContext frame;
        frame.frameIndex = summary_.frames;
        frame.deltaSeconds = std::chrono::duration<double>(frameStart - previousStart).count();
        previousStart = frameStart;

        if (!frontEnd_->pumpPlatformEvents(*this)) requestQuit();
        processEvents();
        pollImports();

        frame.mode = mode();
        if (showsCanvas(frame.mode) && developGraph_) {
            try {
                uploadDirtyCanvasTiles();
                renderFrame(frame);
            } catch (const std::exception& e) {
                logLine("error", "GPU frame failed, disabling canvas rendering: ", e.what());
                disableCanvas();
                frame.canvasOutput = nullptr;
            }
        }
        frame.canvasGeneration = canvasGeneration_;
        frontEnd_->drawFrame(*this, frame);
        ++summary_.frames;

        if (config_.maxFrames && summary_.frames >= *config_.maxFrames) break;
        if (config_.exitWhenIdle && !frontEnd_->interactive() && idle()) break;

        // Presentation already blocks on vsync; sleeping as well would halve
        // the frame rate whenever the two clocks drift apart.
        if (frontEnd_->pacesFrames()) {
            nextDeadline = Clock::now();
            continue;
        }

        // Fixed-rate pacing. After a stall, skip ahead instead of catching up.
        nextDeadline += framePeriod;
        const auto now = Clock::now();
        if (nextDeadline < now) {
            nextDeadline = now;
        } else {
            std::this_thread::sleep_until(nextDeadline);
        }
    }

    shutdown();
    logLine("info", "exit after ", summary_.frames, " frame(s); imports: ", summary_.importsSucceeded, " ok, ",
        summary_.importsFailed, " failed, ", summary_.importsCancelled, " cancelled");
    return summary_.importsFailed > 0 ? 2 : 0;
}

void DarkHouseApp::processEvents() {
    std::deque<AppEvent> batch;
    {
        std::lock_guard lock(eventMutex_);
        batch.swap(events_);
    }
    for (const AppEvent& event : batch) {
        std::visit([this](const auto& e) { handle(e); }, event);
    }
}

void DarkHouseApp::handle(const QuitEvent&) { requestQuit(); }

void DarkHouseApp::handle(const SwitchModeEvent& event) {
    if (event.mode == mode()) return;
    logLine("info", "mode: ", toString(mode()), " -> ", toString(event.mode));
    mode_.store(event.mode);
    graphDirty_ = true;  // re-render the canvas when it becomes visible again
}

void DarkHouseApp::handle(const ImportFilesEvent& event) {
    for (const std::string& path : event.absolutePaths) {
        try {
            pendingImports_.emplace_back(path, assets_->importFile(path));
        } catch (const std::exception& e) {
            ++summary_.importsFailed;
            logLine("error", "import rejected: ", path, ": ", e.what());
        }
    }
}

void DarkHouseApp::handle(const SetRatingEvent& event) {
    try {
        if (!assets_->updateAssetRating(event.assetId, event.rating)) {
            logLine("warn", "rating ignored, no asset with id ", event.assetId);
        }
        ++catalogRevision_;
    } catch (const std::exception& e) {
        logLine("error", "rating failed for ", event.assetId, ": ", e.what());
    }
}

void DarkHouseApp::handle(const SetFlagEvent& event) {
    try {
        if (!assets_->updateAssetFlag(event.assetId, event.flag)) {
            logLine("warn", "flag ignored, no asset with id ", event.assetId);
        }
        ++catalogRevision_;
    } catch (const std::exception& e) {
        logLine("error", "flag failed for ", event.assetId, ": ", e.what());
    }
}

void DarkHouseApp::handle(const SetColorLabelEvent& event) {
    try {
        if (!assets_->updateAssetColorLabel(event.assetId, event.label)) {
            logLine("warn", "colour label ignored, no asset with id ", event.assetId);
        }
        ++catalogRevision_;
    } catch (const std::exception& e) {
        logLine("error", "colour label failed for ", event.assetId, ": ", e.what());
    }
}

void DarkHouseApp::handle(const SetDevelopParamsEvent& event) {
    if (event.nodeIndex >= developStack_.size()) {
        logLine("warn", "develop parameters ignored, no node ", event.nodeIndex);
        return;
    }
    EditNodeRecord& record = developStack_[event.nodeIndex];
    try {
        // The develop graph is a linear chain built in stack order, so node
        // ids equal stack indices (see rebuildDevelopGraph).
        if (developGraph_ && event.nodeIndex < developGraph_->nodeCount()) {
            developGraph_->node(event.nodeIndex).updateUniforms(event.serializedParams);
            graphDirty_ = true;
        }
        record.serializedParams = event.serializedParams;
        if (event.persist && !activeAssetId_.empty()) assets_->saveEditStack(activeAssetId_, developStack_);
    } catch (const std::exception& e) {
        logLine("error", "develop parameters rejected for node ", event.nodeIndex, " (", record.nodeType, "): ", e.what());
    }
}

void DarkHouseApp::handle(const OpenAssetEvent& event) {
    try {
        const std::optional<AssetRecord> asset = assets_->getAsset(event.assetId);
        if (!asset) {
            logLine("warn", "open ignored, no asset with id ", event.assetId);
            return;
        }
        activeAssetId_ = asset->id;
        rebuildDevelopGraph(assets_->loadEditStack(asset->id));
        logLine("info", "opened ", asset->fileName, " [", asset->id, "]");
        if (mode() == AppMode::CATALOG) handle(SwitchModeEvent{AppMode::CANVAS});
    } catch (const std::exception& e) {
        logLine("error", "open failed for ", event.assetId, ": ", e.what());
    }
}

void DarkHouseApp::pollImports() {
    for (auto it = pendingImports_.begin(); it != pendingImports_.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        try {
            const AssetRecord record = it->second.get();
            ++summary_.importsSucceeded;
            ++catalogRevision_;
            std::ostringstream detail;
            if (record.width > 0) detail << ' ' << record.width << 'x' << record.height;
            if (record.metadata.cameraModel) detail << ", " << *record.metadata.cameraModel;
            if (record.metadata.iso) detail << ", ISO " << *record.metadata.iso;
            logLine("info", "imported ", record.fileName, detail.str(), " [", record.id, "]");
        } catch (const std::exception& e) {
            ++summary_.importsFailed;
            logLine("error", "import failed: ", it->first, ": ", e.what());
        }
        it = pendingImports_.erase(it);
    }
}

void DarkHouseApp::uploadDirtyCanvasTiles() {
    if (!gpu_) return;
    const std::vector<TileKey> dirty = takeDirtyTiles(*document_);
    if (dirty.empty()) return;

    // Composite on the CPU, convert to FP16 and upload in one submission.
    std::vector<std::vector<std::uint16_t>> texels;
    std::vector<VulkanContext::RegionUpload> uploads;
    texels.reserve(dirty.size());
    uploads.reserve(dirty.size());
    for (const TileKey& key : dirty) {
        const CompositedTile tile = compositeTileCPU(*document_, key, config_.canvasWidth, config_.canvasHeight);
        std::vector<std::uint16_t>& halves = texels.emplace_back(tile.rgba.size());
        std::transform(tile.rgba.begin(), tile.rgba.end(), halves.begin(), floatToHalf);

        VulkanContext::RegionUpload upload;
        upload.x = key.tx * TILE_SIZE;
        upload.y = key.ty * TILE_SIZE;
        upload.width = tile.width;
        upload.height = tile.height;
        upload.texels = std::as_bytes(std::span<const std::uint16_t>(halves));
        uploads.push_back(upload);
    }
    gpu_->uploadRegions(canvasTexture_, uploads);
    graphDirty_ = true;
}

void DarkHouseApp::renderFrame(FrameContext& frame) {
    if (!developGraph_ || developGraph_->empty()) return;
    if (graphDirty_) {
        gpu_->submitAndWait([this](VkCommandBuffer commandBuffer) { developGraph_->evaluateGraph(commandBuffer); });
        graphDirty_ = false;
    }
    const std::vector<RenderPipelineGraph::NodeId> sinks = developGraph_->sinkNodes();
    if (!sinks.empty()) frame.canvasOutput = &developGraph_->outputOf(sinks.back());
}

bool DarkHouseApp::idle() const {
    std::lock_guard lock(eventMutex_);
    return events_.empty() && pendingImports_.empty();
}

void DarkHouseApp::shutdown() noexcept {
    if (shutDown_) return;
    shutDown_ = true;
    // Destroying these futures does not block (they come from packaged_task).
    // AssetManager's destructor lets in-flight imports finish and drops the rest.
    summary_.importsCancelled += pendingImports_.size();
    pendingImports_.clear();
    segmentation_.reset();
    disableGpu();
    document_.reset();
    assets_.reset();
}

}  // namespace darkhouse
