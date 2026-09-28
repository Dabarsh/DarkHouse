// DarkHouse — AI-assisted selection.
//
// AISegmentationEngine is the backend-neutral interface the canvas uses for
// "Select Subject" / "Select Sky". The shipped backend runs ONNX models through
// ONNX Runtime (built when CMake finds it, see DARKHOUSE_ONNXRUNTIME). Masks
// come back at the source image's resolution, ready to go into a
// LayerNode mask.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace darkhouse {

enum class SegmentationTask : std::uint8_t { SUBJECT, SKY };

enum class OutputActivation : std::uint8_t {
    IDENTITY,  // the model already outputs probabilities
    SIGMOID,   // single-channel logits
    SOFTMAX,   // multi-class logits; foregroundChannel selects the class
};

// Borrowed view of a linear-light, straight-alpha RGBA32F image.
struct ImageBufferView {
    std::span<const float> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t rowStride = 0;  // in floats; 0 = tightly packed (width * 4)
};

struct SegmentationMask {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> alpha;  // width * height, 1 = selected
};

// How to feed a model and read its output. The defaults suit the usual
// ImageNet-normalized salient-object models (U^2-Net, BiRefNet, ...).
struct SegmentationModelConfig {
    std::uint32_t inputWidth = 1024;   // used only when the model's spatial dims are dynamic
    std::uint32_t inputHeight = 1024;
    std::array<float, 3> mean{0.485f, 0.456f, 0.406f};
    std::array<float, 3> stddev{0.229f, 0.224f, 0.225f};
    bool encodeSrgb = true;  // most models were trained on sRGB-encoded photos
    OutputActivation activation = OutputActivation::IDENTITY;
    std::uint32_t foregroundChannel = 0;  // output channel holding the selected class
    std::uint32_t intraOpThreads = 0;     // 0 = runtime default
};

class AISegmentationEngine {
public:
    virtual ~AISegmentationEngine() = default;

    // Loads (or replaces) the model for `task`. Throws on a missing file,
    // unsupported input layout or runtime error.
    virtual void loadModel(SegmentationTask task, const std::filesystem::path& modelPath,
                           const SegmentationModelConfig& config) = 0;
    [[nodiscard]] virtual bool isModelLoaded(SegmentationTask task) const = 0;

    // Throws std::logic_error if the task's model is not loaded. Safe to call
    // from several threads at once.
    [[nodiscard]] virtual SegmentationMask segmentSubject(const ImageBufferView& image) = 0;
    [[nodiscard]] virtual SegmentationMask segmentSky(const ImageBufferView& image) = 0;

    [[nodiscard]] virtual std::string_view backendName() const noexcept = 0;
};

// Returns the ONNX Runtime engine, or nullptr when DarkHouse was built without it.
[[nodiscard]] std::unique_ptr<AISegmentationEngine> createSegmentationEngine();

// Backend-independent pre- and post-processing, exposed for tests and for
// other backends.
namespace segmentation_detail {

// Bilinearly resamples `image` to dstWidth x dstHeight and packs it as a
// 1x3xHxW planar tensor, with optional sRGB encoding and (x - mean) / stddev.
[[nodiscard]] std::vector<float> prepareInputTensor(const ImageBufferView& image, std::uint32_t dstWidth,
                                                    std::uint32_t dstHeight, const SegmentationModelConfig& config);

// Turns a CxHxW output tensor into a [0, 1] mask at targetWidth x targetHeight.
[[nodiscard]] SegmentationMask decodeMaskTensor(std::span<const float> output, std::uint32_t channels,
                                                std::uint32_t outputWidth, std::uint32_t outputHeight,
                                                std::uint32_t targetWidth, std::uint32_t targetHeight,
                                                const SegmentationModelConfig& config);

}  // namespace segmentation_detail
}  // namespace darkhouse
