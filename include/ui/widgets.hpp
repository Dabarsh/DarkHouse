// DarkHouse — reusable ImGui widgets for the shell and panels.
#pragma once

#include "asset_manager.hpp"
#include "ui/panel.hpp"

#include <imgui.h>

#include <cstdint>
#include <initializer_list>
#include <string>

namespace darkhouse::ui {

// --- Text ------------------------------------------------------------------------
// Scope with the small font (theme::kFontSmall): secondary labels, counts, read-outs.
class SmallText {
public:
    SmallText();
    ~SmallText();
    SmallText(const SmallText&) = delete;
    SmallText& operator=(const SmallText&) = delete;
};

// "Cmd+I" on macOS, "Ctrl+I" elsewhere: ImGui maps ImGuiMod_Ctrl to Cmd there,
// and the menus have to say so.
[[nodiscard]] std::string shortcutLabel(const char* key);

// --- Icons -----------------------------------------------------------------------
// Drawn with the draw list in a 16-unit box, so they follow the display scale
// and need no icon font.
enum class Icon : std::uint8_t {
    PLUS, TRASH, ARROW_UP, ARROW_DOWN, EYE, EYE_OFF, RESET, CHEVRON_RIGHT, CHEVRON_DOWN, SEARCH,
    SIDEBAR_LEFT, SIDEBAR_RIGHT, PANEL_BOTTOM, FLAG, XMARK, IMPORT, OPEN, COPY, CHECK, SLIDERS,
};
void drawIcon(ImDrawList* drawList, Icon icon, ImVec2 center, ImU32 color, bool filled = false);

// --- Depth -----------------------------------------------------------------------
// Soft ambient shadow outside a rounded rectangle, biased downwards.
void softShadow(ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding, float alpha, int blur = 14);
// Call once per frame after every window has been submitted: gives each menu,
// combo, context menu, tooltip, modal and floating panel the same shadow.
void drawPopupShadows();
// Background of an overlay floating over the canvas (zoom controls, read-outs).
void hudBackground(ImDrawList* drawList, ImVec2 min, ImVec2 max);

// --- Asset badges (draw-list only, no interaction) ----------------------------
void drawStars(ImDrawList* drawList, ImVec2 topLeft, int rating, float starSize, bool showEmpty = true);
void drawFlagBadge(ImDrawList* drawList, ImVec2 center, AssetFlag flag, float size);
void drawColorLabelDot(ImDrawList* drawList, ImVec2 center, ColorLabel label, float radius);

// Placeholder thumbnail card (until the decoder produces real previews): a
// tile in the photo's aspect ratio, tinted from its content hash, with the
// file type, name, rating, flag and colour label. `focused` says whether the
// owning panel has keyboard focus: the selection ring is bright only then.
void drawAssetCard(ImDrawList* drawList, ImVec2 min, ImVec2 max, const AssetRecord& asset, bool selected,
                   bool hovered, bool showCaption = true, bool focused = true);

// --- Controls --------------------------------------------------------------------
// Square icon button: no fill until hovered; `on` latches it as a toggle.
bool iconButton(const char* id, Icon icon, const char* tooltip = nullptr, bool on = false, bool enabled = true);
// Icon and label in one button; `primary` fills it with the accent.
bool labelButton(const char* id, Icon icon, const char* label, bool primary = false);
// Text button filled with the accent: the default action of a dialog.
bool primaryButton(const char* label, ImVec2 size = ImVec2(0.0f, 0.0f));
// Segmented control ("pick one"). Every segment is a real item, so keyboard
// navigation and the focus ring work as for any button. `prominent` fills the
// selected segment with the accent. Returns true when `index` changes.
bool segmented(const char* id, int& index, std::initializer_list<const char*> labels, float minSegmentWidth = 0.0f,
               bool prominent = false);
// Status pill: a tinted label for state ("Preview", "Mask"), never for actions.
void pill(const char* text, const ImVec4& tint);
[[nodiscard]] float pillWidth(const char* text);
// Small checkbox for dense rows and section headers.
bool miniCheckbox(const char* id, bool& value, const char* label = nullptr);

// Five clickable stars. Clicking the current rating clears it. Returns true on change.
bool ratingWidget(const char* id, int& rating, float starSize = 0.0f);
// Row of colour-label swatches; `allowAny` adds a leading "any" swatch (value -1).
bool colorLabelPicker(const char* id, int& label, bool allowAny);
[[nodiscard]] float colorLabelPickerWidth(bool allowAny);

struct SliderResult {
    bool changed = false;   // value changed this frame (while dragging too)
    bool released = false;  // an edit finished this frame: persist now
};
// Inspector row: label | track | value. The value has its own right-aligned
// slot, so it never moves and the knob never covers it; clicking it types a
// value, as does Ctrl+click on the track. The track fills from `defaultValue`
// to the knob, or shows a gradient (hue, temperature, tint...). Double-click
// the label to reset, like most raw developers.
SliderResult adjustmentSlider(const char* label, float& value, float min, float max, float defaultValue,
                              const char* format, ImU32 gradientLeft = 0, ImU32 gradientRight = 0,
                              ImGuiSliderFlags flags = ImGuiSliderFlags_None);

// --- Layout ----------------------------------------------------------------------
// Small secondary caption heading a group of rows ("Library", "Camera").
void sectionLabel(const char* text);
// Inspector section header: chevron and title; the caller adds trailing
// controls on the same row with alignRight(). Returns whether it is open.
bool sectionHeader(const char* title, bool& open, float trailingWidth = 0.0f);
// Moves the cursor so the next `width` pixels end at the right edge of the row just submitted.
void alignRight(float width);
// Width of an iconButton, for alignRight().
[[nodiscard]] float iconButtonWidth();

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
