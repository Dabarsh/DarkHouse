// DarkHouse — develop controls bound to GPU nodes of the develop stack.
//
// Every section edits one or more develop nodes through a NodeBinding:
// slider moves are sent live (SetDevelopParamsEvent), the release saves them
// with the photo, and the first edit of a section whose node is not in the
// stack yet inserts it in canonical order (develop_stack.hpp). The sections
// are hosted by the develop panels.
#pragma once

#include "color_adjust.hpp"
#include "denoise.hpp"
#include "develop_stack.hpp"
#include "render_pipeline.hpp"
#include "tone_curve.hpp"
#include "ui/panel.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace darkhouse::ui {

// One develop node's parameters, as the panel edits them.
template <class Params>
class NodeBinding {
public:
    explicit NodeBinding(std::string nodeType) : type_(std::move(nodeType)) {}

    // Before the widgets: finds the node and, unless one of this binding's
    // widgets is being dragged, loads its current values (defaults when the
    // stack has no such node).
    void sync(const DarkHouseApp& app) {
        const std::vector<EditNodeRecord>& stack = app.developStack();
        index_.reset();
        const std::optional<std::size_t> found = findDevelopNode(stack, type_);
        if (found && stack[*found].serializedParams.size() == sizeof(Params)) {
            index_ = static_cast<std::uint32_t>(*found);
            if (!editing_) std::memcpy(&values_, stack[*found].serializedParams.data(), sizeof(Params));
        } else if (!editing_) {
            values_ = Params{};
        }
    }

    [[nodiscard]] Params& values() noexcept { return values_; }
    [[nodiscard]] std::optional<std::uint32_t> index() const noexcept { return index_; }

    // After the widgets: live while dragging, saved on release.
    void commit(PanelContext& ctx, const SliderResult& result) {
        editing_ = ImGui::IsAnyItemActive() && (result.changed || editing_) && !result.released;
        if (!result.changed && !result.released) return;
        if (index_) {
            ctx.app.postEvent(SetDevelopParamsEvent{*index_, packParams(values_), result.released, type_});
        } else {
            ctx.app.postEvent(SetDevelopStackEvent{
                withDevelopNode(ctx.app.developStack(), EditNodeRecord{0, type_, packParams(values_)}), true});
        }
    }

    // Sets every value back to the node's defaults and saves.
    void reset(PanelContext& ctx) {
        values_ = Params{};
        commit(ctx, SliderResult{true, true});
    }

private:
    std::string type_;
    Params values_{};
    std::optional<std::uint32_t> index_;
    bool editing_ = false;
};

// White balance, tone and presence: the "Basic" panel of a raw developer.
class BasicSection {
public:
    void draw(PanelContext& ctx);

private:
    NodeBinding<WhiteBalanceParams> whiteBalance_{"white_balance"};
    NodeBinding<ExposureParams> tone_{"exposure"};
    NodeBinding<ColorGradingParams> presence_{"color_grading"};  // vibrance / saturation live in the grading node
};

// Point tone curves: the composite RGB curve and one per channel, with presets.
class ToneCurveSection {
public:
    void draw(PanelContext& ctx);

private:
    NodeBinding<ToneCurveParams> curves_{"tone_curve"};
    int channel_ = 0;  // CurveChannel
};

// 8-band HSL colour mixer.
class ColorMixerSection {
public:
    void draw(PanelContext& ctx);

private:
    NodeBinding<HslParams> hsl_{"hsl"};
    int mode_ = 0;  // 0 hue, 1 saturation, 2 luminance
};

// 3-way colour wheels, global wheel, blending and balance.
class ColorGradingSection {
public:
    void draw(PanelContext& ctx);

private:
    NodeBinding<ColorGradingParams> grading_{"color_grading"};
};

// Noise reduction (the denoise node is enabled explicitly).
class DetailSection {
public:
    void draw(PanelContext& ctx);

private:
    NodeBinding<DenoiseParams> denoise_{"denoise"};
};

}  // namespace darkhouse::ui
