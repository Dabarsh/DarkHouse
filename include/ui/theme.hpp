// DarkHouse — UI colours shared by the shell and panels.
#pragma once

#include "asset_manager.hpp"

#include <imgui.h>

namespace darkhouse::ui::theme {

// Darkroom safelight red: the one accent colour of the DarkHouse chrome.
inline constexpr ImVec4 kAccent{0.89f, 0.33f, 0.24f, 1.0f};
inline constexpr ImVec4 kAccentHovered{0.96f, 0.42f, 0.32f, 1.0f};
inline constexpr ImVec4 kAccentActive{0.78f, 0.27f, 0.19f, 1.0f};
inline constexpr ImVec4 kStar{0.98f, 0.78f, 0.28f, 1.0f};
inline constexpr ImVec4 kPick{0.92f, 0.92f, 0.92f, 1.0f};
inline constexpr ImVec4 kReject{0.90f, 0.25f, 0.22f, 1.0f};

// Layer-type chips in the layer stack.
inline constexpr ImVec4 kLayerParametric{0.62f, 0.45f, 0.90f, 1.0f};
inline constexpr ImVec4 kLayerRaster{0.33f, 0.58f, 0.92f, 1.0f};
inline constexpr ImVec4 kLayerVector{0.36f, 0.78f, 0.52f, 1.0f};
inline constexpr ImVec4 kLayerSmart{0.95f, 0.62f, 0.25f, 1.0f};
inline constexpr ImVec4 kLayerGroup{0.60f, 0.60f, 0.62f, 1.0f};

[[nodiscard]] ImVec4 colorLabelColor(ColorLabel label) noexcept;
[[nodiscard]] const char* colorLabelName(ColorLabel label) noexcept;

}  // namespace darkhouse::ui::theme
