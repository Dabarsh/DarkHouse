#include "mask_engine.hpp"

#include "vulkan_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace darkhouse {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float smoothstep(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float clampFinite(float value, float lo, float hi, float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}

float wrapDegrees(float degrees) noexcept {
    if (!std::isfinite(degrees)) return 0.0f;
    degrees = std::fmod(degrees, 360.0f);
    return degrees < 0.0f ? degrees + 360.0f : degrees;
}

float hueDistance(float a, float b) noexcept {
    // GLSL mod(): x - y * floor(x / y), always non-negative for y > 0.
    const float x = a - b + 540.0f;
    return std::fabs(x - 360.0f * std::floor(x / 360.0f) - 180.0f);
}

// --- Serialization -------------------------------------------------------------------------

constexpr std::uint32_t kMagic = 0x4B4D4844u;  // "DHMK" little-endian
constexpr std::uint32_t kVersion = 1;

class Writer {
public:
    template <class T>
    void put(const T& value) {
        const auto* p = reinterpret_cast<const std::byte*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    void putBool(bool value) { put(static_cast<std::uint32_t>(value ? 1 : 0)); }
    void putString(const std::string& text) {
        put(static_cast<std::uint32_t>(text.size()));
        const auto* p = reinterpret_cast<const std::byte*>(text.data());
        bytes.insert(bytes.end(), p, p + text.size());
    }
    std::vector<std::byte> bytes;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}
    template <class T>
    T get() {
        if (data_.size() - offset_ < sizeof(T)) throw std::invalid_argument("local adjustments: data is truncated");
        T value;
        std::memcpy(&value, data_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }
    bool getBool() { return get<std::uint32_t>() != 0; }
    std::string getString() {
        const auto size = get<std::uint32_t>();
        if (size > 4096 || data_.size() - offset_ < size) throw std::invalid_argument("local adjustments: bad name");
        std::string text(reinterpret_cast<const char*>(data_.data() + offset_), size);
        offset_ += size;
        return text;
    }
    std::uint32_t count(std::uint32_t limit, const char* what) {
        const auto value = get<std::uint32_t>();
        if (value > limit) throw std::invalid_argument(std::string("local adjustments: too many ") + what);
        return value;
    }
    [[nodiscard]] bool done() const noexcept { return offset_ == data_.size(); }

private:
    std::span<const std::byte> data_;
    std::size_t offset_ = 0;
};

// --- Tone (mirror of shaders/tone_common.glsl) --------------------------------------------------

Rgb applyTone(Rgb rgb, float exposureEV, float highlights, float shadows, float contrast) noexcept {
    constexpr float kMiddleGrey = 0.18f, kToneRange = 2.5f, kMaxToneStops = 2.0f, kHalfMax = 65504.0f;
    for (float& c : rgb) c = std::max(c, 0.0f);
    const float gain = std::exp2(exposureEV);
    for (float& c : rgb) c *= gain;
    const float luma = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    const float stopsFromGrey = std::log2(std::max(luma, 1e-6f) / kMiddleGrey);
    const float highlightMask = smoothstep(0.0f, kToneRange, stopsFromGrey);
    const float shadowMask = smoothstep(0.0f, kToneRange, -stopsFromGrey);
    const float toneGain = std::exp2(kMaxToneStops * (highlights * highlightMask + shadows * shadowMask));
    for (float& c : rgb) c *= toneGain;
    const float slope = std::exp2(contrast);
    for (float& c : rgb) c = std::min(kMiddleGrey * std::pow(std::max(c / kMiddleGrey, 1e-8f), slope), kHalfMax);
    return rgb;
}

// Local temperature / tint as a white-balance matrix: +100 temperature is
// the correction a ~0.6 stop warmer illuminant setting would make.
WhiteBalancePush localWhiteBalance(const LocalAdjustParams& params) noexcept {
    return whiteBalancePush({kReferenceTemperature * std::exp2(0.6f * params.temperature / 100.0f), 1.5f * params.tint});
}

// --- GPU records (std430; layouts in shaders/mask_common.glsl and local_adjust.comp) ----------

struct MaskComponentGpu {
    std::uint32_t shape;
    std::uint32_t mode;
    std::uint32_t invert;
    std::uint32_t brushSlot;
    float opacity;
    float angle;
    float feather;
    float falloff;
    float startEnd[4];
    float sizeRange[4];
    float hueParams[4];
};
static_assert(sizeof(MaskComponentGpu) == 80);

struct MaskRecordGpu {
    std::uint32_t firstComponent;
    std::uint32_t componentCount;
    std::uint32_t invert;
    std::uint32_t reserved;
};
static_assert(sizeof(MaskRecordGpu) == 16);

constexpr VkImageUsageFlags kMaskUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

std::uint32_t layersFor(std::size_t channels) { return static_cast<std::uint32_t>(std::max<std::size_t>(1, (channels + 3) / 4)); }

std::uint32_t groups(std::uint32_t size) { return (size + 15) / 16; }

}  // namespace

// -----------------------------------------------------------------------------
// Names
// -----------------------------------------------------------------------------

const char* toString(MaskShape shape) noexcept {
    switch (shape) {
    case MaskShape::BRUSH: return "Brush";
    case MaskShape::LINEAR_GRADIENT: return "Linear Gradient";
    case MaskShape::RADIAL_GRADIENT: return "Radial Gradient";
    case MaskShape::LUMINANCE_RANGE: return "Luminance Range";
    case MaskShape::COLOR_RANGE: return "Color Range";
    case MaskShape::SUBJECT: return "Subject";
    case MaskShape::SKY: return "Sky";
    }
    return "?";
}

const char* toString(MaskMode mode) noexcept {
    switch (mode) {
    case MaskMode::ADD: return "Add";
    case MaskMode::SUBTRACT: return "Subtract";
    case MaskMode::INTERSECT: return "Intersect";
    }
    return "?";
}

// -----------------------------------------------------------------------------
// Serialization and sanitizing
// -----------------------------------------------------------------------------

std::vector<std::byte> serialize(const LocalAdjustments& adjustments) {
    Writer w;
    w.put(kMagic);
    w.put(kVersion);
    w.put(static_cast<std::uint32_t>(adjustments.masks.size()));
    for (const LocalAdjustment& mask : adjustments.masks) {
        w.putString(mask.name);
        w.putBool(mask.enabled);
        w.putBool(mask.invert);
        w.put(mask.amount);
        const LocalAdjustParams& p = mask.params;
        for (float v : {p.exposure, p.contrast, p.highlights, p.shadows, p.temperature, p.tint, p.saturation}) w.put(v);
        w.put(static_cast<std::uint32_t>(mask.components.size()));
        for (const MaskComponent& c : mask.components) {
            w.put(static_cast<std::uint32_t>(c.shape));
            w.put(static_cast<std::uint32_t>(c.mode));
            w.putBool(c.invert);
            for (float v : {c.opacity, c.start[0], c.start[1], c.end[0], c.end[1], c.size[0], c.size[1], c.angle, c.feather,
                            c.low, c.high, c.falloff, c.hue, c.hueWidth}) {
                w.put(v);
            }
            w.put(static_cast<std::uint32_t>(c.dabs.size()));
            for (const BrushDab& dab : c.dabs) {
                for (float v : {dab.x, dab.y, dab.radius, dab.feather, dab.flow}) w.put(v);
                w.putBool(dab.erase);
            }
        }
    }
    return std::move(w.bytes);
}

LocalAdjustments deserializeLocalAdjustments(std::span<const std::byte> bytes) {
    LocalAdjustments out;
    if (bytes.empty()) return out;
    Reader r(bytes);
    if (r.get<std::uint32_t>() != kMagic) throw std::invalid_argument("local adjustments: not DarkHouse mask data");
    if (r.get<std::uint32_t>() != kVersion) throw std::invalid_argument("local adjustments: unsupported version");
    const std::uint32_t maskCount = r.count(static_cast<std::uint32_t>(kMaxMasks), "masks");
    for (std::uint32_t m = 0; m < maskCount; ++m) {
        LocalAdjustment mask;
        mask.name = r.getString();
        mask.enabled = r.getBool();
        mask.invert = r.getBool();
        mask.amount = r.get<float>();
        LocalAdjustParams& p = mask.params;
        for (float* v : {&p.exposure, &p.contrast, &p.highlights, &p.shadows, &p.temperature, &p.tint, &p.saturation}) {
            *v = r.get<float>();
        }
        const std::uint32_t componentCount = r.count(static_cast<std::uint32_t>(kMaxComponentsPerMask), "components");
        for (std::uint32_t i = 0; i < componentCount; ++i) {
            MaskComponent c;
            const auto shape = r.get<std::uint32_t>();
            const auto mode = r.get<std::uint32_t>();
            if (shape >= kMaskShapeCount || mode > static_cast<std::uint32_t>(MaskMode::INTERSECT)) {
                throw std::invalid_argument("local adjustments: unknown mask shape or mode");
            }
            c.shape = static_cast<MaskShape>(shape);
            c.mode = static_cast<MaskMode>(mode);
            c.invert = r.getBool();
            for (float* v : {&c.opacity, &c.start[0], &c.start[1], &c.end[0], &c.end[1], &c.size[0], &c.size[1], &c.angle,
                             &c.feather, &c.low, &c.high, &c.falloff, &c.hue, &c.hueWidth}) {
                *v = r.get<float>();
            }
            const std::uint32_t dabCount = r.count(1u << 22, "brush dabs");
            c.dabs.resize(dabCount);
            for (BrushDab& dab : c.dabs) {
                for (float* v : {&dab.x, &dab.y, &dab.radius, &dab.feather, &dab.flow}) *v = r.get<float>();
                dab.erase = r.getBool();
            }
            mask.components.push_back(std::move(c));
        }
        out.masks.push_back(std::move(mask));
    }
    if (!r.done()) throw std::invalid_argument("local adjustments: trailing data");
    return out;
}

LocalAdjustments sanitize(const LocalAdjustments& adjustments) {
    LocalAdjustments out;
    for (const LocalAdjustment& source : adjustments.masks) {
        if (out.masks.size() == kMaxMasks) break;
        LocalAdjustment mask = source;
        mask.amount = clampFinite(mask.amount, 0.0f, 1.0f, 1.0f);
        LocalAdjustParams& p = mask.params;
        p.exposure = clampFinite(p.exposure, -4.0f, 4.0f, 0.0f);
        for (float* v : {&p.contrast, &p.highlights, &p.shadows, &p.temperature, &p.tint, &p.saturation}) {
            *v = clampFinite(*v, -100.0f, 100.0f, 0.0f);
        }
        if (mask.components.size() > kMaxComponentsPerMask) mask.components.resize(kMaxComponentsPerMask);
        for (MaskComponent& c : mask.components) {
            c.opacity = clampFinite(c.opacity, 0.0f, 1.0f, 1.0f);
            for (float* v : {&c.start[0], &c.start[1], &c.end[0], &c.end[1]}) *v = clampFinite(*v, -2.0f, 3.0f, 0.5f);
            for (float* v : {&c.size[0], &c.size[1]}) *v = clampFinite(*v, 0.001f, 4.0f, 0.3f);
            c.angle = wrapDegrees(c.angle);
            c.feather = clampFinite(c.feather, 0.0f, 1.0f, 0.5f);
            c.low = clampFinite(c.low, 0.0f, 1.0f, 0.0f);
            c.high = clampFinite(c.high, 0.0f, 1.0f, 1.0f);
            c.falloff = clampFinite(c.falloff, 0.001f, 1.0f, 0.1f);
            c.hue = wrapDegrees(c.hue);
            c.hueWidth = clampFinite(c.hueWidth, 1.0f, 360.0f, 60.0f);
            for (BrushDab& dab : c.dabs) {
                dab.x = clampFinite(dab.x, -1.0f, 2.0f, 0.5f);
                dab.y = clampFinite(dab.y, -1.0f, 2.0f, 0.5f);
                dab.radius = clampFinite(dab.radius, 1e-4f, 1.0f, 0.05f);
                dab.feather = clampFinite(dab.feather, 0.0f, 1.0f, 0.5f);
                dab.flow = clampFinite(dab.flow, 0.0f, 1.0f, 1.0f);
            }
        }
        out.masks.push_back(std::move(mask));
    }
    return out;
}

// -----------------------------------------------------------------------------
// CPU evaluation
// -----------------------------------------------------------------------------

void BrushRaster::reset(std::uint32_t width, std::uint32_t height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    coverage_.assign(std::size_t{width} * height, 0.0f);
    painted_.clear();
}

std::array<std::uint32_t, 4> BrushRaster::sync(std::span<const BrushDab> dabs) {
    std::array<std::uint32_t, 4> changed{width_, height_, 0, 0};
    auto grow = [&](const std::array<std::uint32_t, 4>& r) {
        if (r[0] >= r[2] || r[1] >= r[3]) return;
        changed = {std::min(changed[0], r[0]), std::min(changed[1], r[1]), std::max(changed[2], r[2]),
                   std::max(changed[3], r[3])};
    };
    const bool extends = dabs.size() >= painted_.size() && std::equal(painted_.begin(), painted_.end(), dabs.begin());
    std::size_t from = painted_.size();
    if (!extends) {
        // Undo, a deleted stroke or another component's dabs: repaint all.
        std::fill(coverage_.begin(), coverage_.end(), 0.0f);
        painted_.clear();
        from = 0;
        grow({0, 0, width_, height_});
    }
    for (std::size_t i = from; i < dabs.size(); ++i) {
        grow(paint(dabs[i]));
        painted_.push_back(dabs[i]);
    }
    if (changed[0] >= changed[2] || changed[1] >= changed[3]) return {0, 0, 0, 0};
    return changed;
}

std::array<std::uint32_t, 4> BrushRaster::paint(const BrushDab& dab) {
    if (width_ == 0 || height_ == 0) return {0, 0, 0, 0};
    const float cx = dab.x * static_cast<float>(width_);
    const float cy = dab.y * static_cast<float>(height_);
    const float radius = std::max(dab.radius * static_cast<float>(std::max(width_, height_)), 0.5f);
    const float inner = radius * (1.0f - dab.feather);
    auto clampTo = [](float v, std::uint32_t limit) {
        return static_cast<std::uint32_t>(std::clamp(v, 0.0f, static_cast<float>(limit)));
    };
    const std::uint32_t x0 = clampTo(std::floor(cx - radius), width_), x1 = clampTo(std::ceil(cx + radius), width_);
    const std::uint32_t y0 = clampTo(std::floor(cy - radius), height_), y1 = clampTo(std::ceil(cy + radius), height_);
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const float d = std::hypot(static_cast<float>(x) + 0.5f - cx, static_cast<float>(y) + 0.5f - cy);
            const float shape = dab.feather < 1e-3f ? (d < radius ? 1.0f : 0.0f) : 1.0f - smoothstep(inner, radius, d);
            const float a = dab.flow * shape;
            if (a <= 0.0f) continue;
            float& v = coverage_[std::size_t{y} * width_ + x];
            v = dab.erase ? v * (1.0f - a) : v + (1.0f - v) * a;
        }
    }
    return {x0, y0, x1, y1};
}

float componentAlpha(const MaskComponent& c, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                     std::uint32_t height, const Rgb& rgb, float brush) noexcept {
    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    float alpha = 0.0f;
    switch (c.shape) {
    case MaskShape::BRUSH: alpha = brush; break;
    case MaskShape::LINEAR_GRADIENT: {
        const float sx = c.start[0] * w, sy = c.start[1] * h;
        const float dx = c.end[0] * w - sx, dy = c.end[1] * h - sy;
        const float t = ((px - sx) * dx + (py - sy) * dy) / std::max(dx * dx + dy * dy, 1e-6f);
        alpha = 1.0f - smoothstep(0.0f, 1.0f, t);
        break;
    }
    case MaskShape::RADIAL_GRADIENT: {
        const float qx = px - c.start[0] * w, qy = py - c.start[1] * h;
        const float angle = c.angle * kPi / 180.0f;
        const float cs = std::cos(angle), sn = std::sin(angle);
        const float rx = cs * qx + sn * qy, ry = -sn * qx + cs * qy;
        const float radiusX = std::max(c.size[0] * w, 1e-3f), radiusY = std::max(c.size[1] * h, 1e-3f);
        const float f = std::clamp(c.feather, 0.001f, 1.0f);
        alpha = 1.0f - smoothstep(1.0f - f, 1.0f, std::hypot(rx / radiusX, ry / radiusY));
        break;
    }
    case MaskShape::LUMINANCE_RANGE: {
        const float l = std::clamp(linearSrgbToOklab({std::max(rgb[0], 0.0f), std::max(rgb[1], 0.0f), std::max(rgb[2], 0.0f)})[0], 0.0f, 1.0f);
        const float f = std::max(c.falloff, 1e-3f);
        alpha = smoothstep(c.low - f, c.low, l) * (1.0f - smoothstep(c.high, c.high + f, l));
        break;
    }
    case MaskShape::COLOR_RANGE:
    case MaskShape::SKY: {
        const Rgb lab = linearSrgbToOklab({std::max(rgb[0], 0.0f), std::max(rgb[1], 0.0f), std::max(rgb[2], 0.0f)});
        const float chroma = std::hypot(lab[1], lab[2]);
        const float hue = std::atan2(lab[2], lab[1]) * 180.0f / kPi;
        if (c.shape == MaskShape::COLOR_RANGE) {
            if (chroma > 0.0f) {
                const float halfWidth = 0.5f * c.hueWidth;
                const float soft = std::max(c.falloff * 90.0f, 1.0f);
                alpha = (1.0f - smoothstep(halfWidth, halfWidth + soft, hueDistance(hue, c.hue))) *
                        smoothstep(c.low, c.low + 0.03f, chroma);
            }
        } else {
            const float blue = chroma > 0.0f ? (1.0f - smoothstep(40.0f, 70.0f, hueDistance(hue, 240.0f))) *
                                                   smoothstep(0.02f, 0.06f, chroma)
                                             : 0.0f;
            const float bright = smoothstep(0.8f, 0.95f, lab[0]) * (1.0f - smoothstep(0.02f, 0.05f, chroma));
            const float upper = 1.0f - smoothstep(0.35f, 0.7f, py / h);
            alpha = std::max(blue, bright) * upper;
        }
        break;
    }
    case MaskShape::SUBJECT: {
        const float qx = (px / w - 0.5f) / 0.32f, qy = (py / h - 0.5f) / 0.42f;
        alpha = 1.0f - smoothstep(0.6f, 1.0f, std::hypot(qx, qy));
        break;
    }
    }
    return c.invert ? 1.0f - alpha : alpha;
}

float combineMask(float mask, const MaskComponent& c, float alpha) noexcept {
    switch (c.mode) {
    case MaskMode::SUBTRACT: return mask * (1.0f - c.opacity * alpha);
    case MaskMode::INTERSECT: return mask * (1.0f - c.opacity * (1.0f - alpha));
    case MaskMode::ADD: break;
    }
    const float a = c.opacity * alpha;
    return mask + a - mask * a;
}

bool readsImage(const LocalAdjustment& adjustment) noexcept {
    return std::any_of(adjustment.components.begin(), adjustment.components.end(), [](const MaskComponent& c) {
        return c.shape == MaskShape::LUMINANCE_RANGE || c.shape == MaskShape::COLOR_RANGE || c.shape == MaskShape::SKY;
    });
}

std::vector<float> evaluateMask(const LocalAdjustment& adjustment, std::uint32_t width, std::uint32_t height,
                                std::span<const float> rgba) {
    const bool hasImage = !rgba.empty();
    if (hasImage ? rgba.size() < std::size_t{width} * height * 4 : readsImage(adjustment)) {
        throw std::invalid_argument("evaluateMask: image too small");
    }
    std::vector<BrushRaster> brushes(adjustment.components.size());
    for (std::size_t i = 0; i < adjustment.components.size(); ++i) {
        if (adjustment.components[i].shape != MaskShape::BRUSH) continue;
        brushes[i].reset(width, height);
        brushes[i].sync(adjustment.components[i].dabs);
    }
    std::vector<float> mask(std::size_t{width} * height, 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t p = std::size_t{y} * width + x;
            const Rgb rgb = hasImage ? Rgb{rgba[p * 4], rgba[p * 4 + 1], rgba[p * 4 + 2]} : Rgb{0.0f, 0.0f, 0.0f};
            float m = 0.0f;
            for (std::size_t i = 0; i < adjustment.components.size(); ++i) {
                const MaskComponent& c = adjustment.components[i];
                const float brush = c.shape == MaskShape::BRUSH ? brushes[i].at(x, y) : 0.0f;
                m = combineMask(m, c, componentAlpha(c, x, y, width, height, rgb, brush));
            }
            if (adjustment.invert) m = 1.0f - m;
            mask[p] = std::clamp(m, 0.0f, 1.0f);
        }
    }
    return mask;
}

Rgb applyLocalAdjust(const LocalAdjustParams& params, const Rgb& input) noexcept {
    Rgb rgb = input;
    if (params.temperature != 0.0f || params.tint != 0.0f) {
        const WhiteBalancePush wb = localWhiteBalance(params);
        Rgb balanced{};
        for (int r = 0; r < 3; ++r) balanced[r] = wb.rows[r][0] * rgb[0] + wb.rows[r][1] * rgb[1] + wb.rows[r][2] * rgb[2];
        rgb = balanced;
    }
    rgb = applyTone(rgb, params.exposure, params.highlights / 100.0f, params.shadows / 100.0f, params.contrast / 100.0f);
    const float saturation = std::max(0.0f, 1.0f + params.saturation / 100.0f);
    if (saturation != 1.0f) {
        Rgb lab = linearSrgbToOklab(rgb);
        lab[1] *= saturation;
        lab[2] *= saturation;
        rgb = oklabToLinearSrgb(lab);
    }
    for (float& c : rgb) c = std::clamp(c, 0.0f, 65504.0f);
    return rgb;
}

void writeLayerMask(const LocalAdjustment& adjustment, SparseRasterLayer& layerMask, std::span<const float> sourceRgba) {
    if (layerMask.channels() != 1) throw std::invalid_argument("writeLayerMask: the target is not a one-channel mask");
    const std::vector<float> mask = evaluateMask(adjustment, layerMask.width(), layerMask.height(), sourceRgba);
    layerMask.writeRegion(0, 0, layerMask.width(), layerMask.height(), std::span<const float>(mask));
}

// -----------------------------------------------------------------------------
// MaskEngine (GPU)
// -----------------------------------------------------------------------------

MaskEngine::MaskEngine(const VulkanContext& context, const std::filesystem::path& shaderDirectory) : context_(context) {
    const VkDevice device = context_.device();
    try {
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;  // source, brushes, masks, components, mask records
            bindings[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");

        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        checkVk(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");

        const VkShaderModule module = context_.loadShaderModule(shaderDirectory / "mask_generate.spv");
        ScopeExit destroyModule([&] { vkDestroyShaderModule(device, module, nullptr); });
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout_;
        checkVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_),
                "vkCreateComputePipelines");

        std::array<VkDescriptorPoolSize, 2> poolSizes{{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
                                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = descriptorPool_;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &setLayout_;
        checkVk(vkAllocateDescriptorSets(device, &setInfo, &descriptorSet_), "vkAllocateDescriptorSets");

        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        components_ = context_.createBuffer(kMaxMasks * kMaxComponentsPerMask * sizeof(MaskComponentGpu), usage, false);
        maskRecords_ = context_.createBuffer(kMaxMasks * sizeof(MaskRecordGpu), usage, false);
    } catch (...) {
        destroy();
        throw;
    }
}

MaskEngine::~MaskEngine() { destroy(); }

void MaskEngine::destroy() noexcept {
    const VkDevice device = context_.device();
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorPool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    if (setLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    context_.destroyTexture(brushes_);
    context_.destroyTexture(masks_);
    context_.destroyBuffer(components_);
    context_.destroyBuffer(maskRecords_);
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

void MaskEngine::prepare(const LocalAdjustments& adjustments, std::uint32_t width, std::uint32_t height) {
    // Shader records, with brush components numbered in order.
    std::vector<MaskComponentGpu> components;
    std::vector<MaskRecordGpu> records;
    std::vector<const MaskComponent*> brushComponents;
    for (const LocalAdjustment& mask : adjustments.masks) {
        records.push_back({static_cast<std::uint32_t>(components.size()),
                           static_cast<std::uint32_t>(mask.components.size()), mask.invert ? 1u : 0u, 0u});
        for (const MaskComponent& c : mask.components) {
            MaskComponentGpu g{};
            g.shape = static_cast<std::uint32_t>(c.shape);
            g.mode = static_cast<std::uint32_t>(c.mode);
            g.invert = c.invert ? 1u : 0u;
            if (c.shape == MaskShape::BRUSH) {
                g.brushSlot = static_cast<std::uint32_t>(brushComponents.size());
                brushComponents.push_back(&c);
            }
            g.opacity = c.opacity;
            g.angle = c.angle * kPi / 180.0f;
            g.feather = c.feather;
            g.falloff = c.falloff;
            std::copy_n(std::array<float, 4>{c.start[0], c.start[1], c.end[0], c.end[1]}.data(), 4, g.startEnd);
            std::copy_n(std::array<float, 4>{c.size[0], c.size[1], c.low, c.high}.data(), 4, g.sizeRange);
            std::copy_n(std::array<float, 4>{c.hue, c.hueWidth, 0.0f, 0.0f}.data(), 4, g.hueParams);
            components.push_back(g);
        }
    }
    maskCount_ = static_cast<std::uint32_t>(records.size());
    componentData_.resize(components.size() * sizeof(MaskComponentGpu));
    std::memcpy(componentData_.data(), components.data(), componentData_.size());
    maskData_.resize(records.size() * sizeof(MaskRecordGpu));
    std::memcpy(maskData_.data(), records.data(), maskData_.size());

    // Textures: grow (never shrink) with the mask and brush counts. Anything
    // still running may use the old ones, so wait before replacing them.
    const std::uint32_t maskLayers = layersFor(records.size());
    const std::uint32_t brushLayers = layersFor(brushComponents.size());
    const bool resized = !masks_.valid() || masks_.width != width || masks_.height != height;
    if (resized || masks_.layers < maskLayers || brushes_.layers < brushLayers) {
        context_.waitIdle();
        context_.destroyTexture(masks_);
        context_.destroyTexture(brushes_);
        masks_ = context_.createTexture(width, height, PixelFormat::R16G16B16A16_SFLOAT, kMaskUsage, maskLayers);
        brushes_ = context_.createTexture(width, height, PixelFormat::R16G16B16A16_SFLOAT, kMaskUsage, brushLayers);
        context_.clearTexture(masks_, 0.0f, 0.0f, 0.0f, 0.0f);
        context_.clearTexture(brushes_, 0.0f, 0.0f, 0.0f, 0.0f);
        brushRasters_.clear();  // the new brush texture starts empty
        descriptorsDirty_ = true;
    }

    // Brushes: paint new dabs on the CPU, upload the changed rectangle of each layer.
    brushRasters_.resize(brushComponents.size());
    std::vector<std::array<std::uint32_t, 4>> dirty(brushes_.layers, std::array<std::uint32_t, 4>{0, 0, 0, 0});
    for (std::size_t slot = 0; slot < brushComponents.size(); ++slot) {
        brushRasters_[slot].reset(width, height);
        const std::array<std::uint32_t, 4> r = brushRasters_[slot].sync(brushComponents[slot]->dabs);
        if (r[0] >= r[2] || r[1] >= r[3]) continue;
        std::array<std::uint32_t, 4>& d = dirty[slot / 4];
        d = d[0] >= d[2] ? r
                         : std::array<std::uint32_t, 4>{std::min(d[0], r[0]), std::min(d[1], r[1]), std::max(d[2], r[2]),
                                                        std::max(d[3], r[3])};
    }
    for (std::uint32_t layer = 0; layer < dirty.size(); ++layer) {
        const std::array<std::uint32_t, 4>& d = dirty[layer];
        if (d[0] >= d[2]) continue;
        const std::uint32_t w = d[2] - d[0], h = d[3] - d[1];
        std::vector<float> texels(std::size_t{w} * h * 4, 0.0f);
        for (std::uint32_t channel = 0; channel < 4; ++channel) {
            const std::size_t slot = std::size_t{layer} * 4 + channel;
            if (slot >= brushRasters_.size()) break;
            const BrushRaster& raster = brushRasters_[slot];
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) texels[(std::size_t{y} * w + x) * 4 + channel] = raster.at(d[0] + x, d[1] + y);
            }
        }
        std::vector<std::uint16_t> halves(texels.size());
        floatsToHalves(texels.data(), halves.data(), texels.size());
        const VulkanContext::RegionUpload region{d[0], d[1], w, h, std::as_bytes(std::span<const std::uint16_t>(halves))};
        // A separate submission ahead of the evaluation on the queue; its
        // barrier waits for earlier readers of the texture.
        context_.uploadRegions(brushes_, std::span<const VulkanContext::RegionUpload>(&region, 1), layer);
    }
}

void MaskEngine::writeDescriptors(const GPUTexture& source) {
    std::array<VkDescriptorImageInfo, 3> images{};
    images[0] = {VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_GENERAL};
    images[1] = {VK_NULL_HANDLE, brushes_.view, VK_IMAGE_LAYOUT_GENERAL};
    images[2] = {VK_NULL_HANDLE, masks_.view, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkDescriptorBufferInfo, 2> buffers{{{components_.buffer, 0, VK_WHOLE_SIZE},
                                                   {maskRecords_.buffer, 0, VK_WHOLE_SIZE}}};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i < 3) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i - 3];
        }
    }
    vkUpdateDescriptorSets(context_.device(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    boundSource_ = source.view;
    descriptorsDirty_ = false;
}

void MaskEngine::record(VkCommandBuffer commandBuffer, const GPUTexture& source) {
    if (!masks_.valid()) throw std::logic_error("MaskEngine::record called before prepare");
    if (descriptorsDirty_ || source.view != boundSource_) writeDescriptors(source);

    // Every mask texel is rewritten: discard, after earlier readers.
    recordImageBarrier(commandBuffer, masks_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       /*discardContents=*/true);
    if (maskCount_ == 0) return;

    // Parameters travel inside the command buffer, so an evaluation still in
    // flight keeps reading its own. Earlier reads finish before the update.
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!componentData_.empty()) {
        vkCmdUpdateBuffer(commandBuffer, components_.buffer, 0, componentData_.size(), componentData_.data());
    }
    vkCmdUpdateBuffer(commandBuffer, maskRecords_.buffer, 0, maskData_.size(), maskData_.data());
    recordMemoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0,
                            nullptr);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof maskCount_, &maskCount_);
    vkCmdDispatch(commandBuffer, groups(source.width), groups(source.height), 1);
}

}  // namespace darkhouse
