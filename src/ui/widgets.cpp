#include "ui/widgets.hpp"

#include "ui/theme.hpp"

#include "color_adjust.hpp"

#include <imgui_internal.h>  // TempInputIsActive, MarkItemEdited, window list for popup shadows

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace darkhouse::ui {
namespace {

using theme::u32;

constexpr float kPi = 3.14159265358979f;

// Visual height of a control on the current row. The row itself may be taller
// (the toolbar); the control is then centred in it and the row is its target.
float controlVisual() {
    return std::min(ImGui::GetFrameHeight(), ImGui::GetFontSize() * (theme::kControlHeight / theme::kFontBody));
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
// Text
// -----------------------------------------------------------------------------

SmallText::SmallText() { ImGui::PushFont(nullptr, theme::kFontSmall); }
SmallText::~SmallText() { ImGui::PopFont(); }

std::string shortcutLabel(const char* key) {
    return std::string(ImGui::GetIO().ConfigMacOSXBehaviors ? "Cmd+" : "Ctrl+") + key;
}

// -----------------------------------------------------------------------------
// Icons
// -----------------------------------------------------------------------------

void drawIcon(ImDrawList* drawList, Icon icon, ImVec2 center, ImU32 color, bool filled) {
    const float s = ImGui::GetFontSize() / theme::kFontBody;
    const float stroke = 1.4f * s;
    const auto at = [&](float x, float y) { return ImVec2(center.x + x * s, center.y + y * s); };
    const auto path = [&](std::initializer_list<ImVec2> points, bool closed = false) {
        for (const ImVec2& point : points) drawList->PathLineTo(at(point.x, point.y));
        drawList->PathStroke(color, stroke, closed ? ImDrawFlags_Closed : ImDrawFlags_None);
    };
    switch (icon) {
    case Icon::PLUS:
        path({{-5, 0}, {5, 0}});
        path({{0, -5}, {0, 5}});
        break;
    case Icon::TRASH:
        path({{-5.5f, -4}, {5.5f, -4}});
        path({{-2, -4}, {-2, -6}, {2, -6}, {2, -4}});
        path({{-4, -4}, {-3.2f, 6}, {3.2f, 6}, {4, -4}});
        break;
    case Icon::ARROW_UP:
        path({{0, 5.5f}, {0, -5}});
        path({{-4, -1}, {0, -5}, {4, -1}});
        break;
    case Icon::ARROW_DOWN:
        path({{0, -5.5f}, {0, 5}});
        path({{-4, 1}, {0, 5}, {4, 1}});
        break;
    case Icon::EYE:
    case Icon::EYE_OFF:
        drawList->PathLineTo(at(-7, 0));
        drawList->PathBezierQuadraticCurveTo(at(0, -8), at(7, 0));
        drawList->PathBezierQuadraticCurveTo(at(0, 8), at(-7, 0));
        drawList->PathStroke(color, stroke, ImDrawFlags_Closed);
        drawList->AddCircleFilled(center, 2.1f * s, color);
        if (icon == Icon::EYE_OFF) path({{-6, -6}, {6, 6}});
        break;
    case Icon::RESET:
        drawList->PathArcTo(center, 5.0f * s, -2.2f, 2.9f, 24);
        drawList->PathStroke(color, stroke);
        path({{-6.4f, -4.8f}, {-2.9f, -4.0f}, {-3.6f, -7.6f}}, true);
        break;
    case Icon::CHEVRON_RIGHT: path({{-2, -4}, {2, 0}, {-2, 4}}); break;
    case Icon::CHEVRON_DOWN: path({{-4, -2}, {0, 2}, {4, -2}}); break;
    case Icon::SEARCH:
        drawList->AddCircle(at(-1, -1), 4.3f * s, color, 0, stroke);
        path({{2.2f, 2.2f}, {6, 6}});
        break;
    case Icon::SIDEBAR_LEFT:
    case Icon::SIDEBAR_RIGHT:
    case Icon::PANEL_BOTTOM: {
        drawList->AddRect(at(-7, -5), at(7, 5), color, 2.0f * s, stroke);
        const ImVec2 a = icon == Icon::SIDEBAR_LEFT ? at(-7, -5) : icon == Icon::SIDEBAR_RIGHT ? at(2.5f, -5) : at(-7, 1.5f);
        const ImVec2 b = icon == Icon::SIDEBAR_LEFT ? at(-2.5f, 5) : at(7, 5);
        if (filled) drawList->AddRectFilled(a, b, color, 2.0f * s);
        if (icon == Icon::SIDEBAR_LEFT) path({{-2.5f, -5}, {-2.5f, 5}});
        if (icon == Icon::SIDEBAR_RIGHT) path({{2.5f, -5}, {2.5f, 5}});
        if (icon == Icon::PANEL_BOTTOM) path({{-7, 1.5f}, {7, 1.5f}});
        break;
    }
    case Icon::FLAG:
        path({{-4, -6}, {-4, 6}});
        if (filled) {
            for (const ImVec2& point : {ImVec2(-4, -5), ImVec2(5, -5), ImVec2(2.5f, -2), ImVec2(5, 1), ImVec2(-4, 1)}) {
                drawList->PathLineTo(at(point.x, point.y));
            }
            drawList->PathFillConcave(color);
        }
        path({{-4, -5}, {5, -5}, {2.5f, -2}, {5, 1}, {-4, 1}});
        break;
    case Icon::XMARK:
        path({{-4, -4}, {4, 4}});
        path({{-4, 4}, {4, -4}});
        break;
    case Icon::IMPORT:
        path({{0, -6.5f}, {0, 2}});
        path({{-3.2f, -1.2f}, {0, 2}, {3.2f, -1.2f}});
        path({{-6, 1}, {-6, 6}, {6, 6}, {6, 1}});
        break;
    case Icon::OPEN:
        path({{-4, 4}, {4, -4}});
        path({{-1.5f, -4}, {4, -4}, {4, 1.5f}});
        break;
    case Icon::COPY:
        drawList->AddRect(at(-2.5f, -2.5f), at(6, 6), color, 1.5f * s, stroke);
        path({{-6, 2}, {-6, -6}, {2, -6}});
        break;
    case Icon::CHECK: path({{-4, 0}, {-1, 3}, {4, -3.5f}}); break;
    case Icon::SLIDERS:
        path({{-6, -3.5f}, {6, -3.5f}});
        path({{-6, 3.5f}, {6, 3.5f}});
        drawList->AddCircleFilled(at(-2, -3.5f), 2.2f * s, color);
        drawList->AddCircleFilled(at(2.5f, 3.5f), 2.2f * s, color);
        break;
    }
}

// -----------------------------------------------------------------------------
// Depth
// -----------------------------------------------------------------------------

// ImGui has no blur, so the shadow is a stack of one-pixel rings with a
// quadratic falloff. They lie outside the rectangle, so they can be drawn
// after its contents.
void softShadow(ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding, float alpha, int blur) {
    const float s = theme::scale();
    for (int i = 0; i < blur; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(blur);
        const float e = static_cast<float>(i + 1) * s;
        const int a = static_cast<int>(alpha * (1.0f - t) * (1.0f - t) * 255.0f);
        if (a <= 0) continue;
        drawList->AddRect(ImVec2(min.x - e, min.y - e * 0.5f), ImVec2(max.x + e, max.y + e * 1.5f), IM_COL32(0, 0, 0, a),
                          rounding + e, s);
    }
}

void drawPopupShadows() {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiViewport* main = ImGui::GetMainViewport();
    for (ImGuiWindow* window : g.Windows) {
        if (!window->Active || window->Hidden || window->Viewport != main) continue;  // OS windows have a native shadow
        if (window->Flags & ImGuiWindowFlags_ChildWindow) continue;
        const bool popup = (window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0;
        // A panel floating over the workspace: titled, and not docked into it.
        const bool floating = !window->DockIsActive && !(window->Flags & ImGuiWindowFlags_NoTitleBar);
        if (!popup && !floating) continue;
        window->DrawList->PushClipRectFullScreen();
        softShadow(window->DrawList, window->Pos, window->Pos + window->Size, window->WindowRounding, 0.30f);
        window->DrawList->PopClipRect();
    }
}

void hudBackground(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
    const float rounding = theme::kRadiusPopup * theme::scale();
    softShadow(drawList, min, max, rounding, 0.20f, 8);
    // Near-opaque: without blur, see-through chrome over a photo is only mud.
    drawList->AddRectFilled(min, max, u32(theme::rgb(0x0A0E15, 0.94f)), rounding);
    drawList->AddRect(min, max, u32(theme::kPopupBorder), rounding);
}

// -----------------------------------------------------------------------------
// Badges
// -----------------------------------------------------------------------------

void drawStars(ImDrawList* drawList, ImVec2 topLeft, int rating, float starSize, bool showEmpty) {
    const float radius = starSize * 0.5f;
    for (int i = 0; i < 5; ++i) {
        const glm::vec2 center = glm::vec2(topLeft) + glm::vec2(radius + static_cast<float>(i) * starSize * 1.1f, radius);
        if (i < rating) {
            drawStar(drawList, center, radius, u32(theme::kStar), true);
        } else if (showEmpty) {
            drawStar(drawList, center, radius * 0.9f, u32(theme::kTextTertiary), false);
        }
    }
}

void drawFlagBadge(ImDrawList* drawList, ImVec2 center, AssetFlag flag, float size) {
    const glm::vec2 c(center);
    const float h = size * 0.5f;
    if (flag == AssetFlag::PICKED) {
        const ImU32 color = u32(theme::kPick);
        drawList->AddLine(c + glm::vec2(-h * 0.6f, -h), c + glm::vec2(-h * 0.6f, h), color, 1.5f);
        drawList->AddTriangleFilled(c + glm::vec2(-h * 0.6f, -h), c + glm::vec2(h * 0.8f, -h * 0.45f),
                                    c + glm::vec2(-h * 0.6f, h * 0.1f), color);
    } else if (flag == AssetFlag::REJECTED) {
        const ImU32 color = u32(theme::kReject);
        drawList->AddLine(c + glm::vec2(-h, -h), c + glm::vec2(h, h), color, 2.0f);
        drawList->AddLine(c + glm::vec2(-h, h), c + glm::vec2(h, -h), color, 2.0f);
    }
}

void drawColorLabelDot(ImDrawList* drawList, ImVec2 center, ColorLabel label, float radius) {
    if (label == ColorLabel::NONE) return;
    drawList->AddCircleFilled(center, radius, u32(theme::colorLabelColor(label)));
    drawList->AddCircle(center, radius, IM_COL32(0, 0, 0, 120), 0, 1.0f);
}

void drawAssetCard(ImDrawList* drawList, ImVec2 min, ImVec2 max, const AssetRecord& asset, bool selected,
                   bool hovered, bool showCaption, bool focused) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();
    const float rounding = 4.0f * s;
    if (selected) {
        drawList->AddRectFilled(min, max, u32(theme::kSelected), rounding);
    } else if (hovered) {
        drawList->AddRectFilled(min, max, u32(theme::kFillGroup, 1.6f), rounding);
    }

    // Image area, in the photo's aspect ratio (3:2 until dimensions are known).
    const float pad = std::max(5.0f * s, style.FramePadding.x * 0.75f);
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
    const glm::vec2 imageMin = glm::floor(areaMin + (area - size) * 0.5f);
    const glm::vec2 imageMax = imageMin + glm::floor(size);

    // The photo keeps square corners: rounding would hide its own pixels.
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
    const float badge = std::clamp(size.y * 0.14f, 8.0f * s, 14.0f * s);
    if (asset.flag != AssetFlag::UNFLAGGED) {
        drawFlagBadge(drawList, imageMin + glm::vec2(badge, badge), asset.flag, badge);
    }
    drawColorLabelDot(drawList, imageMax - glm::vec2(badge, size.y - badge), asset.colorLabel, badge * 0.45f);
    if (asset.rating > 0 && size.x > badge * 6.0f) {
        drawStars(drawList, glm::vec2(imageMin.x + badge * 0.4f, imageMax.y - badge * 1.3f), asset.rating, badge,
                  false);
    }
    if (asset.flag == AssetFlag::REJECTED) drawList->AddRectFilled(imageMin, imageMax, IM_COL32(0, 0, 0, 120));

    if (showCaption) {
        const glm::vec2 textPos(min.x + pad, max.y - captionHeight);
        drawList->PushClipRect(ImVec2(textPos.x, textPos.y), ImVec2(max.x - pad, max.y), true);
        drawList->AddText(textPos, u32(selected ? theme::kText : theme::kTextSecondary), asset.fileName.c_str());
        drawList->PopClipRect();
    }
    // Bright ring while the panel takes the culling keys, dim otherwise.
    if (selected) {
        drawList->AddRect(min, max, u32(focused ? theme::kAccentBright : theme::kTextTertiary), rounding, 2.0f * s);
    }
}

// -----------------------------------------------------------------------------
// Controls
// -----------------------------------------------------------------------------

float iconButtonWidth() { return controlVisual() + 2.0f * theme::scale(); }

bool iconButton(const char* id, Icon icon, const char* tooltip, bool on, bool enabled) {
    const float height = ImGui::GetFrameHeight();
    const float visual = controlVisual();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(iconButtonWidth(), height));
    ImGui::EndDisabled();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const ImVec2 half(visual * 0.5f, visual * 0.5f);
    const float rounding = ImGui::GetStyle().FrameRounding;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const bool hovered = enabled && ImGui::IsItemHovered();
    if (enabled && ImGui::IsItemActive()) {
        drawList->AddRectFilled(center - half, center + half, u32(theme::kFillActive), rounding);
    } else if (on) {
        drawList->AddRectFilled(center - half, center + half, u32(theme::kSelected), rounding);
    } else if (hovered) {
        drawList->AddRectFilled(center - half, center + half, u32(theme::kFillHovered), rounding);
    }
    const ImVec4& ink = on ? theme::kAccentBright : hovered ? theme::kText : theme::kTextSecondary;
    drawIcon(drawList, icon, center, u32(ink, enabled ? 1.0f : ImGui::GetStyle().DisabledAlpha), on);
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

bool labelButton(const char* id, Icon icon, const char* label, bool primary) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();
    const float height = ImGui::GetFrameHeight();
    const float visual = controlVisual();
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const float iconBox = 16.0f * s;
    const float width = style.FramePadding.x + iconBox + style.ItemInnerSpacing.x + textSize.x + style.FramePadding.x;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const float top = min.y + (height - visual) * 0.5f;
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec4& fill = primary ? (active ? theme::kAccentActive : hovered ? theme::kAccentHovered : theme::kAccent)
                                 : (active ? theme::kFillActive : hovered ? theme::kFillHovered : theme::kFillControl);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(ImVec2(min.x, top), ImVec2(min.x + width, top + visual), u32(fill), style.FrameRounding);
    const ImU32 ink = primary ? IM_COL32_WHITE : u32(theme::kText);
    drawIcon(drawList, icon, ImVec2(min.x + style.FramePadding.x + iconBox * 0.5f, min.y + height * 0.5f), ink);
    drawList->AddText(ImVec2(min.x + style.FramePadding.x + iconBox + style.ItemInnerSpacing.x,
                             min.y + (height - textSize.y) * 0.5f),
                      ink, label);
    return pressed;
}

bool primaryButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool segmented(const char* id, int& index, std::initializer_list<const char*> labels, float minSegmentWidth,
               bool prominent) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();
    const float height = ImGui::GetFrameHeight();
    const float visual = controlVisual();
    const float inset = (height - visual) * 0.5f;
    const float rounding = style.FrameRounding;
    const auto segmentWidth = [&](const char* label) {
        return std::max(minSegmentWidth, ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f);
    };

    float total = 0.0f;
    for (const char* label : labels) total += segmentWidth(label);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(ImVec2(origin.x, origin.y + inset), ImVec2(origin.x + total, origin.y + inset + visual),
                            u32(theme::kFillControl), rounding);

    bool changed = false;
    int i = 0;
    ImGui::PushID(id);
    ImGui::BeginGroup();
    for (const char* label : labels) {
        const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
        const float width = segmentWidth(label);
        if (i > 0) ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##segment", ImVec2(width, height));
        ImGui::PopID();
        const ImVec2 min = ImGui::GetItemRectMin();
        const bool selected = index == i;
        const bool hovered = ImGui::IsItemHovered();
        if (selected) {
            drawList->AddRectFilled(ImVec2(min.x + 2.0f * s, min.y + inset + 2.0f * s),
                                    ImVec2(min.x + width - 2.0f * s, min.y + inset + visual - 2.0f * s),
                                    u32(prominent ? theme::kAccent : theme::kFillActive), rounding - 2.0f * s);
        }
        const ImU32 ink = selected ? (prominent ? IM_COL32_WHITE : u32(theme::kText))
                                   : u32(hovered ? theme::kText : theme::kTextSecondary);
        drawList->AddText(ImVec2(min.x + (width - textSize.x) * 0.5f, min.y + (height - textSize.y) * 0.5f), ink, label,
                          ImGui::FindRenderedTextEnd(label));
        if (pressed && !selected) {
            index = i;
            changed = true;
        }
        ++i;
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

float pillWidth(const char* text) {
    const SmallText small;
    return ImGui::CalcTextSize(text).x + ImGui::GetFontSize() * 0.9f;
}

void pill(const char* text, const ImVec4& tint) {
    const float rowHeight = ImGui::GetFrameHeight();  // centred on a control row
    const SmallText small;
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float em = ImGui::GetFontSize();
    const ImVec2 size(textSize.x + em * 0.9f, em * 1.25f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size.x, rowHeight));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 min(pos.x, std::floor(pos.y + (rowHeight - size.y) * 0.5f));
    drawList->AddRectFilled(min, min + size, u32(tint, 0.16f), size.y * 0.5f);
    drawList->AddText(ImVec2(min.x + (size.x - textSize.x) * 0.5f, min.y + (size.y - textSize.y) * 0.5f), u32(tint), text);
}

bool miniCheckbox(const char* id, bool& value, const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float box = std::floor(ImGui::GetFontSize() * 0.93f);
    const ImVec2 labelSize = label ? ImGui::CalcTextSize(label) : ImVec2(0.0f, 0.0f);
    const float width = box + (label ? style.ItemInnerSpacing.x + labelSize.x : 0.0f);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
    if (pressed) value = !value;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 boxMin(min.x, std::floor(min.y + (height - box) * 0.5f));
    const ImVec2 boxMax(boxMin.x + box, boxMin.y + box);
    const float rounding = 4.0f * theme::scale();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(boxMin, boxMax,
                            u32(value ? theme::kAccent : ImGui::IsItemHovered() ? theme::kFillHovered : theme::kFillControl),
                            rounding);
    if (value) {
        drawIcon(drawList, Icon::CHECK, (boxMin + boxMax) * 0.5f, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
    } else {
        drawList->AddRect(boxMin, boxMax, u32(theme::kTextTertiary, 0.7f), rounding);
    }
    if (label) {
        drawList->AddText(ImVec2(boxMax.x + style.ItemInnerSpacing.x, min.y + (height - labelSize.y) * 0.5f),
                          u32(theme::kText), label);
    }
    return pressed;
}

bool ratingWidget(const char* id, int& rating, float starSize) {
    const float size = starSize > 0.0f ? starSize : ImGui::GetFontSize() * 1.05f;
    const float height = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size * 1.1f * 5.0f, height));
    int shown = rating;
    int hoveredStar = 0;
    if (ImGui::IsItemHovered()) {
        const float x = ImGui::GetIO().MousePos.x - origin.x;
        hoveredStar = std::clamp(static_cast<int>(x / (size * 1.1f)) + 1, 1, 5);
        shown = hoveredStar;
        ImGui::SetTooltip("%d star%s (click again to clear)", hoveredStar, hoveredStar == 1 ? "" : "s");
    }
    drawStars(ImGui::GetWindowDrawList(), ImVec2(origin.x, origin.y + (height - size) * 0.5f), shown, size);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && hoveredStar > 0) {
        rating = hoveredStar == rating ? 0 : hoveredStar;
        return true;
    }
    return false;
}

namespace {

float labelSwatchRadius() { return ImGui::GetFontSize() * 0.36f; }
float labelSwatchCell() { return labelSwatchRadius() * 2.0f + ImGui::GetStyle().ItemInnerSpacing.x; }

}  // namespace

float colorLabelPickerWidth(bool allowAny) {
    return labelSwatchCell() * static_cast<float>(static_cast<int>(ColorLabel::PURPLE) + (allowAny ? 2 : 1));
}

bool colorLabelPicker(const char* id, int& label, bool allowAny) {
    ImGui::PushID(id);
    bool changed = false;
    const float radius = labelSwatchRadius();
    const float cell = labelSwatchCell();
    const float height = ImGui::GetFrameHeight();
    const float stroke = 1.3f * theme::scale();
    for (int value = allowAny ? -1 : 0; value <= static_cast<int>(ColorLabel::PURPLE); ++value) {
        ImGui::PushID(value);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##swatch", ImVec2(cell, height))) {
            label = (label == value && value > 0) ? (allowAny ? -1 : 0) : value;
            changed = true;
        }
        const ImVec2 center(origin.x + cell * 0.5f, origin.y + height * 0.5f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 muted = u32(theme::kTextSecondary, 0.8f);
        if (value < 0) {  // "any": a ring around a dot
            drawList->AddCircle(center, radius, muted, 0, stroke);
            drawList->AddCircleFilled(center, radius * 0.35f, muted);
        } else if (value == 0) {  // "none": a struck-through ring
            drawList->AddCircle(center, radius, muted, 0, stroke);
            drawList->AddLine(ImVec2(center.x - radius * 0.7f, center.y + radius * 0.7f),
                              ImVec2(center.x + radius * 0.7f, center.y - radius * 0.7f), muted, stroke);
        } else {
            drawColorLabelDot(drawList, center, static_cast<ColorLabel>(value), radius);
        }
        // A neutral ring, so the selection reads on every label colour, blue included.
        if (label == value) drawList->AddCircle(center, radius + 2.5f * theme::scale(), u32(theme::kText), 0, stroke * 1.2f);
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
    static ImGuiID focusRequest = 0;  // slider whose text field opens on the next frame
    SliderResult result;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = theme::scale();
    const float height = ImGui::GetFrameHeight();
    // Wide enough for the longest label and value, so the columns line up.
    const float labelWidth = ImGui::CalcTextSize("Temperature").x + style.ItemSpacing.x * 1.5f;
    const float valueWidth = ImGui::CalcTextSize("+0.00 EV").x + 2.0f * s;
    const bool modified = value != defaultValue;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImGui::PushID(label);

    const float rowStart = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);
    ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));  // "Name##id" shows "Name"
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Double-click to reset");
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && modified) {
            value = defaultValue;
            result.changed = result.released = true;
        }
    }
    ImGui::SameLine(rowStart + labelWidth);

    const float trackWidth =
        std::max(ImGui::GetContentRegionAvail().x - valueWidth - style.ItemInnerSpacing.x, ImGui::GetFontSize() * 3.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float centerY = std::floor(origin.y + height * 0.5f);
    const float half = 2.0f * s;
    const ImVec2 trackMin(origin.x + 2.0f, centerY - half);
    const ImVec2 trackMax(origin.x + trackWidth - 2.0f, centerY + half);
    if (gradientLeft != 0 || gradientRight != 0) {
        drawList->AddRectFilledMultiColor(trackMin, trackMax, gradientLeft, gradientRight, gradientRight, gradientLeft);
    } else {
        drawList->AddRectFilled(trackMin, trackMax, u32(theme::kFillActive), half);
        if (!(flags & ImGuiSliderFlags_Logarithmic) && max > min) {
            // Fill from the default value to the knob: how far the setting is from neutral.
            // Mirrors where ImGui::SliderBehavior puts the grab.
            const auto knobX = [&](float v) {
                const float t = std::clamp((v - min) / (max - min), 0.0f, 1.0f);
                return origin.x + 2.0f + style.GrabMinSize * 0.5f + t * (trackWidth - 4.0f - style.GrabMinSize);
            };
            const float from = knobX(defaultValue);
            const float to = knobX(value);
            if (std::abs(to - from) >= 1.0f) {
                drawList->AddRectFilled(ImVec2(std::min(from, to), trackMin.y), ImVec2(std::max(from, to), trackMax.y),
                                        u32(theme::kAccentBright), half);
            }
        }
    }

    // The native slider supplies the behaviour and the knob; its frame and its
    // own centred value text are hidden unless it has turned into a text field.
    const ImGuiID sliderId = ImGui::GetID("##value");
    const bool typing = ImGui::TempInputIsActive(sliderId);
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, typing ? theme::kFillActive : clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, typing ? theme::kFillActive : clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, typing ? theme::kFillActive : clear);
    ImGui::PushStyleColor(ImGuiCol_Text, typing ? theme::kText : clear);
    if (focusRequest == sliderId) {
        ImGui::SetKeyboardFocusHere();  // keyboard focus on a slider opens its text field
        focusRequest = 0;
    }
    ImGui::SetNextItemWidth(trackWidth);
    result.changed |= ImGui::SliderFloat("##value", &value, min, max, format, flags);
    result.released |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleColor(4);

    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    char text[32];
    std::snprintf(text, sizeof text, format, static_cast<double>(value));
    const ImVec2 slot = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##edit", ImVec2(valueWidth, height))) focusRequest = sliderId;
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        ImGui::SetItemTooltip("Click to type a value");
    }
    if (!typing) {
        drawList->AddText(ImVec2(slot.x + valueWidth - ImGui::CalcTextSize(text).x, slot.y + style.FramePadding.y),
                          u32(modified ? theme::kText : theme::kTextSecondary), text);
    }
    ImGui::PopID();
    return result;
}

// -----------------------------------------------------------------------------
// Compact slider, colour wheel
// -----------------------------------------------------------------------------

SliderResult compactSlider(const char* id, float& value, float min, float max, float defaultValue, const char* format,
                           float width, ImGuiSliderFlags flags) {
    SliderResult result;
    ImGui::SetNextItemWidth(width);
    result.changed = ImGui::SliderFloat(id, &value, min, max, format, flags);
    result.released = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && value != defaultValue) {
        value = defaultValue;
        result.changed = result.released = true;
    }
    return result;
}

ImU32 oklchColor(float lightness, float chroma, float hueDegrees, float alpha) {
    const float angle = hueDegrees * 3.14159265f / 180.0f;
    const Rgb linear = oklabToLinearSrgb({lightness, chroma * std::cos(angle), chroma * std::sin(angle)});
    auto encode = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return v;
    };
    return ImGui::ColorConvertFloat4ToU32(ImVec4(encode(linear[0]), encode(linear[1]), encode(linear[2]), alpha));
}

SliderResult colorWheel(const char* id, float& hue, float& saturation, float diameter) {
    SliderResult result;
    ImGui::PushID(id);
    const glm::vec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wheel", ImVec2(diameter, diameter));
    const ImGuiID itemId = ImGui::GetItemID();
    const float radius = diameter * 0.5f - 2.0f;
    const glm::vec2 center = origin + glm::vec2(diameter * 0.5f);

    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Screen y points down; hue angles run counter-clockwise from +x.
        const glm::vec2 offset = glm::vec2(ImGui::GetIO().MousePos) - center;
        const float distance = std::min(glm::length(offset) / radius, 1.0f);
        float angle = std::atan2(-offset.y, offset.x) * 180.0f / 3.14159265f;
        if (angle < 0.0f) angle += 360.0f;
        const float newSaturation = distance * 100.0f;
        if (newSaturation != saturation || angle != hue) {
            hue = angle;
            saturation = newSaturation;
            result.changed = true;
            ImGui::MarkItemEdited(itemId);
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        saturation = 0.0f;
        result.changed = result.released = true;
    }
    result.released |= ImGui::IsItemDeactivatedAfterEdit();

    // Disc: neutral centre fading to the tint colours at the rim.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    constexpr int kSegments = 72;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImU32 centreColour = oklchColor(0.62f, 0.0f, 0.0f);
    drawList->PrimReserve(kSegments * 3, kSegments * 3);
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = static_cast<float>(i) / kSegments * 360.0f;
        const float a1 = static_cast<float>(i + 1) / kSegments * 360.0f;
        const auto rim = [&](float degrees) {
            const float r = degrees * 3.14159265f / 180.0f;
            return ImVec2(center.x + radius * std::cos(r), center.y - radius * std::sin(r));
        };
        const auto base = static_cast<ImDrawIdx>(drawList->_VtxCurrentIdx);
        drawList->PrimWriteVtx(ImVec2(center.x, center.y), uv, centreColour);
        drawList->PrimWriteVtx(rim(a0), uv, oklchColor(0.62f, 0.13f, a0));
        drawList->PrimWriteVtx(rim(a1), uv, oklchColor(0.62f, 0.13f, a1));
        drawList->PrimWriteIdx(base);
        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    }
    drawList->AddCircle(ImVec2(center.x, center.y), radius, IM_COL32(0, 0, 0, 140), kSegments, 1.0f);

    // Handle at (hue, saturation).
    const float angle = hue * 3.14159265f / 180.0f;
    const glm::vec2 handle = center + glm::vec2(std::cos(angle), -std::sin(angle)) * (radius * saturation / 100.0f);
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    drawList->AddCircleFilled(ImVec2(handle.x, handle.y), hot ? 6.0f : 5.0f, oklchColor(0.7f, 0.13f * saturation / 100.0f, hue));
    drawList->AddCircle(ImVec2(handle.x, handle.y), hot ? 6.0f : 5.0f, IM_COL32(255, 255, 255, 230), 16, 1.5f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hue %.0f, saturation %.0f\nDouble-click to reset", hue, saturation);
    ImGui::PopID();
    return result;
}

// -----------------------------------------------------------------------------
// Curve editor
// -----------------------------------------------------------------------------

SliderResult curveEditor(const char* id, Curve& curve, ImU32 color, float size,
                         std::span<const std::pair<const Curve*, ImU32>> others) {
    SliderResult result;
    ImGui::PushID(id);
    constexpr float kHandle = 5.0f;     // point radius, pixels
    constexpr float kPick = 9.0f;       // how close a click must be to grab a point
    constexpr float kGap = 2.0f / 256;  // minimum x distance between neighbours
    const glm::vec2 origin = glm::vec2(ImGui::GetCursorScreenPos()) + glm::vec2(kHandle);
    const float side = std::max(size - 2.0f * kHandle, 32.0f);
    ImGui::InvisibleButton("##curve", ImVec2(side + 2.0f * kHandle, side + 2.0f * kHandle),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImGuiID itemId = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    auto toScreen = [&](CurvePoint p) { return origin + glm::vec2(p.x, 1.0f - p.y) * side; };
    auto toCurve = [&](glm::vec2 s) {
        const glm::vec2 v = (s - origin) / side;
        return CurvePoint{std::clamp(v.x, 0.0f, 1.0f), std::clamp(1.0f - v.y, 0.0f, 1.0f)};
    };

    // An identity curve is edited as its two corners.
    if (curve.count < 2) {
        curve = Curve{};
        curve.count = 2;
        curve.points[0] = {0.0f, 0.0f};
        curve.points[1] = {1.0f, 1.0f};
    }
    const glm::vec2 mouse = ImGui::GetIO().MousePos;
    auto nearest = [&]() {
        int best = -1;
        float bestDistance = kPick;
        for (std::uint32_t i = 0; i < curve.count; ++i) {
            const float distance = glm::length(toScreen(curve.points[i]) - mouse);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = static_cast<int>(i);
            }
        }
        return best;
    };

    // The dragged point and the grab offset live in ImGui's state storage.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID dragKey = ImGui::GetID("drag");
    const ImGuiID offsetXKey = ImGui::GetID("offsetX");
    const ImGuiID offsetYKey = ImGui::GetID("offsetY");
    int dragged = storage->GetInt(dragKey, -1);

    if (hovered && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        const int hit = nearest();
        if (hit > 0 && hit + 1 < static_cast<int>(curve.count)) {
            for (std::uint32_t i = static_cast<std::uint32_t>(hit); i + 1 < curve.count; ++i) curve.points[i] = curve.points[i + 1];
            --curve.count;
            result.changed = result.released = true;
        }
        dragged = -1;
    } else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dragged = nearest();
        glm::vec2 grab(0.0f);
        if (dragged < 0 && curve.count < kMaxCurvePoints) {
            // A new point on the curve below the cursor, between its neighbours.
            const CurvePoint at = toCurve(mouse);
            std::uint32_t slot = 0;
            while (slot < curve.count && curve.points[slot].x < at.x) ++slot;
            const bool roomLeft = slot == 0 || at.x - curve.points[slot - 1].x >= kGap;
            const bool roomRight = slot == curve.count || curve.points[slot].x - at.x >= kGap;
            if (slot > 0 && slot < curve.count && roomLeft && roomRight) {
                const CurvePoint onCurve{at.x, evaluateCurve(curve, at.x)};
                for (std::uint32_t i = curve.count; i > slot; --i) curve.points[i] = curve.points[i - 1];
                curve.points[slot] = onCurve;
                ++curve.count;
                dragged = static_cast<int>(slot);
                result.changed = true;
                ImGui::MarkItemEdited(itemId);
            }
        }
        if (dragged >= 0) grab = toScreen(curve.points[static_cast<std::size_t>(dragged)]) - mouse;
        storage->SetFloat(offsetXKey, grab.x);
        storage->SetFloat(offsetYKey, grab.y);
    }
    if (dragged >= 0 && dragged < static_cast<int>(curve.count) && ImGui::IsItemActive() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const glm::vec2 grab(storage->GetFloat(offsetXKey), storage->GetFloat(offsetYKey));
        CurvePoint moved = toCurve(mouse + grab);
        const auto i = static_cast<std::size_t>(dragged);
        const float lo = i == 0 ? 0.0f : curve.points[i - 1].x + kGap;
        const float hi = i + 1 == curve.count ? 1.0f : curve.points[i + 1].x - kGap;
        moved.x = std::clamp(moved.x, std::min(lo, hi), std::max(lo, hi));
        if (!(moved == curve.points[i])) {
            curve.points[i] = moved;
            result.changed = true;
            ImGui::MarkItemEdited(itemId);
        }
    }
    if (!ImGui::IsItemActive()) dragged = -1;
    storage->SetInt(dragKey, dragged);
    result.released |= ImGui::IsItemDeactivatedAfterEdit();

    // Background, quarter grid and the identity diagonal.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const glm::vec2 end = origin + glm::vec2(side);
    drawList->AddRectFilled(origin, end, IM_COL32(24, 24, 26, 255));
    for (int q = 1; q < 4; ++q) {
        const float t = side * static_cast<float>(q) / 4.0f;
        drawList->AddLine(ImVec2(origin.x + t, origin.y), ImVec2(origin.x + t, end.y), IM_COL32(255, 255, 255, 22));
        drawList->AddLine(ImVec2(origin.x, origin.y + t), ImVec2(end.x, origin.y + t), IM_COL32(255, 255, 255, 22));
    }
    drawList->AddLine(ImVec2(origin.x, end.y), ImVec2(end.x, origin.y), IM_COL32(255, 255, 255, 40));
    drawList->AddRect(origin, end, IM_COL32(255, 255, 255, 40));

    // Curves, sampled every other pixel.
    const int samples = std::max(static_cast<int>(side / 2.0f), 16);
    std::vector<ImVec2> line(static_cast<std::size_t>(samples) + 1);
    auto drawCurve = [&](const Curve& c, ImU32 lineColor, float thickness) {
        for (int s = 0; s <= samples; ++s) {
            const float x = static_cast<float>(s) / static_cast<float>(samples);
            const glm::vec2 point = toScreen({x, evaluateCurve(c, x)});
            line[static_cast<std::size_t>(s)] = ImVec2(point.x, point.y);
        }
        drawList->AddPolyline(line.data(), static_cast<int>(line.size()), lineColor, thickness);
    };
    drawList->PushClipRect(origin - glm::vec2(1.0f), end + glm::vec2(1.0f), true);
    for (const auto& [other, otherColor] : others) {
        if (other && !isIdentity(*other)) drawCurve(*other, (otherColor & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 90), 1.0f);
    }
    drawCurve(curve, color, 2.0f);
    drawList->PopClipRect();

    const int hot = hovered && !ImGui::IsItemActive() ? nearest() : dragged;
    for (std::uint32_t i = 0; i < curve.count; ++i) {
        const glm::vec2 p = toScreen(curve.points[i]);
        const bool highlight = static_cast<int>(i) == hot;
        drawList->AddCircleFilled(ImVec2(p.x, p.y), highlight ? kHandle + 1.0f : kHandle, IM_COL32(20, 20, 22, 255));
        drawList->AddCircle(ImVec2(p.x, p.y), highlight ? kHandle + 1.0f : kHandle, color, 16, 1.5f);
    }
    if (hovered || ImGui::IsItemActive()) {
        const CurvePoint at = dragged >= 0 ? curve.points[static_cast<std::size_t>(dragged)] : toCurve(mouse);
        const float output = dragged >= 0 ? at.y : evaluateCurve(curve, at.x);
        ImGui::SetTooltip("Input %.0f%%  ->  Output %.0f%%\nClick to add a point, double-click a point to remove it",
                          at.x * 100.0f, output * 100.0f);
    }
    ImGui::PopID();
    return result;
}

// -----------------------------------------------------------------------------
// Layout
// -----------------------------------------------------------------------------

void sectionLabel(const char* text) {
    const float s = theme::scale();
    const float height = 22.0f * s;
    const SmallText small;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, height));
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 6.0f * s, pos.y + height - ImGui::GetFontSize() - 3.0f * s),
                                        u32(theme::kTextSecondary), text);
}

bool sectionHeader(const char* title, bool& open, float trailingWidth) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (ImGui::GetCursorPosY() > ImGui::GetCursorStartPos().y + 1.0f) {  // hairline between sections, not above the first
        const float y = std::floor(pos.y - style.ItemSpacing.y * 0.5f);
        drawList->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + width, y), u32(theme::kSeparator));
    }
    ImGui::PushID(title);
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##header", ImVec2(std::max(width - trailingWidth, 1.0f), height))) open = !open;
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const float em = ImGui::GetFontSize();
    drawIcon(drawList, open ? Icon::CHEVRON_DOWN : Icon::CHEVRON_RIGHT, ImVec2(pos.x + em * 0.4f, pos.y + height * 0.5f),
             u32(hovered ? theme::kText : theme::kTextSecondary));
    drawList->AddText(ImVec2(pos.x + em * 1.1f, pos.y + style.FramePadding.y), u32(theme::kText), title);
    return open;
}

void alignRight(float width) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - width, 0.0f));
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
