// DarkHouse — proposed design tokens, style and reusable widgets (Dear ImGui 1.92 docking).
// Proof of concept for the UI review. Compiled and exercised by proof.cpp against the
// project's own ImGui sources; meant to be folded into ui/theme and ui/widgets.
#pragma once

#include <imgui.h>
#include <imgui_internal.h>  // TempInputIsActive, window list for popup shadows

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace dh {

// -----------------------------------------------------------------------------
// Tokens (unscaled; GuiEngine's style.ScaleAllSizes() applies the display scale)
// -----------------------------------------------------------------------------

inline constexpr float kControlHeight = 24.0f;  // buttons, fields, sliders, list rows
inline constexpr float kRowPitch = 28.0f;       // control + gap: also the pointer target
inline constexpr float kToolbarHeight = 28.0f;
inline constexpr float kFontBody = 15.0f;       // ImGui sizes are line heights: 15 px is a 12.8 px em (13 pt)
inline constexpr float kFontSmall = 13.0f;      // 11 pt: secondary labels, counts, section titles
inline constexpr float kRadiusControl = 5.0f;
inline constexpr float kRadiusGroup = 6.0f;
inline constexpr float kRadiusPopup = 8.0f;

constexpr ImVec4 hex(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}
constexpr ImVec4 white(float alpha) { return ImVec4(1.0f, 1.0f, 1.0f, alpha); }
constexpr ImVec4 black(float alpha) { return ImVec4(0.0f, 0.0f, 0.0f, alpha); }

// Surfaces are neutral (R = G = B) so the chrome never tints how a photo is
// judged; fills and text are white or black at an alpha, so they sit correctly
// on any surface.
struct Palette {
    ImVec4 canvas, window, bar, popup;                                    // surfaces, darkest to most raised
    ImVec4 fillGroup, fillControl, fillHover, fillActive, fillSelected;   // translucent fills
    ImVec4 separator, popupBorder;
    ImVec4 text, textSecondary, textTertiary;
    ImVec4 accent, accentHover, accentActive;  // focus, primary action, progress: where input is going
    ImVec4 selectionRing;                      // selected thumbnail; neutral so it cannot be read as a colour label
    ImVec4 danger, warning;
    float shadowAlpha;
};

inline constexpr Palette kDark{
    .canvas = hex(0x141414), .window = hex(0x1F1F1F), .bar = hex(0x262626), .popup = hex(0x2C2C2C, 0.98f),
    .fillGroup = white(0.04f), .fillControl = white(0.09f), .fillHover = white(0.13f), .fillActive = white(0.20f),
    .fillSelected = white(0.12f),
    .separator = white(0.09f), .popupBorder = white(0.14f),
    .text = white(0.88f), .textSecondary = white(0.60f), .textTertiary = white(0.36f),
    .accent = hex(0xE3543D), .accentHover = hex(0xF56B52), .accentActive = hex(0xC74530),
    .selectionRing = white(0.92f),
    .danger = hex(0xFF5A52), .warning = hex(0xF5B83D),
    .shadowAlpha = 0.15f,  // dark surfaces need a stronger shadow than light ones to read at all
};

inline constexpr Palette kLight{
    .canvas = hex(0xD9D9D9), .window = hex(0xF4F4F4), .bar = hex(0xE9E9E9), .popup = hex(0xFBFBFB, 0.98f),
    .fillGroup = black(0.035f), .fillControl = black(0.07f), .fillHover = black(0.11f), .fillActive = black(0.17f),
    .fillSelected = black(0.10f),
    .separator = black(0.11f), .popupBorder = black(0.16f),
    .text = black(0.88f), .textSecondary = black(0.62f), .textTertiary = black(0.45f),
    .accent = hex(0xC8412B), .accentHover = hex(0xD9523B), .accentActive = hex(0xAB3521),
    .selectionRing = black(0.85f),
    .danger = hex(0xD12F26), .warning = hex(0x9A6700),
    .shadowAlpha = 0.05f,
};

inline const Palette* gPalette = &kDark;
[[nodiscard]] inline const Palette& P() noexcept { return *gPalette; }
[[nodiscard]] inline ImU32 U32(const ImVec4& color, float alphaMul = 1.0f) {
    return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w * alphaMul));
}

// -----------------------------------------------------------------------------
// Style
// -----------------------------------------------------------------------------

inline void applyStyle(ImGuiStyle& style, const Palette& p, float fontSize = kFontBody) {
    gPalette = &p;

    // 4 / 8 grid. A control is 24 tall whatever the font, and rows sit on a 28 pitch.
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, (kControlHeight - fontSize) * 0.5f);
    style.ItemSpacing = ImVec2(8.0f, kRowPitch - kControlHeight);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 16.0f;
    style.TouchExtraPadding = ImVec2(0.0f, 2.0f);  // 24 px visuals, 28 px targets: no dead gap between rows
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 8.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 0.0f;    // no coloured stripe over panel titles
    style.DockingSeparatorSize = 1.0f;  // hairline; the grab zone stays WindowBorderHoverPadding wide
    style.SeparatorTextBorderSize = 1.0f;

    style.WindowRounding = 0.0f;  // docked panels read as one surface
    style.ChildRounding = kRadiusGroup;
    style.FrameRounding = kRadiusControl;
    style.PopupRounding = kRadiusPopup;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = kRadiusControl;

    // Panel headers: a title, nothing else. Panels are shown and hidden from
    // the toolbar toggles and the View menu.
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.DockingNodeHasCloseButton = false;
    style.TabCloseButtonMinWidthSelected = 0.0f;       // close button appears on hover only
    style.TabCloseButtonMinWidthUnselected = FLT_MAX;  // and never on background tabs
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.DisabledAlpha = 0.40f;

    ImVec4* c = style.Colors;
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const auto alpha = [](ImVec4 color, float a) { return ImVec4(color.x, color.y, color.z, a); };
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textTertiary;  // only for content that really is unavailable
    c[ImGuiCol_WindowBg] = p.window;
    c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_PopupBg] = p.popup;
    c[ImGuiCol_Border] = p.separator;
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_FrameBg] = p.fillControl;
    c[ImGuiCol_FrameBgHovered] = p.fillHover;
    c[ImGuiCol_FrameBgActive] = p.fillActive;
    c[ImGuiCol_TitleBg] = p.window;
    c[ImGuiCol_TitleBgActive] = p.window;
    c[ImGuiCol_TitleBgCollapsed] = p.window;
    c[ImGuiCol_MenuBarBg] = p.bar;
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = alpha(p.text, 0.18f);
    c[ImGuiCol_ScrollbarGrabHovered] = alpha(p.text, 0.30f);
    c[ImGuiCol_ScrollbarGrabActive] = alpha(p.text, 0.42f);
    c[ImGuiCol_CheckMark] = white(1.0f);
    c[ImGuiCol_CheckboxSelectedBg] = p.accent;
    c[ImGuiCol_SliderGrab] = alpha(p.text, 0.80f);
    c[ImGuiCol_SliderGrabActive] = alpha(p.text, 1.0f);
    c[ImGuiCol_Button] = p.fillControl;
    c[ImGuiCol_ButtonHovered] = p.fillHover;
    c[ImGuiCol_ButtonActive] = p.fillActive;
    c[ImGuiCol_Header] = p.fillSelected;
    c[ImGuiCol_HeaderHovered] = alpha(p.text, 0.07f);
    c[ImGuiCol_HeaderActive] = p.fillActive;
    c[ImGuiCol_Separator] = p.separator;
    c[ImGuiCol_SeparatorHovered] = alpha(p.accent, 0.70f);
    c[ImGuiCol_SeparatorActive] = p.accent;
    c[ImGuiCol_ResizeGrip] = clear;
    c[ImGuiCol_ResizeGripHovered] = alpha(p.accent, 0.60f);
    c[ImGuiCol_ResizeGripActive] = p.accent;
    c[ImGuiCol_InputTextCursor] = p.text;
    c[ImGuiCol_Tab] = clear;
    c[ImGuiCol_TabHovered] = alpha(p.text, 0.07f);
    c[ImGuiCol_TabSelected] = alpha(p.text, 0.08f);
    c[ImGuiCol_TabSelectedOverline] = clear;
    c[ImGuiCol_TabDimmed] = clear;
    c[ImGuiCol_TabDimmedSelected] = alpha(p.text, 0.05f);
    c[ImGuiCol_TabDimmedSelectedOverline] = clear;
    c[ImGuiCol_DockingPreview] = alpha(p.accent, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = p.canvas;
    c[ImGuiCol_TableHeaderBg] = p.fillGroup;
    c[ImGuiCol_TableBorderStrong] = p.separator;
    c[ImGuiCol_TableBorderLight] = alpha(p.text, 0.05f);
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = alpha(p.text, 0.025f);
    c[ImGuiCol_TextLink] = p.accentHover;
    c[ImGuiCol_TextSelectedBg] = alpha(p.accent, 0.40f);
    c[ImGuiCol_TreeLines] = alpha(p.text, 0.12f);
    c[ImGuiCol_DragDropTarget] = p.accent;
    c[ImGuiCol_NavCursor] = p.accent;  // keyboard focus ring
    c[ImGuiCol_ModalWindowDimBg] = black(0.45f);
}

// -----------------------------------------------------------------------------
// Depth
// -----------------------------------------------------------------------------

// Soft ambient shadow outside a rounded rectangle, biased downwards. ImGui has
// no blur, so it is a stack of one-pixel rings with a quadratic falloff.
inline void softShadow(ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding, float alpha, int blur = 14) {
    for (int i = 0; i < blur; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(blur);
        const float e = static_cast<float>(i + 1);
        const int a = static_cast<int>(alpha * (1.0f - t) * (1.0f - t) * 255.0f);
        if (a <= 0) continue;
        drawList->AddRect(ImVec2(min.x - e, min.y - e * 0.5f), ImVec2(max.x + e, max.y + e * 1.5f), IM_COL32(0, 0, 0, a),
                          rounding + e, 1.0f);
    }
}

// Call once per frame, after every window has been submitted and before
// ImGui::Render(): gives each menu, combo, context menu, tooltip and modal the
// same shadow. The rings lie outside the popup, in its own draw list, so they
// are layered exactly like the popup itself.
inline void drawPopupShadows() {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiViewport* main = ImGui::GetMainViewport();
    for (ImGuiWindow* window : g.Windows) {
        if (!window->Active || window->Hidden || window->Viewport != main) continue;  // OS windows have a native shadow
        if (!(window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))) continue;
        if (window->Flags & ImGuiWindowFlags_ChildWindow) continue;
        window->DrawList->PushClipRectFullScreen();
        softShadow(window->DrawList, window->Pos, window->Pos + window->Size, window->WindowRounding, P().shadowAlpha);
        window->DrawList->PopClipRect();
    }
}

// Floating overlay over the canvas (zoom controls, read-outs).
inline void hudBackground(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
    softShadow(drawList, min, max, kRadiusPopup, 0.11f, 8);
    drawList->AddRectFilled(min, max, IM_COL32(24, 24, 24, 240), kRadiusPopup);  // near-opaque: without blur, see-through chrome over a photo is mud
    drawList->AddRect(min, max, IM_COL32(255, 255, 255, 26), kRadiusPopup);
}

// -----------------------------------------------------------------------------
// Icons. Stand-ins drawn with the draw list; the app should merge an icon font
// (see the review) and keep these names.
// -----------------------------------------------------------------------------

enum class Icon {
    Plus, Trash, ArrowUp, ArrowDown, Eye, EyeOff, Reset, ChevronRight, ChevronDown, Search,
    SidebarLeft, SidebarRight, PanelBottom, Flag, XMark, Import, Open, Copy, Check,
};

inline void drawIcon(ImDrawList* drawList, Icon icon, ImVec2 center, ImU32 color, bool filled = false) {
    const float s = ImGui::GetFontSize() / kFontBody;  // icons are drawn in a 16-unit box
    const float stroke = 1.4f * s;
    const auto at = [&](float x, float y) { return ImVec2(center.x + x * s, center.y + y * s); };
    const auto path = [&](std::initializer_list<ImVec2> points, bool closed = false) {
        for (const ImVec2& point : points) drawList->PathLineTo(at(point.x, point.y));
        drawList->PathStroke(color, stroke, closed ? ImDrawFlags_Closed : ImDrawFlags_None);
    };
    switch (icon) {
    case Icon::Plus: path({{-5, 0}, {5, 0}}); path({{0, -5}, {0, 5}}); break;
    case Icon::Trash:
        path({{-5.5f, -4}, {5.5f, -4}});
        path({{-2, -4}, {-2, -6}, {2, -6}, {2, -4}});
        path({{-4, -4}, {-3.2f, 6}, {3.2f, 6}, {4, -4}});
        break;
    case Icon::ArrowUp: path({{0, 5.5f}, {0, -5}}); path({{-4, -1}, {0, -5}, {4, -1}}); break;
    case Icon::ArrowDown: path({{0, -5.5f}, {0, 5}}); path({{-4, 1}, {0, 5}, {4, 1}}); break;
    case Icon::Eye:
    case Icon::EyeOff:
        drawList->PathLineTo(at(-7, 0));
        drawList->PathBezierQuadraticCurveTo(at(0, -8), at(7, 0));
        drawList->PathBezierQuadraticCurveTo(at(0, 8), at(-7, 0));
        drawList->PathStroke(color, stroke, ImDrawFlags_Closed);
        drawList->AddCircleFilled(center, 2.1f * s, color);
        if (icon == Icon::EyeOff) path({{-6, -6}, {6, 6}});
        break;
    case Icon::Reset:
        drawList->PathArcTo(center, 5.0f * s, -2.2f, 2.9f, 24);
        drawList->PathStroke(color, stroke);
        path({{-6.4f, -4.8f}, {-2.9f, -4.0f}, {-3.6f, -7.6f}}, true);
        break;
    case Icon::ChevronRight: path({{-2, -4}, {2, 0}, {-2, 4}}); break;
    case Icon::ChevronDown: path({{-4, -2}, {0, 2}, {4, -2}}); break;
    case Icon::Search:
        drawList->AddCircle(at(-1, -1), 4.3f * s, color, 0, stroke);
        path({{2.2f, 2.2f}, {6, 6}});
        break;
    case Icon::SidebarLeft:
    case Icon::SidebarRight:
    case Icon::PanelBottom: {
        drawList->AddRect(at(-7, -5), at(7, 5), color, 2.0f * s, stroke);
        const ImVec2 a = icon == Icon::SidebarLeft ? at(-7, -5) : icon == Icon::SidebarRight ? at(2.5f, -5) : at(-7, 1.5f);
        const ImVec2 b = icon == Icon::SidebarLeft ? at(-2.5f, 5) : at(7, 5);
        if (filled) drawList->AddRectFilled(a, b, color, 2.0f * s);
        if (icon == Icon::SidebarLeft) path({{-2.5f, -5}, {-2.5f, 5}});
        if (icon == Icon::SidebarRight) path({{2.5f, -5}, {2.5f, 5}});
        if (icon == Icon::PanelBottom) path({{-7, 1.5f}, {7, 1.5f}});
        break;
    }
    case Icon::Flag:
        path({{-4, -6}, {-4, 6}});
        if (filled) {
            for (const ImVec2& point : {ImVec2(-4, -5), ImVec2(5, -5), ImVec2(2.5f, -2), ImVec2(5, 1), ImVec2(-4, 1)}) {
                drawList->PathLineTo(at(point.x, point.y));
            }
            drawList->PathFillConcave(color);
        }
        path({{-4, -5}, {5, -5}, {2.5f, -2}, {5, 1}, {-4, 1}});
        break;
    case Icon::XMark: path({{-4, -4}, {4, 4}}); path({{-4, 4}, {4, -4}}); break;
    case Icon::Import:
        path({{0, -6.5f}, {0, 2}});
        path({{-3.2f, -1.2f}, {0, 2}, {3.2f, -1.2f}});
        path({{-6, 1}, {-6, 6}, {6, 6}, {6, 1}});
        break;
    case Icon::Open: path({{-4, 4}, {4, -4}}); path({{-1.5f, -4}, {4, -4}, {4, 1.5f}}); break;
    case Icon::Copy:
        drawList->AddRect(at(-2.5f, -2.5f), at(6, 6), color, 1.5f * s, stroke);
        path({{-6, 2}, {-6, -6}, {2, -6}});
        break;
    case Icon::Check: path({{-4, 0}, {-1, 3}, {4, -3.5f}}); break;
    }
}

// -----------------------------------------------------------------------------
// Controls
// -----------------------------------------------------------------------------

// Square icon button: no fill until hovered, `on` for a latched toggle.
// The target is a full frame (28 px with TouchExtraPadding); the glyph is 16.
inline bool iconButton(const char* id, Icon icon, const char* tooltip = nullptr, bool on = false, bool enabled = true) {
    const float size = ImGui::GetFrameHeight();
    const float visual = std::min(size, ImGui::GetFontSize() * (kControlHeight / kFontBody));
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(visual + 2.0f, size));
    ImGui::EndDisabled();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const ImVec2 half(visual * 0.5f, visual * 0.5f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const bool hovered = enabled && ImGui::IsItemHovered();
    if (ImGui::IsItemActive()) {
        drawList->AddRectFilled(center - half, center + half, U32(P().fillActive), kRadiusControl);
    } else if (hovered || on) {
        drawList->AddRectFilled(center - half, center + half, U32(on ? P().fillSelected : P().fillHover), kRadiusControl);
    }
    drawIcon(drawList, icon, center, U32(on || hovered ? P().text : P().textSecondary, enabled ? 1.0f : 0.4f), on);
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

// Segmented control ("pick one"). Every segment is a real item, so Tab and
// arrow-key navigation, activation and the focus ring work as for any button.
// Returns true when `index` changes.
inline bool segmented(const char* id, int& index, std::initializer_list<const char*> labels, float minSegmentWidth = 0.0f) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();  // the target; the visual is at most one control high
    const float visual = std::min(height, ImGui::GetFontSize() * (kControlHeight / kFontBody));
    const float inset = (height - visual) * 0.5f;
    const float rounding = style.FrameRounding;

    float total = 0.0f;
    for (const char* label : labels) {
        total += std::max(minSegmentWidth, ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f);
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(ImVec2(origin.x, origin.y + inset), ImVec2(origin.x + total, origin.y + inset + visual),
                            U32(P().fillControl), rounding);

    bool changed = false;
    int i = 0;
    ImGui::PushID(id);
    ImGui::BeginGroup();
    for (const char* label : labels) {
        const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
        const float width = std::max(minSegmentWidth, textSize.x + style.FramePadding.x * 2.0f);
        if (i > 0) ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##segment", ImVec2(width, height));
        ImGui::PopID();
        const ImVec2 min = ImGui::GetItemRectMin();
        const bool selected = index == i;
        const bool hovered = ImGui::IsItemHovered();
        if (selected) {
            drawList->AddRectFilled(ImVec2(min.x + 2.0f, min.y + inset + 2.0f),
                                    ImVec2(min.x + width - 2.0f, min.y + inset + visual - 2.0f), U32(P().fillActive),
                                    rounding - 2.0f);
        }
        drawList->AddText(ImVec2(min.x + (width - textSize.x) * 0.5f, min.y + (height - textSize.y) * 0.5f),
                          U32(selected || hovered ? P().text : P().textSecondary), label,
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

// Status pill: a tinted label for state ("Preview", "Mask", "3 failed"), never for actions.
inline void pill(const char* text, const ImVec4& tint) {
    ImGui::PushFont(nullptr, kFontSmall);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float em = ImGui::GetFontSize();
    const ImVec2 size(textSize.x + em * 0.9f, em * 1.25f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float offsetY = (ImGui::GetFrameHeight() - size.y) * 0.5f;  // centred on a control row
    ImGui::Dummy(ImVec2(size.x, ImGui::GetFrameHeight()));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 min(pos.x, std::floor(pos.y + offsetY));
    drawList->AddRectFilled(min, min + size, U32(tint, 0.16f), size.y * 0.5f);
    drawList->AddText(ImVec2(min.x + (size.x - textSize.x) * 0.5f, min.y + (size.y - textSize.y) * 0.5f), U32(tint), text);
    ImGui::PopFont();
}

// Small checkbox for dense rows and section headers (accent fill, white tick).
inline bool miniCheckbox(const char* id, bool& value, const char* label = nullptr) {
    const float height = ImGui::GetFrameHeight();
    const float box = std::floor(ImGui::GetFontSize() * 0.93f);
    const ImVec2 labelSize = label ? ImGui::CalcTextSize(label) : ImVec2(0.0f, 0.0f);
    const float width = box + (label ? ImGui::GetStyle().ItemInnerSpacing.x + labelSize.x : 0.0f);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
    if (pressed) value = !value;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 boxMin(min.x, std::floor(min.y + (height - box) * 0.5f));
    const ImVec2 boxMax(boxMin.x + box, boxMin.y + box);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(boxMin, boxMax, U32(value ? P().accent : ImGui::IsItemHovered() ? P().fillHover : P().fillControl),
                            4.0f);
    if (!value) drawList->AddRect(boxMin, boxMax, U32(P().textTertiary, 0.6f), 4.0f);
    if (value) drawIcon(drawList, Icon::Check, (boxMin + boxMax) * 0.5f, IM_COL32_WHITE);
    if (label) {
        drawList->AddText(ImVec2(boxMax.x + ImGui::GetStyle().ItemInnerSpacing.x, min.y + (height - labelSize.y) * 0.5f),
                          U32(P().text), label);
    }
    return pressed;
}

struct SliderResult {
    bool changed = false;   // value changed this frame (while dragging too)
    bool released = false;  // an edit finished this frame: persist now
};

// Inspector row: label | 4 px track | value. The value sits in its own
// right-aligned slot instead of on the track under the knob, so it never
// moves and nothing covers it. Behaviour comes from the native slider (drag,
// arrow keys, Cmd/Ctrl+click to type); clicking the value also starts typing.
// Double-clicking the label resets. A gradient replaces the plain track for
// hue, temperature and tint.
inline SliderResult sliderRow(const char* label, float& value, float min, float max, float defaultValue, const char* format,
                              ImU32 gradientLeft = 0, ImU32 gradientRight = 0,
                              ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    static ImGuiID focusRequest = 0;
    SliderResult result;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const float height = ImGui::GetFrameHeight();
    const float labelWidth = em * 6.2f;  // fits "Temperature"
    const float valueWidth = em * 3.9f;  // fits "+0.00 EV" and "12000 K"
    const bool modified = value != defaultValue;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImGui::PushID(label);

    const float rowStart = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, P().textSecondary);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && modified) {
        value = defaultValue;
        result.changed = result.released = true;
    }
    ImGui::SameLine(rowStart + labelWidth);

    const float trackWidth = std::max(ImGui::GetContentRegionAvail().x - valueWidth - style.ItemInnerSpacing.x, em * 3.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float centerY = std::floor(origin.y + height * 0.5f);
    const ImVec2 trackMin(origin.x + 2.0f, centerY - 2.0f);
    const ImVec2 trackMax(origin.x + trackWidth - 2.0f, centerY + 2.0f);
    if (gradientLeft != 0 || gradientRight != 0) {
        drawList->AddRectFilledMultiColor(trackMin, trackMax, gradientLeft, gradientRight, gradientRight, gradientLeft);
    } else {
        drawList->AddRectFilled(trackMin, trackMax, U32(P().fillActive), 2.0f);
        if (!(flags & ImGuiSliderFlags_Logarithmic) && max > min) {
            // Fill from the default value to the knob: how far the setting is from neutral.
            const auto knobX = [&](float v) {
                const float t = std::clamp((v - min) / (max - min), 0.0f, 1.0f);
                return origin.x + 2.0f + style.GrabMinSize * 0.5f + t * (trackWidth - 4.0f - style.GrabMinSize);
            };
            const float from = knobX(defaultValue);
            const float to = knobX(value);
            if (std::abs(to - from) >= 1.0f) {
                drawList->AddRectFilled(ImVec2(std::min(from, to), trackMin.y), ImVec2(std::max(from, to), trackMax.y),
                                        U32(P().text, 0.62f), 2.0f);
            }
        }
    }

    const ImGuiID sliderId = ImGui::GetID("##value");
    const bool typing = ImGui::TempInputIsActive(sliderId);
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, typing ? P().fillActive : clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, typing ? P().fillActive : clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, typing ? P().fillActive : clear);
    ImGui::PushStyleColor(ImGuiCol_Text, typing ? P().text : clear);  // the slider's own centred value
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
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    if (!typing) {
        drawList->AddText(ImVec2(slot.x + valueWidth - ImGui::CalcTextSize(text).x, slot.y + style.FramePadding.y),
                          U32(modified ? P().text : P().textSecondary), text);
    }
    ImGui::PopID();
    return result;
}

// Inspector section header: disclosure chevron, title, and trailing controls
// supplied by the caller (reset, enable, a status pill) on the same row.
// Returns whether the section is open. `trailingWidth` reserves room on the right.
inline bool sectionHeader(const char* title, bool& open, float trailingWidth = 0.0f) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (ImGui::GetCursorPosY() > ImGui::GetCursorStartPos().y + 1.0f) {  // hairline between sections, not above the first
        drawList->AddLine(ImVec2(pos.x, pos.y - style.ItemSpacing.y * 0.5f),
                          ImVec2(pos.x + width, pos.y - style.ItemSpacing.y * 0.5f), U32(P().separator));
    }
    ImGui::PushID(title);
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##header", ImVec2(std::max(width - trailingWidth, 1.0f), height))) open = !open;
    ImGui::PopID();
    const float em = ImGui::GetFontSize();
    drawIcon(drawList, open ? Icon::ChevronDown : Icon::ChevronRight, ImVec2(pos.x + em * 0.4f, pos.y + height * 0.5f),
             U32(P().textSecondary));
    drawList->AddText(ImVec2(pos.x + em * 1.1f, pos.y + style.FramePadding.y), U32(P().text), title);
    return open;
}

// Moves the cursor so the next `width` pixels end at the right edge of the row just submitted.
inline void alignRight(float width) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - width, 0.0f));
}

}  // namespace dh
