// DarkHouse — reusable ImGui widgets for the panels.
#pragma once

#include "asset_manager.hpp"
#include "ui/panel.hpp"

#include <imgui.h>

#include <cstdint>
#include <string>

namespace darkhouse::ui {

// --- Asset badges (draw-list only, no interaction) ----------------------------
void drawStars(ImDrawList* drawList, ImVec2 topLeft, int rating, float starSize, bool showEmpty = true);
void drawFlagBadge(ImDrawList* drawList, ImVec2 center, AssetFlag flag, float size);
void drawColorLabelDot(ImDrawList* drawList, ImVec2 center, ColorLabel label, float radius);

// Placeholder thumbnail card (until the decoder produces real previews): a
// tile in the photo's aspect ratio, tinted from its content hash, with the
// file type, name, rating, flag and colour label.
void drawAssetCard(ImDrawList* drawList, ImVec2 min, ImVec2 max, const AssetRecord& asset, bool selected,
                   bool hovered, bool showCaption = true);

// --- Interactive widgets ---------------------------------------------------------
// Five clickable stars. Clicking the current rating clears it. Returns true on change.
bool ratingWidget(const char* id, int& rating, float starSize = 0.0f);
// Row of colour-label swatches; `allowAny` adds a leading "any" swatch (value -1).
bool colorLabelPicker(const char* id, int& label, bool allowAny);

struct SliderResult {
    bool changed = false;   // value changed this frame (while dragging too)
    bool released = false;  // an edit finished this frame: persist now
};
// Slider with an optional horizontal gradient behind it (hue, temperature,
// tint...). Double-click resets to `defaultValue`, like most raw developers.
SliderResult adjustmentSlider(const char* label, float& value, float min, float max, float defaultValue,
                              const char* format, ImU32 gradientLeft = 0, ImU32 gradientRight = 0);

// --- Formatting -----------------------------------------------------------------
[[nodiscard]] std::string formatDateTime(std::int64_t unixSeconds);  // "2024-06-01 14:03" (UTC)
[[nodiscard]] std::string formatShutter(double seconds);            // "1/250 s", "2.5 s"
[[nodiscard]] std::string fileExtensionTag(const std::string& fileName);  // "NEF"

// --- Keyboard ---------------------------------------------------------------------
// Lightroom-style culling keys for the focused grid or filmstrip:
//   Left/Right (and Up/Down by `rowStride`), Home/End  move the selection
//   0-5 rating, P pick, X reject, U unflag, 6-9 red/yellow/green/blue label
//   Enter opens the selected asset on the canvas
void handleAssetKeys(PanelContext& ctx, int rowStride);

}  // namespace darkhouse::ui
