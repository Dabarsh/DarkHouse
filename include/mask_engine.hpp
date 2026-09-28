// DarkHouse — masks for local (selective) adjustments.
//
// A mask is a stack of components, each a shape that selects part of the
// image, combined in order:
//
//   BRUSH             painted dabs (add or erase), rasterized on the CPU and
//                     uploaded as dirty rectangles
//   LINEAR_GRADIENT   full effect at `start`, fading to none at `end`
//   RADIAL_GRADIENT   an ellipse (centre `start`, radii `size`, rotated by
//                     `angle`) with a feathered edge
//   LUMINANCE_RANGE   Oklab lightness in [low, high] with a soft falloff
//   COLOR_RANGE       hues within `hueWidth` of `hue`, chroma above `low`
//   SUBJECT, SKY      placeholders until the AI segmentation models are wired
//                     in: a centre-weighted ellipse / a blue-or-bright,
//                     upper-frame colour heuristic
//
// Each component can be inverted and has an opacity; ADD unions it with the
// mask so far (screen), SUBTRACT removes it, INTERSECT keeps only the overlap.
// Positions are normalized to the image (0..1 across width and height) and
// brush radii to its long edge, so masks survive the preview downscale and a
// later full-resolution render.
//
// A LocalAdjustment pairs a mask with the edits it applies (exposure,
// contrast, highlights, shadows, temperature, tint, saturation). The GPU
// side (MaskEngine) generates every mask's alpha into RGBA16F array texture
// channels (mask i in layer i / 4, channel i % 4), and LocalAdjustNode
// blends each adjustment in with it:
//
//     colour = mix(colour, adjusted(colour), maskAlpha * amount)
//
// The CPU functions here are the reference the GPU output is tested against,
// and rasterize masks for raster layers in the canvas (layer masks).
#pragma once

#include "color_adjust.hpp"
#include "layer_stack.hpp"
#include "vulkan_context.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace darkhouse {

enum class MaskShape : std::uint32_t {
    BRUSH,
    LINEAR_GRADIENT,
    RADIAL_GRADIENT,
    LUMINANCE_RANGE,
    COLOR_RANGE,
    SUBJECT,
    SKY,
};
inline constexpr std::size_t kMaskShapeCount = 7;
[[nodiscard]] const char* toString(MaskShape shape) noexcept;

enum class MaskMode : std::uint32_t { ADD, SUBTRACT, INTERSECT };
[[nodiscard]] const char* toString(MaskMode mode) noexcept;

struct BrushDab {
    float x = 0.5f;          // image position, 0..1 across the width
    float y = 0.5f;          // 0..1 down the height
    float radius = 0.05f;    // fraction of the image's long edge
    float feather = 0.5f;    // 0 = hard edge .. 1 = soft from the centre
    float flow = 1.0f;       // strength per dab, 0..1
    bool erase = false;

    bool operator==(const BrushDab&) const = default;
};

struct MaskComponent {
    MaskShape shape = MaskShape::LINEAR_GRADIENT;
    MaskMode mode = MaskMode::ADD;
    bool invert = false;
    float opacity = 1.0f;  // 0..1

    // Gradients (normalized image coordinates).
    std::array<float, 2> start{0.5f, 0.2f};  // linear: full effect; radial: centre
    std::array<float, 2> end{0.5f, 0.6f};    // linear: no effect
    std::array<float, 2> size{0.3f, 0.3f};   // radial radii, fractions of width / height
    float angle = 0.0f;                      // radial rotation, degrees
    float feather = 0.5f;                    // radial edge softness, 0..1

    // Ranges: luminance selects Oklab lightness in [low, high]; colour selects
    // hue +- hueWidth / 2 with chroma above `low`.
    float low = 0.0f;
    float high = 0.3f;
    float falloff = 0.1f;  // soft edge (lightness units; x 90 degrees for hue)
    float hue = 30.0f;
    float hueWidth = 60.0f;

    std::vector<BrushDab> dabs;  // BRUSH
};

// Local edits; each in [-100, 100] except exposure (stops, [-4, 4]).
struct LocalAdjustParams {
    float exposure = 0.0f;
    float contrast = 0.0f;
    float highlights = 0.0f;
    float shadows = 0.0f;
    float temperature = 0.0f;  // +100 warms by about 0.6 stop of colour temperature
    float tint = 0.0f;         // +100 towards magenta
    float saturation = 0.0f;

    bool operator==(const LocalAdjustParams&) const = default;
};

struct LocalAdjustment {
    std::string name = "Mask";
    bool enabled = true;
    bool invert = false;   // inverts the combined mask
    float amount = 1.0f;   // opacity of the whole adjustment, 0..1
    std::vector<MaskComponent> components;
    LocalAdjustParams params;
};

// The parameters of a local_adjust develop node: every mask of the photo.
struct LocalAdjustments {
    std::vector<LocalAdjustment> masks;
};

inline constexpr std::size_t kMaxMasks = 16;
inline constexpr std::size_t kMaxComponentsPerMask = 16;

// Versioned little-endian binary form (edit_nodes.serialized_params).
[[nodiscard]] std::vector<std::byte> serialize(const LocalAdjustments& adjustments);
// Throws std::invalid_argument for malformed or truncated data. An empty
// span yields no masks.
[[nodiscard]] LocalAdjustments deserializeLocalAdjustments(std::span<const std::byte> bytes);

// Clamps every value into range, drops components and masks over the limits.
[[nodiscard]] LocalAdjustments sanitize(const LocalAdjustments& adjustments);

// --- CPU evaluation -----------------------------------------------------------------------

// Rasterizes brush dabs into a width x height coverage buffer (0..1), in order:
// painting builds up towards 1, erasing scales down.
class BrushRaster {
public:
    void reset(std::uint32_t width, std::uint32_t height);
    // Paints the dabs not painted yet when `dabs` extends what was painted;
    // otherwise repaints from scratch. Returns the changed pixel rectangle
    // {x0, y0, x1, y1} (exclusive), empty when nothing changed.
    std::array<std::uint32_t, 4> sync(std::span<const BrushDab> dabs);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<float>& coverage() const noexcept { return coverage_; }
    [[nodiscard]] float at(std::uint32_t x, std::uint32_t y) const noexcept { return coverage_[std::size_t{y} * width_ + x]; }

private:
    std::array<std::uint32_t, 4> paint(const BrushDab& dab);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<float> coverage_;
    std::vector<BrushDab> painted_;
};

// Alpha (0..1) of one component at pixel (x, y) of a width x height image
// whose colour there is `rgb`; `brush` is the component's coverage for BRUSH.
[[nodiscard]] float componentAlpha(const MaskComponent& component, std::uint32_t x, std::uint32_t y,
                                   std::uint32_t width, std::uint32_t height, const Rgb& rgb, float brush) noexcept;

// Combines one component's alpha into the running mask value.
[[nodiscard]] float combineMask(float mask, const MaskComponent& component, float alpha) noexcept;

// The whole mask of `adjustment` (without its amount), width * height values.
// `rgba` is the source image (linear RGBA floats) the ranges are measured on.
[[nodiscard]] std::vector<float> evaluateMask(const LocalAdjustment& adjustment, std::uint32_t width,
                                              std::uint32_t height, std::span<const float> rgba);

// The edits of one adjustment applied to a colour (no mask).
[[nodiscard]] Rgb applyLocalAdjust(const LocalAdjustParams& params, const Rgb& rgb) noexcept;

// Canvas layers: writes the mask of `adjustment` (evaluated against
// `sourceRgba`, width x height linear RGBA) into a one-channel layer mask of
// the same size, replacing its contents. The same shapes serve develop masks
// and compositing layer masks.
void writeLayerMask(const LocalAdjustment& adjustment, SparseRasterLayer& layerMask, std::span<const float> sourceRgba);

// --- GPU ------------------------------------------------------------------------------------

// Generates the alpha of every mask on the GPU for LocalAdjustNode: brush
// coverage lives in `brushTexture` (component i in layer i / 4, channel
// i % 4) and is uploaded incrementally; gradients and ranges are evaluated
// by shaders/mask_generate.comp against the node's input image.
class MaskEngine {
public:
    MaskEngine(const VulkanContext& context, const std::filesystem::path& shaderDirectory);
    ~MaskEngine();

    MaskEngine(const MaskEngine&) = delete;
    MaskEngine& operator=(const MaskEngine&) = delete;

    // Prepares textures for a width x height source and the given masks:
    // reallocates when the size or the number of masks / brushes grows past
    // what the textures hold (waiting for the device first), and paints and
    // uploads new brush dabs. Call before record().
    void prepare(const LocalAdjustments& adjustments, std::uint32_t width, std::uint32_t height);

    // Records the mask generation from `source` into maskTexture().
    void record(VkCommandBuffer commandBuffer, const GPUTexture& source);

    [[nodiscard]] GPUTexture& maskTexture() noexcept { return masks_; }
    [[nodiscard]] std::uint32_t maskCount() const noexcept { return maskCount_; }

private:
    void destroy() noexcept;
    void writeDescriptors(const GPUTexture& source);

    const VulkanContext& context_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;

    GPUTexture brushes_;  // RGBA16F array: brush coverage, 4 components per layer
    GPUTexture masks_;    // RGBA16F array: mask alpha, 4 masks per layer
    GPUBuffer components_;
    GPUBuffer maskRecords_;
    VkImageView boundSource_ = VK_NULL_HANDLE;
    bool descriptorsDirty_ = true;

    std::vector<BrushRaster> brushRasters_;       // one per BRUSH component, in mask order
    std::vector<std::byte> componentData_;        // shader records, uploaded with vkCmdUpdateBuffer
    std::vector<std::byte> maskData_;
    std::uint32_t maskCount_ = 0;
};

}  // namespace darkhouse
