// DarkHouse — design tokens (sizes and colours) shared by the shell and panels.
#pragma once

#include "asset_manager.hpp"

#include <imgui.h>

namespace darkhouse::ui::theme {

// --- Sizes ---------------------------------------------------------------------
// Unscaled; GuiEngine applies the display scale to the style afterwards, and
// widgets multiply their own pixel constants by scale().

inline constexpr float kControlHeight = 24.0f;  // buttons, fields, sliders, list rows
inline constexpr float kRowPitch = 28.0f;       // control + gap: also the pointer target
inline constexpr float kToolbarHeight = 28.0f;
inline constexpr float kFontBody = 15.0f;       // ImGui sizes are line heights: 15 px is a 13 pt em
inline constexpr float kFontSmall = 13.0f;      // 11 pt: secondary labels, counts, read-outs
inline constexpr float kRadiusControl = 5.0f;
inline constexpr float kRadiusGroup = 6.0f;
inline constexpr float kRadiusPopup = 8.0f;

// --- Colours: blue on black ----------------------------------------------------

constexpr ImVec4 rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(hex & 0xFF) / 255.0f, alpha);
}

// Surfaces, darkest to most raised. Photos sit on pure black, which is
// neutral; the blue cast is kept to the chrome around them.
inline constexpr ImVec4 kCanvas = rgb(0x000000);  // viewport surround, grid and filmstrip wells
inline constexpr ImVec4 kWindow = rgb(0x080B11);  // panels
inline constexpr ImVec4 kBar = rgb(0x0C1119);     // toolbar
inline constexpr ImVec4 kPopup = rgb(0x10161F, 0.98f);

// Fills are one pale blue at an alpha, so they sit correctly on any surface.
inline constexpr unsigned kTint = 0x7FB0FF;
inline constexpr ImVec4 kFillGroup = rgb(kTint, 0.05f);    // inset groups
inline constexpr ImVec4 kFillControl = rgb(kTint, 0.11f);  // buttons, fields
inline constexpr ImVec4 kFillHovered = rgb(kTint, 0.17f);
inline constexpr ImVec4 kFillActive = rgb(kTint, 0.26f);
inline constexpr ImVec4 kSeparator = rgb(kTint, 0.13f);
inline constexpr ImVec4 kPopupBorder = rgb(kTint, 0.22f);

inline constexpr ImVec4 kText = rgb(0xE8EEF8);           // 16.9:1 on kWindow
inline constexpr ImVec4 kTextSecondary = rgb(0x9BACC4);  // 8.5:1: labels, counts
inline constexpr ImVec4 kTextTertiary = rgb(0x5C6A80);   // 3.6:1: unavailable content only

// The accent marks where input is going: selection, focus, the primary
// action, progress. kAccent carries white text (4.6:1); kAccentBright is for
// strokes, fills of slider tracks and text on black (7.2:1).
inline constexpr ImVec4 kAccent = rgb(0x2F6FEB);
inline constexpr ImVec4 kAccentHovered = rgb(0x3B7BF2);
inline constexpr ImVec4 kAccentActive = rgb(0x2459C7);
inline constexpr ImVec4 kAccentBright = rgb(0x5C9DFF);
inline constexpr ImVec4 kSelected = rgb(0x2F6FEB, 0.30f);  // selected list row or thumbnail cell

inline constexpr ImVec4 kDanger = rgb(0xFF5A52);
inline constexpr ImVec4 kWarning = rgb(0xF5B83D);
inline constexpr ImVec4 kStar = rgb(0xFAC747);
inline constexpr ImVec4 kPick = rgb(0xEBEBEB);
inline constexpr ImVec4 kReject = rgb(0xFF5A52);

// Layer-type chips in the layer stack.
inline constexpr ImVec4 kLayerParametric = rgb(0xB28EF0);
inline constexpr ImVec4 kLayerRaster = rgb(0x78AAF5);
inline constexpr ImVec4 kLayerVector = rgb(0x6ECD91);
inline constexpr ImVec4 kLayerSmart = rgb(0xF2A65A);
inline constexpr ImVec4 kLayerGroup = rgb(0x9BACC4);

[[nodiscard]] ImVec4 colorLabelColor(ColorLabel label) noexcept;
[[nodiscard]] const char* colorLabelName(ColorLabel label) noexcept;

// Colour for a draw-list call, with the current style alpha (so BeginDisabled dims it).
[[nodiscard]] ImU32 u32(const ImVec4& color, float alphaMul = 1.0f);
// Display scale of the UI: multiply pixel constants that do not come from the style.
[[nodiscard]] float scale();

// The DarkHouse look. Sizes are unscaled; GuiEngine applies the display scale afterwards.
void applyStyle(ImGuiStyle& style);

// Registers the embedded UI font (Roboto Medium) as the default font.
void loadFonts(ImGuiIO& io, ImGuiStyle& style);

}  // namespace darkhouse::ui::theme
