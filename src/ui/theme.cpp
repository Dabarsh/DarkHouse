#include "ui/theme.hpp"

#include "darkhouse_ui_font.hpp"

#include <cstdio>

namespace darkhouse::ui::theme {

ImVec4 colorLabelColor(ColorLabel label) noexcept {
    switch (label) {
    case ColorLabel::NONE: return {0.45f, 0.45f, 0.47f, 1.0f};
    case ColorLabel::RED: return {0.86f, 0.26f, 0.24f, 1.0f};
    case ColorLabel::YELLOW: return {0.93f, 0.79f, 0.24f, 1.0f};
    case ColorLabel::GREEN: return {0.36f, 0.72f, 0.36f, 1.0f};
    case ColorLabel::BLUE: return {0.30f, 0.52f, 0.90f, 1.0f};
    case ColorLabel::PURPLE: return {0.62f, 0.40f, 0.86f, 1.0f};
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

namespace {

constexpr ImVec4 grey(float value, float alpha = 1.0f) { return ImVec4(value, value, value, alpha); }
constexpr ImVec4 withAlpha(ImVec4 color, float alpha) { return ImVec4(color.x, color.y, color.z, alpha); }

}  // namespace

void applyStyle(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarOverlineSize = 2.0f;
    style.DockingSeparatorSize = 2.0f;

    style.WindowRounding = 0.0f;  // docked panels read as one surface
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = grey(0.86f);
    c[ImGuiCol_TextDisabled] = grey(0.50f);
    c[ImGuiCol_WindowBg] = grey(0.145f);
    c[ImGuiCol_ChildBg] = grey(0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = grey(0.12f, 0.98f);
    c[ImGuiCol_Border] = grey(0.07f);
    c[ImGuiCol_BorderShadow] = grey(0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = grey(0.21f);
    c[ImGuiCol_FrameBgHovered] = grey(0.26f);
    c[ImGuiCol_FrameBgActive] = grey(0.30f);
    c[ImGuiCol_TitleBg] = grey(0.11f);
    c[ImGuiCol_TitleBgActive] = grey(0.13f);
    c[ImGuiCol_TitleBgCollapsed] = grey(0.11f);
    c[ImGuiCol_MenuBarBg] = grey(0.11f);
    c[ImGuiCol_ScrollbarBg] = grey(0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = grey(0.30f);
    c[ImGuiCol_ScrollbarGrabHovered] = grey(0.38f);
    c[ImGuiCol_ScrollbarGrabActive] = grey(0.46f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_CheckboxSelectedBg] = grey(0.21f);
    c[ImGuiCol_SliderGrab] = grey(0.62f);
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Button] = grey(0.24f);
    c[ImGuiCol_ButtonHovered] = grey(0.30f);
    c[ImGuiCol_ButtonActive] = grey(0.36f);
    c[ImGuiCol_Header] = grey(0.25f);
    c[ImGuiCol_HeaderHovered] = grey(0.29f);
    c[ImGuiCol_HeaderActive] = grey(0.33f);
    c[ImGuiCol_Separator] = grey(0.22f);
    c[ImGuiCol_SeparatorHovered] = withAlpha(kAccent, 0.70f);
    c[ImGuiCol_SeparatorActive] = kAccent;
    c[ImGuiCol_ResizeGrip] = grey(0.30f, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(kAccent, 0.60f);
    c[ImGuiCol_ResizeGripActive] = kAccent;
    c[ImGuiCol_InputTextCursor] = grey(0.92f);
    c[ImGuiCol_Tab] = grey(0.12f);
    c[ImGuiCol_TabHovered] = grey(0.28f);
    c[ImGuiCol_TabSelected] = grey(0.19f);
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = grey(0.11f);
    c[ImGuiCol_TabDimmedSelected] = grey(0.16f);
    c[ImGuiCol_TabDimmedSelectedOverline] = grey(0.42f);
    c[ImGuiCol_DockingPreview] = withAlpha(kAccent, 0.55f);
    c[ImGuiCol_DockingEmptyBg] = grey(0.09f);
    c[ImGuiCol_TableHeaderBg] = grey(0.18f);
    c[ImGuiCol_TableBorderStrong] = grey(0.10f);
    c[ImGuiCol_TableBorderLight] = grey(0.18f);
    c[ImGuiCol_TableRowBg] = grey(0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = grey(1.0f, 0.03f);
    c[ImGuiCol_TextLink] = kAccentHovered;
    c[ImGuiCol_TextSelectedBg] = withAlpha(kAccent, 0.35f);
    c[ImGuiCol_TreeLines] = grey(0.30f);
    c[ImGuiCol_DragDropTarget] = kAccent;
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = grey(0.0f, 0.55f);
}

void loadFonts(ImGuiIO& io, ImGuiStyle& style) {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;  // static storage in the executable
    config.OversampleH = 2;
    std::snprintf(config.Name, sizeof config.Name, "Roboto Medium");
    constexpr float kBaseSize = 15.0f;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(generated::kUiFontTtf),
                                   static_cast<int>(sizeof generated::kUiFontTtf), kBaseSize, &config);
    style.FontSizeBase = kBaseSize;
}

}  // namespace darkhouse::ui::theme
