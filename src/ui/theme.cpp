#include "ui/theme.hpp"

#include "darkhouse_ui_font.hpp"

#include <cfloat>
#include <cstdio>

namespace darkhouse::ui::theme {

ImVec4 colorLabelColor(ColorLabel label) noexcept {
    switch (label) {
    case ColorLabel::NONE: return kTextTertiary;
    case ColorLabel::RED: return rgb(0xE5484D);
    case ColorLabel::YELLOW: return rgb(0xEDC93D);
    case ColorLabel::GREEN: return rgb(0x5CB85C);
    case ColorLabel::BLUE: return rgb(0x4D9FE6);
    case ColorLabel::PURPLE: return rgb(0xA873E0);
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
}

const char* colorLabelName(ColorLabel label) noexcept {
    switch (label) {
    case ColorLabel::NONE: return "None";
    case ColorLabel::RED: return "Red";
    case ColorLabel::YELLOW: return "Yellow";
    case ColorLabel::GREEN: return "Green";
    case ColorLabel::BLUE: return "Blue";
    case ColorLabel::PURPLE: return "Purple";
    }
    return "?";
}

ImU32 u32(const ImVec4& color, float alphaMul) {
    return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w * alphaMul));
}

float scale() { return ImGui::GetStyle().FontScaleDpi; }

namespace {

constexpr ImVec4 withAlpha(ImVec4 color, float alpha) { return ImVec4(color.x, color.y, color.z, alpha); }

}  // namespace

void applyStyle(ImGuiStyle& style) {
    // 4 / 8 grid. A control is 24 tall whatever the font, and rows sit on a 28 pitch.
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, (kControlHeight - kFontBody) * 0.5f);
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
    style.TabBarOverlineSize = 0.0f;
    style.DockingSeparatorSize = 1.0f;  // hairline; the grab zone stays WindowBorderHoverPadding wide
    style.SeparatorTextBorderSize = 1.0f;

    style.WindowRounding = 0.0f;  // docked panels read as one surface
    style.ChildRounding = kRadiusGroup;
    style.FrameRounding = kRadiusControl;
    style.PopupRounding = kRadiusPopup;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = kRadiusControl;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.DisabledAlpha = 0.40f;

    // Panel headers carry a title and nothing else: panels are shown and
    // hidden from the toolbar toggles and View > Panels.
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.DockingNodeHasCloseButton = false;
    style.TabCloseButtonMinWidthSelected = 0.0f;       // close button on hover only
    style.TabCloseButtonMinWidthUnselected = FLT_MAX;  // and never on background tabs

    ImVec4* c = style.Colors;
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextTertiary;
    c[ImGuiCol_WindowBg] = kWindow;
    c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_PopupBg] = kPopup;
    c[ImGuiCol_Border] = kSeparator;
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_FrameBg] = kFillControl;
    c[ImGuiCol_FrameBgHovered] = kFillHovered;
    c[ImGuiCol_FrameBgActive] = kFillActive;
    c[ImGuiCol_TitleBg] = kWindow;
    c[ImGuiCol_TitleBgActive] = kWindow;
    c[ImGuiCol_TitleBgCollapsed] = kWindow;
    c[ImGuiCol_MenuBarBg] = kBar;
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = rgb(kTint, 0.22f);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(kTint, 0.34f);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(kTint, 0.46f);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c[ImGuiCol_CheckboxSelectedBg] = kAccent;
    c[ImGuiCol_SliderGrab] = withAlpha(kText, 0.88f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c[ImGuiCol_Button] = kFillControl;
    c[ImGuiCol_ButtonHovered] = kFillHovered;
    c[ImGuiCol_ButtonActive] = kFillActive;
    c[ImGuiCol_Header] = kSelected;
    c[ImGuiCol_HeaderHovered] = rgb(kTint, 0.12f);
    c[ImGuiCol_HeaderActive] = withAlpha(kAccent, 0.45f);
    c[ImGuiCol_Separator] = kSeparator;
    c[ImGuiCol_SeparatorHovered] = withAlpha(kAccentBright, 0.70f);
    c[ImGuiCol_SeparatorActive] = kAccentBright;
    c[ImGuiCol_ResizeGrip] = clear;
    c[ImGuiCol_ResizeGripHovered] = withAlpha(kAccentBright, 0.60f);
    c[ImGuiCol_ResizeGripActive] = kAccentBright;
    c[ImGuiCol_InputTextCursor] = kText;
    c[ImGuiCol_Tab] = clear;
    c[ImGuiCol_TabHovered] = rgb(kTint, 0.12f);
    c[ImGuiCol_TabSelected] = rgb(kTint, 0.10f);
    c[ImGuiCol_TabSelectedOverline] = clear;
    c[ImGuiCol_TabDimmed] = clear;
    c[ImGuiCol_TabDimmedSelected] = rgb(kTint, 0.06f);
    c[ImGuiCol_TabDimmedSelectedOverline] = clear;
    c[ImGuiCol_DockingPreview] = withAlpha(kAccent, 0.50f);
    c[ImGuiCol_DockingEmptyBg] = kCanvas;
    c[ImGuiCol_PlotLines] = kAccentBright;
    c[ImGuiCol_PlotLinesHovered] = kText;
    c[ImGuiCol_PlotHistogram] = kAccentBright;
    c[ImGuiCol_PlotHistogramHovered] = kText;
    c[ImGuiCol_TableHeaderBg] = kFillGroup;
    c[ImGuiCol_TableBorderStrong] = kSeparator;
    c[ImGuiCol_TableBorderLight] = rgb(kTint, 0.07f);
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = rgb(kTint, 0.03f);
    c[ImGuiCol_TextLink] = kAccentBright;
    c[ImGuiCol_TextSelectedBg] = withAlpha(kAccent, 0.50f);
    c[ImGuiCol_TreeLines] = kSeparator;
    c[ImGuiCol_DragDropTarget] = kAccentBright;
    c[ImGuiCol_NavCursor] = kAccentBright;  // keyboard focus ring
    c[ImGuiCol_NavWindowingHighlight] = withAlpha(kAccentBright, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.60f);
}

void loadFonts(ImGuiIO& io, ImGuiStyle& style) {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;  // static storage in the executable
    config.OversampleH = 2;
    std::snprintf(config.Name, sizeof config.Name, "Roboto Medium");
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(generated::kUiFontTtf),
                                   static_cast<int>(sizeof generated::kUiFontTtf), kFontBody, &config);
    style.FontSizeBase = kFontBody;
}

}  // namespace darkhouse::ui::theme
