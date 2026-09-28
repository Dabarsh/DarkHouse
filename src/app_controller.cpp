#include "app_controller.hpp"

#include "parallel.hpp"

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

std::unique_ptr<LayerNode> emptyDocument(std::uint32_t width, std::uint32_t height) {
    std::unique_ptr<LayerNode> document = LayerNode::createGroup("Document");
    document->addChild(LayerNode::createRaster("Background", width, height));
    return document;
}

template <class T>
bool isReady(const std::future<T>& future) {
    return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
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
    canvasWidth_ = config_.canvasWidth;
    canvasHeight_ = config_.canvasHeight;
    document_ = emptyDocument(canvasWidth_, canvasHeight_);

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
        createCanvasTexture();
        developGraph_ = std::make_unique<RenderPipelineGraph>();
        rebuildDevelopGraph({});
        logLine("info", "canvas ", canvasWidth_, "x", canvasHeight_, " RGBA16F");
    } catch (const std::exception& e) {
        logLine("warn", "canvas rendering disabled: ", e.what());
        disableCanvas();
    }
}

void DarkHouseApp::createCanvasTexture() {
    canvasTexture_ = gpu_->createTexture(canvasWidth_, canvasHeight_, PixelFormat::R16G16B16A16_SFLOAT,
                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    gpu_->clearTexture(canvasTexture_, 0.0f, 0.0f, 0.0f, 0.0f);
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
    // -> display transform. An empty stack still gets an identity exposure
    // node. Node ids equal stack indices; the display node comes last.
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
    const RenderPipelineGraph::NodeId display =
        developGraph_->addNode(std::make_unique<DisplayTransformNode>(*gpu_, config_.shaderDirectory));
    developGraph_->connectNodes(*previous, display, 0);
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
        pollPhotoLoad();

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
        startPhotoLoad(*asset);
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
            sessionImports_.push_back(record.id);
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

DarkHouseApp::LoadedPhoto DarkHouseApp::loadPhoto(const std::filesystem::path& path, DecodeOptions options) {
    const auto start = std::chrono::steady_clock::now();
    const DecodedImage image = decodeImage(path, options);
    LoadedPhoto photo;
    photo.document = LayerNode::createGroup("Document");
    LayerNode& background = photo.document->addChild(LayerNode::createRaster("Background", image.width, image.height));
    background.raster()->writeRegion(0, 0, image.width, image.height, image.rgba);
    photo.width = image.width;
    photo.height = image.height;
    photo.format = image.format;
    const bool quarterTurn = image.orientation >= 5;  // EXIF 5..8 swap the axes
    photo.sourceWidth = quarterTurn ? image.sourceHeight : image.sourceWidth;
    photo.sourceHeight = quarterTurn ? image.sourceWidth : image.sourceHeight;
    photo.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return photo;
}

void DarkHouseApp::startPhotoLoad(const AssetRecord& asset) {
    std::future<LoadedPhoto> load = std::async(std::launch::async, &DarkHouseApp::loadPhoto,
                                               std::filesystem::path(asset.filePath),
                                               DecodeOptions{config_.previewMaxDimension});
    if (photoLoad_.valid()) abandonedPhotoLoads_.push_back(std::move(photoLoad_));
    photoLoad_ = std::move(load);
    photo_ = PhotoStatus{};
    photo_.state = PhotoStatus::State::LOADING;
    photo_.assetId = asset.id;
    photo_.fileName = asset.fileName;
}

void DarkHouseApp::pollPhotoLoad() {
    std::erase_if(abandonedPhotoLoads_, [](const std::future<LoadedPhoto>& load) { return isReady(load); });
    if (!isReady(photoLoad_)) return;
    try {
        LoadedPhoto loaded = photoLoad_.get();
        photo_.state = PhotoStatus::State::READY;
        photo_.format = loaded.format;
        photo_.sourceWidth = loaded.sourceWidth;
        photo_.sourceHeight = loaded.sourceHeight;
        photo_.decodeSeconds = loaded.seconds;
        std::ostringstream scaled;
        if (loaded.width != loaded.sourceWidth || loaded.height != loaded.sourceHeight) {
            scaled << " -> " << loaded.width << 'x' << loaded.height << " preview";
        }
        logLine("info", "photo ", photo_.fileName, ": ", loaded.format, ' ', loaded.sourceWidth, 'x',
            loaded.sourceHeight, scaled.str(), " in ", static_cast<int>(loaded.seconds * 1000.0 + 0.5), " ms");
        replaceDocument(std::move(loaded.document), loaded.width, loaded.height);
    } catch (const std::exception& e) {
        // A photo that cannot be shown must not leave the previous one on screen.
        photo_.state = PhotoStatus::State::FAILED;
        photo_.error = e.what();
        const std::string prefix = photo_.fileName + ": ";  // decoder messages name the file; so does the UI
        if (photo_.error.starts_with(prefix)) photo_.error.erase(0, prefix.size());
        logLine("error", "cannot display ", photo_.fileName, ": ", photo_.error);
        replaceDocument(emptyDocument(canvasWidth_, canvasHeight_), canvasWidth_, canvasHeight_);
    }
}

void DarkHouseApp::replaceDocument(std::unique_ptr<LayerNode> document, std::uint32_t width, std::uint32_t height) {
    document_ = std::move(document);
    const bool resized = width != canvasWidth_ || height != canvasHeight_;
    canvasWidth_ = width;
    canvasHeight_ = height;
    if (!gpu_ || !developGraph_) return;
    try {
        if (resized) {
            // The graph samples the canvas and in-flight UI frames sample the
            // graph's outputs; both are recreated at the new size.
            gpu_->waitIdle();
            gpu_->destroyTexture(canvasTexture_);
            createCanvasTexture();
            rebuildDevelopGraph(developStack_);
        } else {
            // Tiles the new document never wrote must read as transparent,
            // not as the previous document.
            gpu_->clearTexture(canvasTexture_, 0.0f, 0.0f, 0.0f, 0.0f);
            graphDirty_ = true;
        }
    } catch (const std::exception& e) {
        logLine("error", "canvas ", width, "x", height, " could not be created, disabling canvas rendering: ", e.what());
        disableCanvas();
    }
}

void DarkHouseApp::uploadDirtyCanvasTiles() {
    if (!gpu_) return;
    std::vector<TileKey> dirty = takeDirtyTiles(*document_);
    // A layer larger than the canvas can report tiles the canvas does not have.
    const std::uint32_t tilesX = (canvasWidth_ + TILE_SIZE - 1) / TILE_SIZE;
    const std::uint32_t tilesY = (canvasHeight_ + TILE_SIZE - 1) / TILE_SIZE;
    std::erase_if(dirty, [&](const TileKey& key) { return key.tx >= tilesX || key.ty >= tilesY; });
    if (dirty.empty()) return;

    // Composite to FP16 on all cores (tiles are independent and the document
    // is only read), then upload everything in one submission.
    std::vector<CompositedTileHalf> tiles(dirty.size());
    detail::parallelFor(dirty.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) tiles[i] = compositeTileHalf(*document_, dirty[i], canvasWidth_, canvasHeight_);
    });
    std::vector<VulkanContext::RegionUpload> uploads;
    uploads.reserve(dirty.size());
    for (std::size_t i = 0; i < dirty.size(); ++i) {
        VulkanContext::RegionUpload upload;
        upload.x = dirty[i].tx * TILE_SIZE;
        upload.y = dirty[i].ty * TILE_SIZE;
        upload.width = tiles[i].width;
        upload.height = tiles[i].height;
        upload.texels = std::as_bytes(std::span<const std::uint16_t>(tiles[i].rgba));
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
    return events_.empty() && pendingImports_.empty() && !photoLoad_.valid();
}

void DarkHouseApp::shutdown() noexcept {
    if (shutDown_) return;
    shutDown_ = true;
    // Destroying these futures does not block (they come from packaged_task).
    // AssetManager's destructor lets in-flight imports finish and drops the rest.
    summary_.importsCancelled += pendingImports_.size();
    pendingImports_.clear();
    // These do block, until the decodes in flight finish.
    photoLoad_ = {};
    abandonedPhotoLoads_.clear();
    segmentation_.reset();
    disableGpu();
    document_.reset();
    assets_.reset();
}

}  // namespace darkhouse
