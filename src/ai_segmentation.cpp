#include "ai_segmentation.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#if defined(DARKHOUSE_WITH_ONNXRUNTIME)
#include <onnxruntime_cxx_api.h>

#include <mutex>
#include <shared_mutex>
#endif

namespace darkhouse {
namespace segmentation_detail {
namespace {

float linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Maps destination sample d to a source coordinate with pixel centres aligned,
// then clamps to the valid range.
struct Tap {
    std::uint32_t i0;
    std::uint32_t i1;
    float t;
};

Tap bilinearTap(std::uint32_t d, std::uint32_t dstSize, std::uint32_t srcSize) {
    const float s = std::clamp((static_cast<float>(d) + 0.5f) * static_cast<float>(srcSize) / static_cast<float>(dstSize) - 0.5f,
                               0.0f, static_cast<float>(srcSize - 1));
    const auto i0 = static_cast<std::uint32_t>(s);
    return {i0, std::min(i0 + 1, srcSize - 1), s - static_cast<float>(i0)};
}

float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

}  // namespace

std::vector<float> prepareInputTensor(const ImageBufferView& image, std::uint32_t dstWidth, std::uint32_t dstHeight,
                                      const SegmentationModelConfig& config) {
    if (image.width == 0 || image.height == 0 || dstWidth == 0 || dstHeight == 0) {
        throw std::invalid_argument("prepareInputTensor: empty image or tensor");
    }
    const std::size_t stride = image.rowStride ? image.rowStride : std::size_t{image.width} * 4;
    if (stride < std::size_t{image.width} * 4 ||
        image.pixels.size() < (std::size_t{image.height} - 1) * stride + std::size_t{image.width} * 4) {
        throw std::invalid_argument("prepareInputTensor: pixel buffer is smaller than width/height/stride imply");
    }

    const std::size_t plane = std::size_t{dstWidth} * dstHeight;
    std::vector<float> tensor(plane * 3);
    for (std::uint32_t y = 0; y < dstHeight; ++y) {
        const Tap ty = bilinearTap(y, dstHeight, image.height);
        const float* row0 = image.pixels.data() + ty.i0 * stride;
        const float* row1 = image.pixels.data() + ty.i1 * stride;
        for (std::uint32_t x = 0; x < dstWidth; ++x) {
            const Tap tx = bilinearTap(x, dstWidth, image.width);
            for (std::uint32_t c = 0; c < 3; ++c) {
                const float top = row0[tx.i0 * 4 + c] + (row0[tx.i1 * 4 + c] - row0[tx.i0 * 4 + c]) * tx.t;
                const float bottom = row1[tx.i0 * 4 + c] + (row1[tx.i1 * 4 + c] - row1[tx.i0 * 4 + c]) * tx.t;
                float value = top + (bottom - top) * ty.t;
                value = config.encodeSrgb ? linearToSrgb(value) : std::clamp(value, 0.0f, 1.0f);
                tensor[c * plane + std::size_t{y} * dstWidth + x] = (value - config.mean[c]) / config.stddev[c];
            }
        }
    }
    return tensor;
}

SegmentationMask decodeMaskTensor(std::span<const float> output, std::uint32_t channels, std::uint32_t outputWidth,
                                  std::uint32_t outputHeight, std::uint32_t targetWidth, std::uint32_t targetHeight,
                                  const SegmentationModelConfig& config) {
    const std::size_t plane = std::size_t{outputWidth} * outputHeight;
    if (channels == 0 || plane == 0 || output.size() < plane * channels) {
        throw std::invalid_argument("decodeMaskTensor: output tensor is smaller than its declared shape");
    }
    if (config.foregroundChannel >= channels) {
        throw std::invalid_argument("decodeMaskTensor: foregroundChannel is out of range for this model");
    }
    if (targetWidth == 0 || targetHeight == 0) throw std::invalid_argument("decodeMaskTensor: empty target");

    // 1. Probability of the foreground class at model resolution.
    std::vector<float> probability(plane);
    for (std::size_t i = 0; i < plane; ++i) {
        const float fg = output[config.foregroundChannel * plane + i];
        switch (config.activation) {
        case OutputActivation::IDENTITY:
            probability[i] = fg;
            break;
        case OutputActivation::SIGMOID:
            probability[i] = sigmoid(fg);
            break;
        case OutputActivation::SOFTMAX: {
            float maxLogit = output[i];
            for (std::uint32_t c = 1; c < channels; ++c) maxLogit = std::max(maxLogit, output[c * plane + i]);
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < channels; ++c) sum += std::exp(output[c * plane + i] - maxLogit);
            probability[i] = std::exp(fg - maxLogit) / sum;
            break;
        }
        }
    }

    // 2. Bilinear upsample to the source resolution.
    SegmentationMask mask;
    mask.width = targetWidth;
    mask.height = targetHeight;
    mask.alpha.resize(std::size_t{targetWidth} * targetHeight);
    for (std::uint32_t y = 0; y < targetHeight; ++y) {
        const Tap ty = bilinearTap(y, targetHeight, outputHeight);
        for (std::uint32_t x = 0; x < targetWidth; ++x) {
            const Tap tx = bilinearTap(x, targetWidth, outputWidth);
            const float p00 = probability[std::size_t{ty.i0} * outputWidth + tx.i0];
            const float p01 = probability[std::size_t{ty.i0} * outputWidth + tx.i1];
            const float p10 = probability[std::size_t{ty.i1} * outputWidth + tx.i0];
            const float p11 = probability[std::size_t{ty.i1} * outputWidth + tx.i1];
            const float top = p00 + (p01 - p00) * tx.t;
            const float bottom = p10 + (p11 - p10) * tx.t;
            const float value = top + (bottom - top) * ty.t;
            mask.alpha[std::size_t{y} * targetWidth + x] = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
        }
    }
    return mask;
}

}  // namespace segmentation_detail

#if defined(DARKHOUSE_WITH_ONNXRUNTIME)
namespace {

const char* taskName(SegmentationTask task) { return task == SegmentationTask::SUBJECT ? "subject" : "sky"; }

class OnnxSegmentationEngine final : public AISegmentationEngine {
public:
    OnnxSegmentationEngine() : env_(ORT_LOGGING_LEVEL_WARNING, "DarkHouse") {}

    void loadModel(SegmentationTask task, const std::filesystem::path& modelPath,
                   const SegmentationModelConfig& config) override {
        if (!std::filesystem::is_regular_file(modelPath)) {
            throw std::runtime_error("segmentation model not found: '" + modelPath.string() + "'");
        }
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (config.intraOpThreads > 0) options.SetIntraOpNumThreads(static_cast<int>(config.intraOpThreads));

        auto model = std::make_unique<LoadedModel>();
        // path::c_str() is wchar_t on Windows, matching ORTCHAR_T there.
        model->session = std::make_unique<Ort::Session>(env_, modelPath.c_str(), options);
        if (model->session->GetInputCount() < 1 || model->session->GetOutputCount() < 1) {
            throw std::runtime_error("segmentation model must have at least one input and one output");
        }

        Ort::AllocatorWithDefaultOptions allocator;
        model->inputName = model->session->GetInputNameAllocated(0, allocator).get();
        model->outputName = model->session->GetOutputNameAllocated(0, allocator).get();

        Ort::TypeInfo inputType = model->session->GetInputTypeInfo(0);
        auto inputInfo = inputType.GetTensorTypeAndShapeInfo();
        if (inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error("segmentation model input must be float32");
        }
        std::vector<std::int64_t> shape = inputInfo.GetShape();
        if (shape.size() != 4 || (shape[1] != 3 && shape[1] > 0)) {
            throw std::runtime_error("segmentation model input must be NCHW with 3 channels");
        }
        shape[0] = 1;
        shape[1] = 3;
        if (shape[2] <= 0) shape[2] = config.inputHeight;
        if (shape[3] <= 0) shape[3] = config.inputWidth;
        model->inputShape = std::move(shape);
        model->config = config;

        std::unique_lock lock(mutex_);
        models_[index(task)] = std::move(model);
    }

    [[nodiscard]] bool isModelLoaded(SegmentationTask task) const override {
        std::shared_lock lock(mutex_);
        return models_[index(task)] != nullptr;
    }

    [[nodiscard]] SegmentationMask segmentSubject(const ImageBufferView& image) override {
        return run(SegmentationTask::SUBJECT, image);
    }
    [[nodiscard]] SegmentationMask segmentSky(const ImageBufferView& image) override {
        return run(SegmentationTask::SKY, image);
    }
    [[nodiscard]] std::string_view backendName() const noexcept override { return "ONNX Runtime"; }

private:
    struct LoadedModel {
        std::unique_ptr<Ort::Session> session;
        std::string inputName;
        std::string outputName;
        std::vector<std::int64_t> inputShape;  // resolved: 1 x 3 x H x W
        SegmentationModelConfig config;
    };

    static std::size_t index(SegmentationTask task) { return static_cast<std::size_t>(task); }

    SegmentationMask run(SegmentationTask task, const ImageBufferView& image) {
        std::shared_lock lock(mutex_);  // Ort::Session::Run is safe to call concurrently
        const LoadedModel* model = models_[index(task)].get();
        if (!model) throw std::logic_error(std::string(taskName(task)) + " segmentation model is not loaded");

        const auto inputHeight = static_cast<std::uint32_t>(model->inputShape[2]);
        const auto inputWidth = static_cast<std::uint32_t>(model->inputShape[3]);
        std::vector<float> input =
            segmentation_detail::prepareInputTensor(image, inputWidth, inputHeight, model->config);

        const Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(memoryInfo, input.data(), input.size(),
                                                                 model->inputShape.data(), model->inputShape.size());
        const char* inputNames[] = {model->inputName.c_str()};
        const char* outputNames[] = {model->outputName.c_str()};
        std::vector<Ort::Value> outputs =
            model->session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, 1);

        auto outputInfo = outputs.front().GetTensorTypeAndShapeInfo();
        if (outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error("segmentation model output must be float32");
        }
        // Accept [1, C, H, W], [1, H, W] or [H, W].
        const std::vector<std::int64_t> shape = outputInfo.GetShape();
        std::int64_t channels = 1, height = 0, width = 0;
        switch (shape.size()) {
        case 4: channels = shape[1]; height = shape[2]; width = shape[3]; break;
        case 3: height = shape[1]; width = shape[2]; break;
        case 2: height = shape[0]; width = shape[1]; break;
        default: throw std::runtime_error("unsupported segmentation output rank " + std::to_string(shape.size()));
        }
        if (channels <= 0 || height <= 0 || width <= 0) throw std::runtime_error("invalid segmentation output shape");

        const float* data = outputs.front().GetTensorData<float>();
        return segmentation_detail::decodeMaskTensor(
            std::span<const float>(data, outputInfo.GetElementCount()), static_cast<std::uint32_t>(channels),
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), image.width, image.height,
            model->config);
    }

    Ort::Env env_;  // declared first: must outlive every session
    mutable std::shared_mutex mutex_;
    std::array<std::unique_ptr<LoadedModel>, 2> models_;
};

}  // namespace
#endif  // DARKHOUSE_WITH_ONNXRUNTIME

std::unique_ptr<AISegmentationEngine> createSegmentationEngine() {
#if defined(DARKHOUSE_WITH_ONNXRUNTIME)
    return std::make_unique<OnnxSegmentationEngine>();
#else
    return nullptr;
#endif
}

}  // namespace darkhouse
