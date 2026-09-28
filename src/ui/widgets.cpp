#include "ui/widgets.hpp"

#include "ui/theme.hpp"

#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace darkhouse::ui {
namespace {

constexpr float kPi = 3.14159265358979f;

ImU32 toU32(const ImVec4& color, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
}

// A five-pointed star as a triangle fan around its centre (the outline is
// concave, so AddConvexPolyFilled cannot draw it).
void drawStar(ImDrawList* drawList, glm::vec2 center, float radius, ImU32 color, bool filled) {
    std::array<ImVec2, 10> points;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const float r = (i % 2 == 0) ? radius : radius * 0.45f;
        const float angle = -kPi * 0.5f + static_cast<float>(i) * kPi / 5.0f;
        points[i] = center + r * glm::vec2(std::cos(angle), std::sin(angle));
    }
    if (filled) {
        for (std::size_t i = 0; i < points.size(); ++i) {
            drawList->AddTriangleFilled(center, points[i], points[(i + 1) % points.size()], color);
        }
    } else {
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, 1.0f, ImDrawFlags_Closed);
    }
}

// Deterministic, muted tint per asset, so placeholder cards are distinguishable.
glm::vec3 tintFor(const std::string& key) {
    std::uint32_t hash = 2166136261u;  // FNV-1a
    for (unsigned char c : key) hash = (hash ^ c) * 16777619u;
    const float hue = static_cast<float>(hash % 360u) / 360.0f;
    glm::vec3 rgb;
    ImGui::ColorConvertHSVtoRGB(hue, 0.38f, 0.46f, rgb.r, rgb.g, rgb.b);
    return rgb;
}

}  // namespace

// -----------------------------------------------------------------------------
// Badges
// -----------------------------------------------------------------------------

void drawStars(ImDrawList* drawList, ImVec2 topLeft, int rating, float starSize, bool showEmpty) {
    const float radius = starSize * 0.5f;
    for (int i = 0; i < 5; ++i) {
        const glm::vec2 center = glm::vec2(topLeft) + glm::vec2(radius + static_cast<float>(i) * starSize * 1.1f, radius);
        if (i < rating) {
            drawStar(drawList, center, radius, toU32(theme::kStar), true);
        } else if (showEmpty) {
            drawStar(drawList, center, radius * 0.9f, IM_COL32(150, 150, 150, 110), false);
        }
    }
}

void drawFlagBadge(ImDrawList* drawList, ImVec2 center, AssetFlag flag, float size) {
    const glm::vec2 c(center);
    const float h = size * 0.5f;
    if (flag == AssetFlag::PICKED) {
        const ImU32 color = toU32(theme::kPick);
        drawList->AddLine(c + glm::vec2(-h * 0.6f, -h), c + glm::vec2(-h * 0.6f, h), color, 1.5f);
        drawList->AddTriangleFilled(c + glm::vec2(-h * 0.6f, -h), c + glm::vec2(h * 0.8f, -h * 0.45f),
                                    c + glm::vec2(-h * 0.6f, h * 0.1f), color);
    } else if (flag == AssetFlag::REJECTED) {
        const ImU32 color = toU32(theme::kReject);
        drawList->AddLine(c + glm::vec2(-h, -h), c + glm::vec2(h, h), color, 2.0f);
        drawList->AddLine(c + glm::vec2(-h, h), c + glm::vec2(h, -h), color, 2.0f);
    }
}

void drawColorLabelDot(ImDrawList* drawList, ImVec2 center, ColorLabel label, float radius) {
    if (label == ColorLabel::NONE) return;
    drawList->AddCircleFilled(center, radius, toU32(theme::colorLabelColor(label)));
    drawList->AddCircle(center, radius, IM_COL32(0, 0, 0, 120), 0, 1.0f);
}

void drawAssetCard(ImDrawList* drawList, ImVec2 min, ImVec2 max, const AssetRecord& asset, bool selected,
                   bool hovered, bool showCaption) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rounding = 4.0f;
    const ImU32 background = selected  ? IM_COL32(62, 62, 66, 255)
                             : hovered ? IM_COL32(50, 50, 54, 255)
                                       : IM_COL32(38, 38, 41, 255);
    drawList->AddRectFilled(min, max, background, rounding);
    if (selected) drawList->AddRect(min, max, toU32(theme::kAccent), rounding, 2.0f);

    // Image area, in the photo's aspect ratio (3:2 until dimensions are known).
    const float pad = std::max(4.0f, style.FramePadding.x);
    const float captionHeight = showCaption ? ImGui::GetTextLineHeight() + pad : 0.0f;
    const glm::vec2 areaMin = glm::vec2(min) + glm::vec2(pad);
    const glm::vec2 areaMax = glm::vec2(max) - glm::vec2(pad, pad + captionHeight);
    const glm::vec2 area = glm::max(areaMax - areaMin, glm::vec2(1.0f));
    const float aspect = asset.width > 0 && asset.height > 0
                             ? static_cast<float>(asset.width) / static_cast<float>(asset.height)
                             : 1.5f;
    glm::vec2 size = area;
    if (area.x / area.y > aspect) {
        size.x = area.y * aspect;
    } else {
        size.y = area.x / aspect;
    }
    const glm::vec2 imageMin = areaMin + (area - size) * 0.5f;
    const glm::vec2 imageMax = imageMin + size;

    const glm::vec3 tint = tintFor(asset.fileHash.empty() ? asset.id : asset.fileHash);
    const glm::vec3 light = glm::min(tint * 1.35f, glm::vec3(1.0f));
    const glm::vec3 dark = tint * 0.55f;
    const ImU32 top = ImGui::GetColorU32(ImVec4(light.r, light.g, light.b, 1.0f));
    const ImU32 bottom = ImGui::GetColorU32(ImVec4(dark.r, dark.g, dark.b, 1.0f));
    drawList->AddRectFilledMultiColor(imageMin, imageMax, top, top, bottom, bottom);

    // File type in the middle of the tile.
    const std::string tag = fileExtensionTag(asset.fileName);
    const glm::vec2 tagSize = ImGui::CalcTextSize(tag.c_str());
    if (tagSize.x < size.x) {
        drawList->AddText((imageMin + imageMax - tagSize) * 0.5f, IM_COL32(255, 255, 255, 150), tag.c_str());
    }

    // Overlays: flag top-left, label top-right, stars bottom-left.
    const float badge = std::clamp(size.y * 0.14f, 8.0f, 14.0f);
    if (asset.flag != AssetFlag::UNFLAGGED) {
        drawFlagBadge(drawList, imageMin + glm::vec2(badge, badge), asset.flag, badge);
    }
    drawColorLabelDot(drawList, imageMax - glm::vec2(badge, size.y - badge), asset.colorLabel, badge * 0.45f);
    if (asset.rating > 0 && size.x > badge * 6.0f) {
        drawStars(drawList, glm::vec2(imageMin.x + badge * 0.4f, imageMax.y - badge * 1.3f), asset.rating, badge,
                  false);
    }
    if (asset.flag == AssetFlag::REJECTED) drawList->AddRectFilled(imageMin, imageMax, IM_COL32(0, 0, 0, 110));

    if (showCaption) {
        const glm::vec2 textPos(min.x + pad, max.y - captionHeight);
        drawList->PushClipRect(ImVec2(textPos.x, textPos.y), ImVec2(max.x - pad, max.y), true);
        drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text, selected ? 1.0f : 0.75f), asset.fileName.c_str());
        drawList->PopClipRect();
    }
}

// -----------------------------------------------------------------------------
// Interactive widgets
// -----------------------------------------------------------------------------

bool ratingWidget(const char* id, int& rating, float starSize) {
    const float size = starSize > 0.0f ? starSize : ImGui::GetFontSize() * 1.15f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 extent(size * 1.1f * 5.0f, size);
    ImGui::InvisibleButton(id, extent);
    int shown = rating;
    int hoveredStar = 0;
    if (ImGui::IsItemHovered()) {
        const float x = ImGui::GetIO().MousePos.x - origin.x;
        hoveredStar = std::clamp(static_cast<int>(x / (size * 1.1f)) + 1, 1, 5);
        shown = hoveredStar;
        ImGui::SetTooltip("%d star%s (click again to clear)", hoveredStar, hoveredStar == 1 ? "" : "s");
    }
    drawStars(ImGui::GetWindowDrawList(), origin, shown, size);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && hoveredStar > 0) {
        rating = hoveredStar == rating ? 0 : hoveredStar;
        return true;
    }
    return false;
}

bool colorLabelPicker(const char* id, int& label, bool allowAny) {
    ImGui::PushID(id);
    bool changed = false;
    const float radius = ImGui::GetFontSize() * 0.42f;
    const float cell = radius * 2.0f + ImGui::GetStyle().ItemInnerSpacing.x;
    for (int value = allowAny ? -1 : 0; value <= static_cast<int>(ColorLabel::PURPLE); ++value) {
        ImGui::PushID(value);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##swatch", ImVec2(cell, radius * 2.0f + 2.0f))) {
            label = (label == value && value > 0) ? (allowAny ? -1 : 0) : value;
            changed = true;
        }
        const ImVec2 center(origin.x + radius + 1.0f, origin.y + radius + 1.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 muted = IM_COL32(150, 150, 150, 200);
        if (value < 0) {  // "any"
            drawList->AddCircle(center, radius, muted, 0, 1.5f);
            drawList->AddText(ImVec2(center.x - radius * 0.55f, center.y - ImGui::GetFontSize() * 0.5f), muted, "*");
        } else if (value == 0) {  // "none"
            drawList->AddCircle(center, radius, muted, 0, 1.5f);
            drawList->AddLine(ImVec2(center.x - radius * 0.7f, center.y + radius * 0.7f),
                              ImVec2(center.x + radius * 0.7f, center.y - radius * 0.7f), muted, 1.5f);
        } else {
            drawColorLabelDot(drawList, center, static_cast<ColorLabel>(value), radius);
        }
        if (label == value) drawList->AddCircle(center, radius + 2.5f, toU32(theme::kAccent), 0, 2.0f);
        ImGui::SetItemTooltip("%s", value < 0 ? "Any label" : theme::colorLabelName(static_cast<ColorLabel>(value)));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PopID();
    }
    ImGui::NewLine();
    ImGui::PopID();
    return changed;
}

SliderResult adjustmentSlider(const char* label, float& value, float min, float max, float defaultValue,
                              const char* format, ImU32 gradientLeft, ImU32 gradientRight, ImGuiSliderFlags flags) {
    SliderResult result;
    ImGui::PushID(label);
    // Wide enough for the longest adjustment label, so columns line up.
    const float labelWidth = ImGui::CalcTextSize("Temperature").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Double-click to reset");
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && value != defaultValue) {
            value = defaultValue;
            result.changed = result.released = true;
        }
    }
    ImGui::SameLine(labelWidth);

    const bool gradient = gradientLeft != 0 || gradientRight != 0;
    if (gradient) {
        // Paint the gradient where the slider frame will be, then draw the
        // slider over it with a translucent frame.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFrameHeight();
        const float inset = height * 0.32f;
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(p.x, p.y + inset),
                                                            ImVec2(p.x + width, p.y + height - inset), gradientLeft,
                                                            gradientRight, gradientRight, gradientLeft);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    result.changed |= ImGui::SliderFloat("##value", &value, min, max, format, flags);
    result.released |= ImGui::IsItemDeactivatedAfterEdit();
    if (gradient) ImGui::PopStyleColor(3);
    ImGui::PopID();
    return result;
}

// -----------------------------------------------------------------------------
// Formatting
// -----------------------------------------------------------------------------

std::string formatDateTime(std::int64_t unixSeconds) {
    using namespace std::chrono;
    const sys_seconds time{seconds{unixSeconds}};
    const sys_days day = floor<days>(time);
    const year_month_day date{day};
    const long long secondsOfDay = (time - day).count();
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02u %02lld:%02lld", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()), secondsOfDay / 3600,
                  secondsOfDay % 3600 / 60);
    return buffer;
}

std::string formatShutter(double seconds) {
    char buffer[32];
    if (seconds >= 0.3) {
        std::snprintf(buffer, sizeof buffer, "%.1f s", seconds);
    } else {
        std::snprintf(buffer, sizeof buffer, "1/%.0f s", std::round(1.0 / seconds));
    }
    return buffer;
}

std::string fileExtensionTag(const std::string& fileName) {
    std::string ext = std::filesystem::path(fileName).extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return ext.empty() ? std::string("IMG") : ext;
}

// -----------------------------------------------------------------------------
// Keyboard
// -----------------------------------------------------------------------------

void handleAssetKeys(PanelContext& ctx, int rowStride) {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper) return;  // Ctrl+1..3 switch workspaces

    LibraryModel& library = ctx.library;
    const int stride = std::max(rowStride, 1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) library.selectOffset(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) library.selectOffset(+1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) library.selectOffset(-stride);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) library.selectOffset(+stride);
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) library.select(0);
    if (ImGui::IsKeyPressed(ImGuiKey_End) && !library.visible().empty()) library.select(library.visible().size() - 1);

    const AssetRecord* asset = library.selected();
    if (!asset) return;
    for (int stars = 0; stars <= 5; ++stars) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + stars), false) ||
            ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + stars), false)) {
            ctx.app.postEvent(SetRatingEvent{asset->id, stars});
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) ctx.app.postEvent(SetFlagEvent{asset->id, AssetFlag::PICKED});
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) ctx.app.postEvent(SetFlagEvent{asset->id, AssetFlag::REJECTED});
    if (ImGui::IsKeyPressed(ImGuiKey_U, false)) ctx.app.postEvent(SetFlagEvent{asset->id, AssetFlag::UNFLAGGED});
    constexpr std::array<std::pair<ImGuiKey, ColorLabel>, 4> kLabelKeys{{
        {ImGuiKey_6, ColorLabel::RED}, {ImGuiKey_7, ColorLabel::YELLOW},
        {ImGuiKey_8, ColorLabel::GREEN}, {ImGuiKey_9, ColorLabel::BLUE}}};
    for (const auto& [key, label] : kLabelKeys) {
        if (ImGui::IsKeyPressed(key, false)) {
            ctx.app.postEvent(SetColorLabelEvent{asset->id, asset->colorLabel == label ? ColorLabel::NONE : label});
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        ctx.app.postEvent(OpenAssetEvent{asset->id});
    }
}

}  // namespace darkhouse::ui
