#include "lens_correction.hpp"

#include "color_adjust.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace darkhouse {
namespace {

constexpr float kHalfMax = 65504.0f;

float clampFinite(float value, float lo, float hi, float fallback) {
    return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}

std::array<float, 3> finite3(const std::array<float, 3>& v, const std::array<float, 3>& fallback) {
    for (float x : v) {
        if (!std::isfinite(x) || std::fabs(x) > 10.0f) return fallback;
    }
    return v;
}

// Ratio of source to corrected radius for a distortion model.
float distortionRatio(std::uint32_t model, const std::array<float, 4>& k, float r) noexcept {
    switch (static_cast<DistortionModel>(model)) {
    case DistortionModel::PTLENS: return k[0] * r * r * r + k[1] * r * r + k[2] * r + 1.0f - k[0] - k[1] - k[2];
    case DistortionModel::POLY3: return 1.0f - k[0] + k[0] * r * r;
    case DistortionModel::POLY5: return 1.0f + k[0] * r * r + k[1] * r * r * r * r;
    case DistortionModel::NONE: break;
    }
    return 1.0f;
}

float tcaRatio(std::uint32_t model, const std::array<float, 4>& k, float r) noexcept {
    switch (static_cast<TcaModel>(model)) {
    case TcaModel::LINEAR: return k[0];
    case TcaModel::POLY3: return k[2] * r * r + k[1] * r + k[0];
    case TcaModel::NONE: break;
    }
    return 1.0f;
}

// Clamped-edge bilinear sample of one channel at pixel position (x, y)
// (pixel centres at i + 0.5).
float bilinear(std::span<const float> rgba, std::uint32_t width, std::uint32_t height, float x, float y, int channel) {
    const float fx = x - 0.5f, fy = y - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    const int maxX = static_cast<int>(width) - 1, maxY = static_cast<int>(height) - 1;
    const int x0 = std::clamp(static_cast<int>(x0f), 0, maxX), x1 = std::clamp(static_cast<int>(x0f) + 1, 0, maxX);
    const int y0 = std::clamp(static_cast<int>(y0f), 0, maxY), y1 = std::clamp(static_cast<int>(y0f) + 1, 0, maxY);
    auto at = [&](int px, int py) { return rgba[(static_cast<std::size_t>(py) * width + static_cast<std::size_t>(px)) * 4 + static_cast<std::size_t>(channel)]; };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return top + (bottom - top) * ty;
}

// Every sample of every border pixel lands inside the image.
bool bordersInside(const LensCorrectionPush& push, std::uint32_t width, std::uint32_t height) {
    constexpr int kSteps = 48;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    auto inside = [&](const std::array<float, 2>& p) {
        return p[0] >= 0.5f - 1e-3f && p[0] <= w - 0.5f + 1e-3f && p[1] >= 0.5f - 1e-3f && p[1] <= h - 0.5f + 1e-3f;
    };
    auto check = [&](float x, float y) {
        const LensSample s = lensSourcePosition(push, x, y);
        return inside(s.red) && inside(s.green) && inside(s.blue);
    };
    for (int i = 0; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / kSteps;
        const float x = 0.5f + (w - 1.0f) * t, y = 0.5f + (h - 1.0f) * t;
        if (!check(x, 0.5f) || !check(x, h - 0.5f) || !check(0.5f, y) || !check(w - 0.5f, y)) return false;
    }
    return true;
}

}  // namespace

LensCorrectionParams sanitize(const LensCorrectionParams& params) {
    LensCorrectionParams out = params;
    out.profileEnabled = params.profileEnabled ? 1u : 0u;
    if (static_cast<std::uint32_t>(params.distortionModel) > static_cast<std::uint32_t>(DistortionModel::POLY5)) {
        out.distortionModel = DistortionModel::NONE;
    }
    if (static_cast<std::uint32_t>(params.tcaModel) > static_cast<std::uint32_t>(TcaModel::POLY3)) out.tcaModel = TcaModel::NONE;
    out.distortion = finite3(params.distortion, {});
    out.tcaRed = finite3(params.tcaRed, {1.0f, 0.0f, 0.0f});
    out.tcaBlue = finite3(params.tcaBlue, {1.0f, 0.0f, 0.0f});
    out.hasVignetting = params.hasVignetting ? 1u : 0u;
    out.vignetting = finite3(params.vignetting, {});
    out.radiusScale = clampFinite(params.radiusScale, 0.1f, 10.0f, 1.0f);
    out.distortionAmount = clampFinite(params.distortionAmount, 0.0f, 200.0f, 100.0f);
    out.vignettingAmount = clampFinite(params.vignettingAmount, 0.0f, 200.0f, 100.0f);
    out.removeChromaticAberration = params.removeChromaticAberration ? 1u : 0u;
    out.manualDistortion = clampFinite(params.manualDistortion, -100.0f, 100.0f, 0.0f);
    out.manualVignetting = clampFinite(params.manualVignetting, -100.0f, 100.0f, 0.0f);
    out.manualVignettingMidpoint = clampFinite(params.manualVignettingMidpoint, 0.0f, 100.0f, 50.0f);
    out.manualCaRed = clampFinite(params.manualCaRed, -100.0f, 100.0f, 0.0f);
    out.manualCaBlue = clampFinite(params.manualCaBlue, -100.0f, 100.0f, 0.0f);
    out.constrainCrop = params.constrainCrop ? 1u : 0u;
    out.scale = clampFinite(params.scale, 50.0f, 150.0f, 100.0f);
    out.profileName[sizeof out.profileName - 1] = '\0';
    return out;
}

bool isIdentity(const LensCorrectionParams& params) noexcept {
    const bool profile = params.profileEnabled &&
                         ((params.distortionModel != DistortionModel::NONE && params.distortionAmount > 0.0f) ||
                          (params.tcaModel != TcaModel::NONE && params.removeChromaticAberration) ||
                          (params.hasVignetting && params.vignettingAmount > 0.0f));
    return !profile && params.manualDistortion == 0.0f && params.manualVignetting == 0.0f && params.manualCaRed == 0.0f &&
           params.manualCaBlue == 0.0f && params.scale == 100.0f;
}

void applyProfile(LensCorrectionParams& params, const ResolvedLensProfile& profile) {
    params.profileEnabled = 1;
    params.distortionModel = profile.distortionModel;
    params.distortion = profile.distortion;
    params.tcaModel = profile.tcaModel;
    params.tcaRed = profile.tcaRed;
    params.tcaBlue = profile.tcaBlue;
    params.hasVignetting = profile.hasVignetting ? 1u : 0u;
    params.vignetting = profile.vignetting;
    params.radiusScale = profile.radiusScale;
    std::memset(params.profileName, 0, sizeof params.profileName);
    std::strncpy(params.profileName, profile.name.c_str(), sizeof params.profileName - 1);
}

LensCorrectionPush lensCorrectionPush(const LensCorrectionParams& raw, std::uint32_t width, std::uint32_t height) {
    const LensCorrectionParams params = sanitize(raw);
    LensCorrectionPush push;
    const float w = static_cast<float>(std::max(width, 1u)), h = static_cast<float>(std::max(height, 1u));
    push.geometry = {w * 0.5f, h * 0.5f, std::min(w, h) * 0.5f / params.radiusScale,
                     std::hypot(w, h) * 0.5f / params.radiusScale};
    const bool profile = params.profileEnabled != 0;
    if (profile && params.distortionModel != DistortionModel::NONE && params.distortionAmount > 0.0f) {
        push.distortion = {params.distortion[0], params.distortion[1], params.distortion[2], params.distortionAmount / 100.0f};
        push.models[0] = static_cast<std::uint32_t>(params.distortionModel);
    }
    push.tcaRed = {1.0f, 0.0f, 0.0f, 1.0f + params.manualCaRed / 100.0f * 0.002f};
    push.tcaBlue = {1.0f, 0.0f, 0.0f, 1.0f + params.manualCaBlue / 100.0f * 0.002f};
    if (profile && params.removeChromaticAberration && params.tcaModel != TcaModel::NONE) {
        push.tcaRed = {params.tcaRed[0], params.tcaRed[1], params.tcaRed[2], push.tcaRed[3]};
        push.tcaBlue = {params.tcaBlue[0], params.tcaBlue[1], params.tcaBlue[2], push.tcaBlue[3]};
        push.models[1] = static_cast<std::uint32_t>(params.tcaModel);
    }
    if (profile && params.hasVignetting && params.vignettingAmount > 0.0f) {
        push.vignetting = {params.vignetting[0], params.vignetting[1], params.vignetting[2], params.vignettingAmount / 100.0f};
    }
    // Positive manual distortion pulls the edges in: it undoes barrel distortion.
    push.manual = {-0.1f * params.manualDistortion / 100.0f, params.manualVignetting / 100.0f,
                   params.manualVignettingMidpoint / 100.0f, 1.0f};

    // Constrain crop: the widest view (up to the full frame) whose samples all
    // stay inside the photo, found by bisection; then the manual scale.
    float view = 1.0f;
    if (params.constrainCrop && !bordersInside(push, width, height)) {
        float lo = 0.2f, hi = 1.0f;
        for (int i = 0; i < 24; ++i) {
            const float mid = 0.5f * (lo + hi);
            push.manual[3] = mid;
            (bordersInside(push, width, height) ? lo : hi) = mid;
        }
        view = lo;
    }
    push.manual[3] = view * 100.0f / params.scale;
    return push;
}

LensSample lensSourcePosition(const LensCorrectionPush& push, float x, float y) noexcept {
    const float cx = push.geometry[0], cy = push.geometry[1], unit = push.geometry[2], vignetteUnit = push.geometry[3];
    float vx = (x - cx) / unit * push.manual[3];
    float vy = (y - cy) / unit * push.manual[3];
    const float manual = 1.0f + push.manual[0] * (vx * vx + vy * vy);
    vx *= manual;
    vy *= manual;
    if (push.models[0] != 0) {
        const float ratio = 1.0f + (distortionRatio(push.models[0], push.distortion, std::sqrt(vx * vx + vy * vy)) - 1.0f) *
                                       push.distortion[3];
        vx *= ratio;
        vy *= ratio;
    }
    LensSample sample;
    sample.green = {cx + vx * unit, cy + vy * unit};
    const float r = std::sqrt(vx * vx + vy * vy);
    const float red = tcaRatio(push.models[1], push.tcaRed, r) * push.tcaRed[3];
    const float blue = tcaRatio(push.models[1], push.tcaBlue, r) * push.tcaBlue[3];
    sample.red = {cx + vx * red * unit, cy + vy * red * unit};
    sample.blue = {cx + vx * blue * unit, cy + vy * blue * unit};

    if (push.vignetting[3] > 0.0f) {
        const float dx = sample.green[0] - cx, dy = sample.green[1] - cy;
        const float r2 = (dx * dx + dy * dy) / (vignetteUnit * vignetteUnit);
        const float falloff = 1.0f + push.vignetting[0] * r2 + push.vignetting[1] * r2 * r2 + push.vignetting[2] * r2 * r2 * r2;
        sample.gain = std::pow(1.0f / std::max(falloff, 0.05f), push.vignetting[3]);
    }
    if (push.manual[1] != 0.0f) {
        const float corner = std::hypot(cx, cy);
        const float rc = std::hypot(x - cx, y - cy) / corner;
        const float start = push.manual[2] * 0.9f;
        float t = std::clamp((rc - start) / (1.0f - start), 0.0f, 1.0f);
        t = t * t * (3.0f - 2.0f * t);
        sample.gain *= std::exp2(push.manual[1] * t);
    }
    return sample;
}

std::vector<float> applyLensCorrection(const LensCorrectionPush& push, std::uint32_t width, std::uint32_t height,
                                       std::span<const float> rgba) {
    if (rgba.size() < std::size_t{width} * height * 4) throw std::invalid_argument("applyLensCorrection: image too small");
    std::vector<float> out(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const LensSample s = lensSourcePosition(push, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            float* p = &out[(std::size_t{y} * width + x) * 4];
            p[0] = std::clamp(bilinear(rgba, width, height, s.red[0], s.red[1], 0) * s.gain, 0.0f, kHalfMax);
            p[1] = std::clamp(bilinear(rgba, width, height, s.green[0], s.green[1], 1) * s.gain, 0.0f, kHalfMax);
            p[2] = std::clamp(bilinear(rgba, width, height, s.blue[0], s.blue[1], 2) * s.gain, 0.0f, kHalfMax);
            p[3] = bilinear(rgba, width, height, s.green[0], s.green[1], 3);
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// LensCorrectionNode
// -----------------------------------------------------------------------------

LensCorrectionNode::LensCorrectionNode(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : PointOperatorNode(context, shaderDirectory / "lens_correction.spv", sizeof(LensCorrectionPush),
                        PixelFormat::R16G16B16A16_SFLOAT),
      push_(lensCorrectionPush(params_, width_, height_)) {}

void LensCorrectionNode::updateUniforms(std::span<const std::byte> packedParams) {
    setParams(unpackParams<LensCorrectionParams>(packedParams, "LensCorrectionNode::updateUniforms"));
}

void LensCorrectionNode::setInputTexture(std::uint32_t slot, const GPUTexture& texture) {
    PointOperatorNode::setInputTexture(slot, texture);
    width_ = texture.width;
    height_ = texture.height;
    push_ = lensCorrectionPush(params_, width_, height_);
}

void LensCorrectionNode::setParams(const LensCorrectionParams& params) {
    params_ = sanitize(params);
    push_ = lensCorrectionPush(params_, width_, height_);
}

}  // namespace darkhouse
