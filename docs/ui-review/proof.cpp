// Headless proof for the DarkHouse UI review: builds the proposed Canvas
// workspace with real Dear ImGui (docking) against DarkHouse's own ImGui build,
// drives it with scripted mouse input, and rasterises the frame to a PNG.
//
// Not part of the DarkHouse build. From the repository root, after a normal
// CMake configure has fetched the dependencies into build/_deps:
//
//   c++ -std=c++20 -O2 '-DIMGUI_USER_CONFIG="darkhouse_imconfig.hpp"' -Iinclude \
//       -isystem build/_deps/imgui-src -isystem build/_deps/glm-src -isystem build/_deps/stb-src \
//       docs/ui-review/proof.cpp build/_deps/imgui-src/imgui.cpp build/_deps/imgui-src/imgui_draw.cpp \
//       build/_deps/imgui-src/imgui_tables.cpp build/_deps/imgui-src/imgui_widgets.cpp -o /tmp/dh_proof
//   /tmp/dh_proof build/_deps/imgui-src/misc/fonts/Roboto-Medium.ttf docs/ui-review
//
// Add `light` as a third argument for the light palette. Exits non-zero if an
// interaction check fails.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "dh_design.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using dh::Icon;
using dh::P;
using dh::U32;

namespace {

constexpr float kWidth = 1632.0f;  // the logical size of the window in docs/images/workspace-canvas.png
constexpr float kHeight = 918.0f;
constexpr int kScale = 2;

std::map<std::string, ImRect> gRects;  // item rectangles the input script aims at
void mark(const char* key) { gRects[key] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }

struct State {
    int workspace = 1;
    bool showLeft = true, showFilmstrip = true, showRight = true;
    float exposure = 0.64f, contrast = 0.0f, highlights = -24.0f, shadows = 0.0f;
    float luminance = 50.0f, colour = 50.0f, detail = 20.0f;
    bool toneOpen = true, noiseOpen = true, balanceOpen = false, hslOpen = false;
    bool noiseEnabled = true, noiseAuto = true;
    int zoom = 0;
    bool filterPicks = false, filterRejects = false;
    int selectedLayer = 0;
    int rating = 3;
    bool picked = false, rejected = false;
    float opacity = 100.0f;
    char search[64] = "";
    bool contextMenu = false;
    int contrastReleases = 0;
};

struct Small {
    Small() { ImGui::PushFont(nullptr, dh::kFontSmall); }
    ~Small() { ImGui::PopFont(); }
    Small(const Small&) = delete;
    Small& operator=(const Small&) = delete;
};

void star(ImDrawList* drawList, ImVec2 center, float radius, ImU32 color, bool filled) {
    std::array<ImVec2, 10> points;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const float r = (i % 2 == 0) ? radius : radius * 0.45f;
        const float angle = -1.5708f + static_cast<float>(i) * 0.6283185f;
        points[i] = ImVec2(center.x + r * std::cos(angle), center.y + r * std::sin(angle));
    }
    if (filled) {
        for (std::size_t i = 0; i < points.size(); ++i) {
            drawList->AddTriangleFilled(center, points[i], points[(i + 1) % points.size()], color);
        }
    } else {
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, 1.0f, ImDrawFlags_Closed);
    }
}

void stars(int rating, float size) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(size * 5.5f, height));
    for (int i = 0; i < 5; ++i) {
        const ImVec2 center(pos.x + size * 0.5f + static_cast<float>(i) * size * 1.1f, pos.y + height * 0.5f);
        star(ImGui::GetWindowDrawList(), center, size * 0.5f, i < rating ? IM_COL32(250, 199, 71, 255) : U32(P().textTertiary),
             i < rating);
    }
}

constexpr std::array<ImU32, 5> kLabels{IM_COL32(219, 66, 61, 255), IM_COL32(237, 201, 61, 255), IM_COL32(92, 184, 92, 255),
                                       IM_COL32(77, 133, 230, 255), IM_COL32(158, 102, 219, 255)};

void labelDots(int selected, bool withNone) {
    const float height = ImGui::GetFrameHeight();
    const float radius = ImGui::GetFontSize() * 0.36f;
    const float cell = radius * 2.0f + 6.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const int count = static_cast<int>(kLabels.size()) + (withNone ? 1 : 0);
    ImGui::Dummy(ImVec2(cell * static_cast<float>(count), height));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int i = 0; i < count; ++i) {
        const ImVec2 center(pos.x + cell * (static_cast<float>(i) + 0.5f), pos.y + height * 0.5f);
        const int label = withNone ? i - 1 : i;
        if (label < 0) {
            drawList->AddCircle(center, radius, U32(P().textTertiary), 0, 1.2f);
            drawList->AddLine(ImVec2(center.x - radius * 0.7f, center.y + radius * 0.7f),
                              ImVec2(center.x + radius * 0.7f, center.y - radius * 0.7f), U32(P().textTertiary), 1.2f);
        } else {
            drawList->AddCircleFilled(center, radius, kLabels[static_cast<std::size_t>(label)]);
        }
        // Selection is a neutral ring, so it reads on every label colour.
        if (i == selected) drawList->AddCircle(center, radius + 2.5f, U32(P().text), 0, 1.5f);
    }
}

void sectionLabel(const char* text) {
    const Small small;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 22.0f));
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 6.0f, pos.y + 22.0f - ImGui::GetFontSize() - 3.0f),
                                        U32(P().textSecondary), text);
}

bool sourceRow(const char* label, int count, bool selected, float indent = 0.0f, int chevron = 0) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(width, height));
    ImGui::PopID();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (selected) {
        drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), U32(P().fillSelected), style.FrameRounding);
    } else if (ImGui::IsItemHovered()) {
        drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), U32(P().text, 0.05f), style.FrameRounding);
    }
    float x = pos.x + 6.0f + indent;
    if (chevron != 0) {
        dh::drawIcon(drawList, chevron == 2 ? Icon::ChevronDown : Icon::ChevronRight, ImVec2(x + 5.0f, pos.y + height * 0.5f),
                     U32(P().textSecondary));
    }
    x += 16.0f;
    drawList->AddText(ImVec2(x, pos.y + style.FramePadding.y), U32(P().text), label);
    char text[16];
    std::snprintf(text, sizeof text, "%d", count);
    const Small small;
    const ImVec2 size = ImGui::CalcTextSize(text);
    drawList->AddText(ImVec2(pos.x + width - size.x - 8.0f, pos.y + (height - size.y) * 0.5f),
                      U32(count > 0 ? P().textSecondary : P().textTertiary), text);
    return pressed;
}

// -----------------------------------------------------------------------------
// Toolbar: menus, the primary action, workspace switcher, activity, panel toggles
// -----------------------------------------------------------------------------

bool toolbarButton(const char* id, Icon icon, const char* label, bool primary = false) {
    const float height = ImGui::GetFrameHeight();
    const float visual = dh::kControlHeight;
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const float width = 8.0f + 16.0f + 6.0f + textSize.x + 10.0f;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const float top = min.y + (height - visual) * 0.5f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec4 fill = primary ? (ImGui::IsItemHovered() ? P().accentHover : P().accent)
                                : (ImGui::IsItemHovered() ? P().fillHover : P().fillControl);
    drawList->AddRectFilled(ImVec2(min.x, top), ImVec2(min.x + width, top + visual), U32(fill), dh::kRadiusControl);
    const ImU32 ink = primary ? IM_COL32_WHITE : U32(P().text);
    dh::drawIcon(drawList, icon, ImVec2(min.x + 16.0f, min.y + height * 0.5f), ink);
    drawList->AddText(ImVec2(min.x + 30.0f, min.y + (height - textSize.y) * 0.5f), ink, label);
    return pressed;
}

void drawToolbar(State& s) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x, (dh::kToolbarHeight - ImGui::GetFontSize()) * 0.5f));
    if (ImGui::BeginMainMenuBar()) {
        for (const char* menu : {"File", "View", "Help"}) {
            if (ImGui::BeginMenu(menu)) {
                ImGui::MenuItem("Placeholder");
                ImGui::EndMenu();
            }
        }
        toolbarButton("##import", Icon::Import, "Import");
        mark("import");

        // Workspace switcher, centred in the window.
        const float segment = 76.0f;
        const float barWidth = ImGui::GetWindowWidth();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (barWidth - segment * 3.0f) * 0.5f));
        dh::segmented("##workspace", s.workspace, {"Catalog", "Canvas", "Split"}, segment);
        mark("workspace");

        // Trailing: activity, then the three panel toggles.
        const char* activity = "Importing 128 of 342";
        ImVec2 activitySize;
        {
            const Small small;
            activitySize = ImGui::CalcTextSize(activity);
        }
        const float toggles = 3.0f * (dh::kControlHeight + 2.0f) + 2.0f * 2.0f;
        const float trailing = activitySize.x + 8.0f + 64.0f + 16.0f + toggles + 8.0f;
        ImGui::SetCursorPosX(barWidth - trailing);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetFrameHeight();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        {
            const Small small;
            drawList->AddText(ImVec2(pos.x, pos.y + (height - activitySize.y) * 0.5f), U32(P().textSecondary), activity);
        }
        const ImVec2 barMin(pos.x + activitySize.x + 8.0f, pos.y + height * 0.5f - 2.0f);
        drawList->AddRectFilled(barMin, ImVec2(barMin.x + 64.0f, barMin.y + 4.0f), U32(P().fillActive), 2.0f);
        drawList->AddRectFilled(barMin, ImVec2(barMin.x + 64.0f * 128.0f / 342.0f, barMin.y + 4.0f), U32(P().accent), 2.0f);
        ImGui::Dummy(ImVec2(activitySize.x + 8.0f + 64.0f + 8.0f, height));

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
        if (dh::iconButton("##left", Icon::SidebarLeft, "Library sidebar", s.showLeft)) s.showLeft = !s.showLeft;
        if (dh::iconButton("##film", Icon::PanelBottom, "Filmstrip", s.showFilmstrip)) s.showFilmstrip = !s.showFilmstrip;
        if (dh::iconButton("##right", Icon::SidebarRight, "Inspector", s.showRight)) s.showRight = !s.showRight;
        ImGui::PopStyleVar();
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
}

// -----------------------------------------------------------------------------
// Left: source list and Info
// -----------------------------------------------------------------------------

void drawCollections() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    sectionLabel("Library");
    sourceRow("All Photographs", 24, true);
    sourceRow("Imported This Session", 0, false);
    sourceRow("Picks", 0, false);
    sourceRow("Rejected", 0, false);
    sourceRow("Unrated", 19, false);
    sectionLabel("Folders");
    sourceRow("DarkHouse Samples", 24, false, 0.0f, 2);
    sourceRow("2023", 10, false, 16.0f, 1);
    sourceRow("2024", 14, false, 16.0f, 1);
    sectionLabel("Smart Collections");
    sourceRow("Five Stars", 2, false);
    sourceRow("High ISO (3200+)", 8, false);
    sourceRow("Wide Angle (< 24 mm)", 2, false);
    sourceRow("Telephoto (135 mm+)", 3, false);
    ImGui::PopStyleVar();
}

void keyValue(const char* key, const char* value) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetFontSize() + 5.0f;
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, line));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddText(ImVec2(pos.x + 6.0f, pos.y + 2.0f), U32(P().textSecondary), key);
    drawList->AddText(ImVec2(pos.x + 78.0f, pos.y + 2.0f), U32(P().text), value);
}

void drawInfo(State& s) {
    const ImGuiStyle& style = ImGui::GetStyle();
    // Title with the panel's actions on the same row, so they never scroll away.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("DSC_1010.jpg");
    dh::alignRight(2.0f * (dh::kControlHeight + 2.0f) + 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, style.ItemSpacing.y));
    dh::iconButton("##open", Icon::Open, "Open in Canvas  (Return)");
    ImGui::SameLine();
    dh::iconButton("##copy", Icon::Copy, "Copy path");
    ImGui::PopStyleVar();

    // Culling on one row: rating, flag, colour label.
    stars(s.rating, 15.0f);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, style.ItemSpacing.y));
    dh::iconButton("##pick", Icon::Flag, "Pick  (P)", s.picked);
    ImGui::SameLine();
    dh::iconButton("##reject", Icon::XMark, "Reject  (X)", s.rejected);
    ImGui::PopStyleVar();
    ImGui::SameLine(0.0f, 6.0f);
    labelDots(0, true);

    // Exposure strip: four values on one line instead of four table rows.
    {
        const Small small;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = dh::kControlHeight + 2.0f;
        ImGui::Dummy(ImVec2(width, height));
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), U32(P().fillGroup), dh::kRadiusGroup);
        drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), U32(P().separator), dh::kRadiusGroup);
        const std::array<const char*, 4> cells{"ISO 12800", "16 mm", "f/5.6", "1/250 s"};
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const float cell = width / static_cast<float>(cells.size());
            const ImVec2 size = ImGui::CalcTextSize(cells[i]);
            const float x = pos.x + cell * static_cast<float>(i);
            drawList->AddText(ImVec2(x + (cell - size.x) * 0.5f, pos.y + (height - size.y) * 0.5f), U32(P().text), cells[i]);
            if (i > 0) drawList->AddLine(ImVec2(x, pos.y + 5.0f), ImVec2(x, pos.y + height - 5.0f), U32(P().separator));
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    sectionLabel("Camera");
    {
        const Small small;
        keyValue("Body", "NIKON Z 8");
        keyValue("Lens", "NIKKOR Z 24-70mm f/2.8 S");
        keyValue("Size", "600 \xC3\x97 400  \xC2\xB7  JPG");
    }
    sectionLabel("Dates");
    {
        const Small small;
        keyValue("Captured", "2024-11-04  11:14");
        keyValue("Imported", "2026-09-28  19:00 UTC");
    }
    sectionLabel("File");
    {
        const Small small;
        keyValue("Path", "\xE2\x80\xA6/Samples/2024/DSC_1010.jpg");
        keyValue("Hash", "9f3c61d2a07be21a");
    }
    ImGui::PopStyleVar();
}

// -----------------------------------------------------------------------------
// Centre: viewport and filmstrip
// -----------------------------------------------------------------------------

void drawViewport(State& s) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", region);

    const float side = std::floor(std::min(region.x, region.y) * 0.94f);
    const ImVec2 min(std::floor(origin.x + (region.x - side) * 0.5f), std::floor(origin.y + (region.y - side) * 0.5f));
    const ImVec2 max(min.x + side, min.y + side);
    drawList->AddRectFilledMultiColor(min, ImVec2(max.x, min.y + side * 0.2f), IM_COL32_BLACK, IM_COL32_WHITE, IM_COL32_WHITE,
                                      IM_COL32_BLACK);
    drawList->AddRectFilled(ImVec2(min.x, min.y + side * 0.2f), ImVec2(max.x, min.y + side * 0.7f), IM_COL32(6, 6, 6, 255));
    constexpr std::array<ImU32, 24> kPatches{
        IM_COL32(72, 37, 26, 255),   IM_COL32(222, 126, 97, 255), IM_COL32(51, 79, 143, 255),  IM_COL32(40, 54, 22, 255),
        IM_COL32(98, 85, 172, 255),  IM_COL32(50, 204, 168, 255), IM_COL32(255, 86, 14, 255),  IM_COL32(32, 38, 160, 255),
        IM_COL32(210, 32, 52, 255),  IM_COL32(40, 14, 58, 255),   IM_COL32(136, 200, 22, 255), IM_COL32(255, 146, 8, 255),
        IM_COL32(12, 20, 120, 255),  IM_COL32(30, 122, 30, 255),  IM_COL32(176, 12, 16, 255),  IM_COL32(255, 224, 4, 255),
        IM_COL32(204, 32, 124, 255), IM_COL32(0, 100, 152, 255),  IM_COL32(244, 244, 242, 255), IM_COL32(200, 202, 202, 255),
        IM_COL32(150, 150, 150, 255), IM_COL32(86, 86, 86, 255),  IM_COL32(40, 40, 40, 255),   IM_COL32(12, 12, 12, 255)};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 6; ++column) {
            const float cw = side / 6.0f;
            const float ch = side * 0.5f / 4.0f;
            const ImVec2 a(min.x + cw * (static_cast<float>(column) + 0.06f),
                           min.y + side * 0.2f + ch * (static_cast<float>(row) + 0.06f));
            const ImVec2 b(min.x + cw * (static_cast<float>(column) + 0.94f),
                           min.y + side * 0.2f + ch * (static_cast<float>(row) + 0.94f));
            drawList->AddRectFilled(a, b, kPatches[static_cast<std::size_t>(row * 6 + column)]);
        }
    }
    for (int i = 0; i < 48; ++i) {
        const auto hue = [](int step) {
            float r = 0, g = 0, b = 0;
            ImGui::ColorConvertHSVtoRGB(static_cast<float>(step % 48) / 48.0f, 1.0f, 1.0f, r, g, b);
            return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
        };
        const float x0 = min.x + side * static_cast<float>(i) / 48.0f;
        const float x1 = min.x + side * static_cast<float>(i + 1) / 48.0f;
        drawList->AddRectFilledMultiColor(ImVec2(x0, min.y + side * 0.7f), ImVec2(x1, max.y), hue(i), hue(i + 1),
                                          IM_COL32_WHITE, IM_COL32_WHITE);
    }

    // Zoom controls: one floating group, top-left.
    {
        const ImVec2 pos(origin.x + 8.0f, origin.y + 8.0f);
        const float width = 2.0f * 46.0f + 8.0f + 44.0f + 8.0f;
        dh::hudBackground(drawList, pos, ImVec2(pos.x + width, pos.y + dh::kControlHeight + 8.0f));
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 4.0f, pos.y + 4.0f));
        dh::segmented("##zoom", s.zoom, {"Fit", "100%"}, 46.0f);
        mark("zoom");
        const Small small;
        const char* percent = "33%";
        const ImVec2 size = ImGui::CalcTextSize(percent);
        drawList->AddText(ImVec2(pos.x + width - 10.0f - size.x, pos.y + 4.0f + (dh::kControlHeight - size.y) * 0.5f),
                          U32(P().textSecondary), percent);
    }
    // Read-out: fixed slots, so nothing shifts while the pointer moves.
    {
        const Small small;
        const float line = ImGui::GetFontSize();
        const ImVec2 pos(origin.x + 8.0f, origin.y + region.y - line - 8.0f - 12.0f);
        const float width = 398.0f;
        dh::hudBackground(drawList, pos, ImVec2(pos.x + width, pos.y + line + 12.0f));
        float x = pos.x + 10.0f;
        const float y = pos.y + 6.0f;
        const auto put = [&](const char* text, const ImVec4& color, float advance) {
            drawList->AddText(ImVec2(x, y), U32(color), text);
            x += advance;
        };
        put("2048 \xC3\x97 2048", P().text, 88.0f);
        put("RGBA16F linear", P().textSecondary, 106.0f);
        put("1 node", P().textSecondary, 58.0f);
        drawList->AddLine(ImVec2(x - 8.0f, pos.y + 5.0f), ImVec2(x - 8.0f, pos.y + line + 7.0f), IM_COL32(255, 255, 255, 30));
        const auto slot = [&](const char* key, const char* value) {
            drawList->AddText(ImVec2(x, y), U32(P().textSecondary), key);
            const ImVec2 size = ImGui::CalcTextSize(value);
            drawList->AddText(ImVec2(x + 48.0f - size.x, y), U32(P().text), value);  // right-aligned in a 5-digit slot
            x += 62.0f;
        };
        slot("x", "934");
        slot("y", "830");
    }
}

void drawFilmstrip(State& s) {
    const ImGuiStyle& style = ImGui::GetStyle();
    // Filter bar: the Search panel's controls, one click away in every workspace.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("All Photographs");
    ImGui::SameLine(0.0f, 6.0f);
    {
        const Small small;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(P().textSecondary, "24");
    }
    ImGui::SameLine(0.0f, 16.0f);
    stars(0, 13.0f);
    ImGui::SameLine(0.0f, 2.0f);
    {
        const Small small;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(P().textTertiary, "& up");
    }
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, style.ItemSpacing.y));
    if (dh::iconButton("##picks", Icon::Flag, "Show picks", s.filterPicks)) s.filterPicks = !s.filterPicks;
    ImGui::SameLine();
    if (dh::iconButton("##rejects", Icon::XMark, "Show rejects", s.filterRejects)) s.filterRejects = !s.filterRejects;
    ImGui::PopStyleVar();
    ImGui::SameLine(0.0f, 12.0f);
    labelDots(-1, false);

    const float searchWidth = 168.0f;
    const char* sort = "Capture time";
    float sortWidth = 0.0f;
    {
        const Small small;
        sortWidth = ImGui::CalcTextSize(sort).x + 18.0f;
    }
    dh::alignRight(sortWidth + 12.0f + searchWidth);
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const Small small;
        const ImVec2 size = ImGui::CalcTextSize(sort);
        ImGui::Dummy(ImVec2(sortWidth, ImGui::GetFrameHeight()));
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddText(ImVec2(pos.x, pos.y + (dh::kControlHeight - size.y) * 0.5f), U32(P().textSecondary), sort);
        dh::drawIcon(drawList, Icon::ChevronDown, ImVec2(pos.x + size.x + 10.0f, pos.y + dh::kControlHeight * 0.5f),
                     U32(P().textSecondary));
    }
    ImGui::SameLine(0.0f, 12.0f);
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(26.0f, style.FramePadding.y));
        ImGui::SetNextItemWidth(searchWidth);
        ImGui::InputTextWithHint("##search", "Search", s.search, sizeof s.search);
        ImGui::PopStyleVar();
        dh::drawIcon(ImGui::GetWindowDrawList(), Icon::Search, ImVec2(pos.x + 13.0f, pos.y + dh::kControlHeight * 0.5f),
                     U32(P().textSecondary));
    }

    // Cells.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float cellHeight = available.y;
    const float cellWidth = std::floor(cellHeight * 1.34f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    constexpr std::array<float, 8> kHues{0.38f, 0.62f, 0.07f, 0.09f, 0.30f, 0.16f, 0.48f, 0.90f};
    constexpr std::array<int, 8> kRatings{0, 3, 4, 0, 0, 0, 5, 0};
    constexpr std::array<bool, 8> kPortrait{true, false, false, true, false, true, false, true};
    drawList->PushClipRect(origin, ImVec2(origin.x + available.x, origin.y + available.y), true);
    for (std::size_t i = 0; i < kHues.size(); ++i) {
        const ImVec2 min(origin.x + static_cast<float>(i) * (cellWidth + 4.0f), origin.y);
        const ImVec2 max(min.x + cellWidth, min.y + cellHeight);
        const bool selected = i == 1;
        if (selected) drawList->AddRectFilled(min, max, U32(P().fillSelected), 4.0f);
        const float pad = 6.0f;
        const ImVec2 area(cellWidth - pad * 2.0f, cellHeight - pad * 2.0f);
        const float aspect = kPortrait[i] ? 0.667f : 1.5f;
        ImVec2 size = area;
        if (area.x / area.y > aspect) size.x = area.y * aspect; else size.y = area.x / aspect;
        const ImVec2 imageMin(std::floor(min.x + pad + (area.x - size.x) * 0.5f), std::floor(min.y + pad + (area.y - size.y) * 0.5f));
        const ImVec2 imageMax(imageMin.x + std::floor(size.x), imageMin.y + std::floor(size.y));
        float r = 0, g = 0, b = 0;
        ImGui::ColorConvertHSVtoRGB(kHues[i], 0.38f, 0.46f, r, g, b);
        const ImU32 top = ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(r * 1.35f, 1.0f), std::min(g * 1.35f, 1.0f), std::min(b * 1.35f, 1.0f), 1.0f));
        const ImU32 bottom = ImGui::ColorConvertFloat4ToU32(ImVec4(r * 0.55f, g * 0.55f, b * 0.55f, 1.0f));
        drawList->AddRectFilledMultiColor(imageMin, imageMax, top, top, bottom, bottom);  // photo corners stay square
        if (kRatings[i] > 0) {
            for (int n = 0; n < kRatings[i]; ++n) {
                star(drawList, ImVec2(imageMin.x + 9.0f + static_cast<float>(n) * 11.0f, imageMax.y - 9.0f), 5.0f,
                     IM_COL32(250, 199, 71, 255), true);
            }
        }
        // Selection: a neutral ring. Bright while this panel has key focus, dim otherwise.
        if (selected) drawList->AddRect(min, max, U32(P().selectionRing), 4.0f, 2.0f);
        if (selected) gRects["thumb"] = ImRect(min, max);
    }
    drawList->PopClipRect();
    ImGui::Dummy(available);

    if (s.contextMenu) {
        ImGui::SetNextWindowPos(ImVec2(gRects["thumb"].Max.x + 150.0f, gRects["thumb"].Min.y - 150.0f));
        ImGui::OpenPopup("##asset");
    }
    if (ImGui::BeginPopup("##asset")) {
        {
            const Small small;
            ImGui::TextColored(P().textSecondary, "DSC_1010.jpg");
        }
        ImGui::Separator();
        ImGui::MenuItem("Open in Canvas", "Return");
        if (ImGui::BeginMenu("Rating")) ImGui::EndMenu();
        ImGui::MenuItem("Pick", "P");
        ImGui::MenuItem("Reject", "X");
        if (ImGui::BeginMenu("Colour Label")) ImGui::EndMenu();
        ImGui::Separator();
        ImGui::MenuItem("Copy Path");
        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------------
// Right: layers and adjustments
// -----------------------------------------------------------------------------

void layerRow(State& s, int index, const char* name, const char* chip, ImU32 tint, bool visible, const char* opacity = "100%") {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(index);
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##layer", ImVec2(width, height))) s.selectedLayer = index;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (s.selectedLayer == index) {
        drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), U32(P().fillSelected), style.FrameRounding);
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 2.0f, pos.y));
    dh::iconButton("##eye", visible ? Icon::Eye : Icon::EyeOff, "Visibility");
    ImGui::PopID();
    {
        const Small small;
        const ImVec2 size = ImGui::CalcTextSize(chip);
        const ImVec2 chipMin(pos.x + 34.0f, pos.y + (height - size.y - 2.0f) * 0.5f);
        const ImVec2 chipMax(chipMin.x + 30.0f, chipMin.y + size.y + 2.0f);
        drawList->AddRectFilled(chipMin, chipMax, (tint & 0x00FFFFFFu) | 0x30000000u, 4.0f);
        drawList->AddText(ImVec2(chipMin.x + (30.0f - size.x) * 0.5f, chipMin.y + 1.0f), tint, chip);
        const ImVec2 opacitySize = ImGui::CalcTextSize(opacity);
        drawList->AddText(ImVec2(pos.x + width - opacitySize.x - 8.0f, pos.y + (height - opacitySize.y) * 0.5f),
                          U32(P().textSecondary), opacity);
    }
    drawList->AddText(ImVec2(pos.x + 72.0f, pos.y + style.FramePadding.y), U32(P().text, visible ? 1.0f : 0.55f), name);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height));
}

void drawLayers(State& s) {
    const ImGuiStyle& style = ImGui::GetStyle();
    {
        const Small small;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(P().textSecondary, "Document  \xC2\xB7  4 layers");
    }
    dh::alignRight(4.0f * (dh::kControlHeight + 2.0f) + 3.0f * 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, style.ItemSpacing.y));
    dh::iconButton("##add", Icon::Plus, "Add layer");
    ImGui::SameLine();
    dh::iconButton("##delete", Icon::Trash, "Delete layer");
    ImGui::SameLine();
    dh::iconButton("##raise", Icon::ArrowUp, "Move up");
    ImGui::SameLine();
    dh::iconButton("##lower", Icon::ArrowDown, "Move down", false, false);
    ImGui::PopStyleVar();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    layerRow(s, 3, "Exposure 1", "ADJ", IM_COL32(178, 142, 240, 255), true);
    layerRow(s, 2, "Ellipse 1", "VEC", IM_COL32(110, 205, 145, 255), false, "60%");
    layerRow(s, 0, "Test Chart 1", "PX", IM_COL32(120, 170, 245, 255), true);
    layerRow(s, 1, "Background", "PX", IM_COL32(120, 170, 245, 255), true);
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));

    // Properties of the selected layer, as an inset group: blend and opacity share a row.
    const ImVec2 groupMin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->ChannelsSplit(2);
    drawList->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(groupMin.x + 8.0f, groupMin.y + 8.0f));
    ImGui::BeginGroup();
    const float inner = width - 16.0f;
    ImGui::SetNextItemWidth(inner - 96.0f - style.ItemSpacing.x);
    int blend = 0;
    ImGui::Combo("##blend", &blend, "Normal\0Multiply\0Screen\0Overlay\0Color Dodge\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    ImGui::DragFloat("##opacity", &s.opacity, 0.5f, 0.0f, 100.0f, "%.0f%%");
    ImGui::SetItemTooltip("Opacity: drag, or double-click to type");
    ImGui::Button("Add Mask");
    ImGui::SameLine();
    {
        const Small small;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(P().textTertiary, "16 of 16 tiles  \xC2\xB7  32.0 MB");
    }
    ImGui::EndGroup();
    const ImVec2 groupMax(groupMin.x + width, ImGui::GetItemRectMax().y + 8.0f);
    drawList->ChannelsSetCurrent(0);
    drawList->AddRectFilled(groupMin, groupMax, U32(P().fillGroup), dh::kRadiusGroup);
    drawList->AddRect(groupMin, groupMax, U32(P().separator), dh::kRadiusGroup);
    drawList->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(groupMin.x, groupMax.y + style.ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void drawAdjustments(State& s) {
    const float button = dh::kControlHeight + 2.0f;
    const float contentX = ImGui::GetCursorScreenPos().x;

    if (dh::sectionHeader("Tone", s.toneOpen, button)) {
        dh::alignRight(button);
        if (dh::iconButton("##resetTone", Icon::Reset, "Reset Tone")) {
            s.exposure = s.contrast = s.highlights = s.shadows = 0.0f;
        }
        dh::sliderRow("Exposure", s.exposure, -5.0f, 5.0f, 0.0f, "%+.2f EV");
        const dh::SliderResult contrast = dh::sliderRow("Contrast", s.contrast, -100.0f, 100.0f, 0.0f, "%+.0f");
        mark("contrast.value");
        gRects["contrast.track"] = ImRect(ImVec2(contentX + ImGui::GetFontSize() * 6.2f, gRects["contrast.value"].Min.y),
                                          ImVec2(gRects["contrast.value"].Min.x - 6.0f, gRects["contrast.value"].Max.y));
        gRects["contrast.label"] = ImRect(ImVec2(contentX, gRects["contrast.value"].Min.y),
                                          ImVec2(contentX + 50.0f, gRects["contrast.value"].Max.y));
        if (contrast.released) ++s.contrastReleases;
        dh::sliderRow("Highlights", s.highlights, -100.0f, 100.0f, 0.0f, "%+.0f");
        dh::sliderRow("Shadows", s.shadows, -100.0f, 100.0f, 0.0f, "%+.0f");
    }

    if (dh::sectionHeader("Noise Reduction", s.noiseOpen, button)) {
        dh::alignRight(button - 4.0f);
        dh::miniCheckbox("##noiseEnabled", s.noiseEnabled);
        ImGui::SetItemTooltip("Enable noise reduction");
        ImGui::BeginDisabled(!s.noiseEnabled);
        dh::sliderRow("Luminance", s.luminance, 0.0f, 100.0f, 50.0f, "%.0f");
        dh::sliderRow("Color", s.colour, 0.0f, 100.0f, 50.0f, "%.0f");
        dh::sliderRow("Detail", s.detail, 0.0f, 100.0f, 20.0f, "%.0f");
        dh::miniCheckbox("##auto", s.noiseAuto, "Auto noise level");
        {
            const Small small;
            const char* measured = "\xCF\x83 0.0123  \xC2\xB7  5 bands  \xC2\xB7  2.1 ms";
            dh::alignRight(ImGui::CalcTextSize(measured).x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(P().textTertiary, "%s", measured);
        }
        ImGui::EndDisabled();
    }

    // Sections whose GPU nodes do not exist yet: closed, and labelled as such in the header.
    for (const char* title : {"White Balance & Presence", "HSL / Color"}) {
        bool& open = std::strcmp(title, "HSL / Color") == 0 ? s.hslOpen : s.balanceOpen;
        float pillWidth = 0.0f;
        {
            const Small small;
            pillWidth = ImGui::CalcTextSize("Preview").x + ImGui::GetFontSize() * 0.9f;
        }
        dh::sectionHeader(title, open, pillWidth);
        dh::alignRight(pillWidth);
        dh::pill("Preview", P().warning);
        ImGui::SetItemTooltip("Interface preview: this adjustment does not change the image yet.");
    }
}

// -----------------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------------

void buildLayout(ImGuiID root, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, size);
    ImGuiID center = root;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 360.0f / size.x, nullptr, &center);
    const ImGuiID adjustments = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, nullptr, &right);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 300.0f / (size.x - 360.0f), nullptr, &center);
    const ImGuiID info = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.50f, nullptr, &left);
    const ImGuiID filmstrip = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 150.0f / size.y, nullptr, &center);
    ImGui::DockBuilderDockWindow("Collections", left);
    ImGui::DockBuilderDockWindow("Info", info);
    ImGui::DockBuilderDockWindow("Filmstrip", filmstrip);
    ImGui::DockBuilderDockWindow("Layers", right);
    ImGui::DockBuilderDockWindow("Adjustments", adjustments);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(root);
}

void drawFrame(State& s) {
    drawToolbar(s);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImGui::GetID("Workspace");
    if (ImGui::DockBuilderGetNode(dockspace) == nullptr) buildLayout(dockspace, viewport->WorkSize);
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##host", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                     ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);
    ImGui::DockSpace(dockspace);
    ImGui::End();

    ImGuiWindowClass bare;
    bare.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;

    if (ImGui::Begin("Collections")) drawCollections();
    ImGui::End();
    if (ImGui::Begin("Info")) drawInfo(s);
    ImGui::End();

    const dh::Palette* chrome = dh::gPalette;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, dh::kDark.canvas);  // the photo is always judged on the dark surround
    ImGui::SetNextWindowClass(&bare);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool viewportVisible = ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    dh::gPalette = &dh::kDark;
    if (viewportVisible) drawViewport(s);
    dh::gPalette = chrome;
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, P().canvas);
    ImGui::SetNextWindowClass(&bare);
    if (ImGui::Begin("Filmstrip", nullptr, ImGuiWindowFlags_NoScrollbar)) drawFilmstrip(s);
    ImGui::End();
    ImGui::PopStyleColor();

    if (ImGui::Begin("Layers")) drawLayers(s);
    ImGui::End();
    if (ImGui::Begin("Adjustments")) drawAdjustments(s);
    ImGui::End();

    dh::drawPopupShadows();
}

// -----------------------------------------------------------------------------
// Software rasteriser for ImDrawData (one sample per pixel, top-left fill rule)
// -----------------------------------------------------------------------------

struct Framebuffer {
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;  // RGBA8
};

void serviceTextures(ImDrawData* drawData) {
    if (!drawData->Textures) return;
    for (ImTextureData* texture : *drawData->Textures) {
        if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates) {
            texture->SetTexID(static_cast<ImTextureID>(1));
            texture->SetStatus(ImTextureStatus_OK);
        } else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0) {
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

void sample(const ImTextureData* texture, float u, float v, float out[4]) {
    if (!texture || !texture->Pixels) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    const float x = u * static_cast<float>(texture->Width) - 0.5f;
    const float y = v * static_cast<float>(texture->Height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            const int px = std::clamp(x0 + dx, 0, texture->Width - 1);
            const int py = std::clamp(y0 + dy, 0, texture->Height - 1);
            const float weight = (dx ? fx : 1.0f - fx) * (dy ? fy : 1.0f - fy);
            const unsigned char* texel = texture->Pixels + (static_cast<std::size_t>(py) * static_cast<std::size_t>(texture->Width) + static_cast<std::size_t>(px)) * static_cast<std::size_t>(texture->BytesPerPixel);
            if (texture->BytesPerPixel == 4) {
                for (int c = 0; c < 4; ++c) out[c] += weight * static_cast<float>(texel[c]) / 255.0f;
            } else {
                out[0] += weight;
                out[1] += weight;
                out[2] += weight;
                out[3] += weight * static_cast<float>(texel[0]) / 255.0f;
            }
        }
    }
}

void rasterize(const ImDrawData* drawData, Framebuffer& fb) {
    using i64 = long long;
    const ImVec2 offset = drawData->DisplayPos;
    const ImVec2 scale = drawData->FramebufferScale;
    for (const ImDrawList* list : drawData->CmdLists) {
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const int clipX0 = std::max(0, static_cast<int>(std::floor((cmd.ClipRect.x - offset.x) * scale.x)));
            const int clipY0 = std::max(0, static_cast<int>(std::floor((cmd.ClipRect.y - offset.y) * scale.y)));
            const int clipX1 = std::min(fb.width, static_cast<int>(std::ceil((cmd.ClipRect.z - offset.x) * scale.x)));
            const int clipY1 = std::min(fb.height, static_cast<int>(std::ceil((cmd.ClipRect.w - offset.y) * scale.y)));
            if (clipX0 >= clipX1 || clipY0 >= clipY1) continue;
            const ImTextureData* texture = cmd.TexRef._TexData;
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawVert* v[3];
                i64 x[3], y[3];
                for (unsigned int k = 0; k < 3; ++k) {
                    v[k] = &list->VtxBuffer[static_cast<int>(cmd.VtxOffset + list->IdxBuffer[static_cast<int>(cmd.IdxOffset + i + k)])];
                    x[k] = std::llround(static_cast<double>((v[k]->pos.x - offset.x) * scale.x) * 16.0);
                    y[k] = std::llround(static_cast<double>((v[k]->pos.y - offset.y) * scale.y) * 16.0);
                }
                i64 area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
                if (area == 0) continue;
                if (area < 0) {
                    std::swap(v[1], v[2]);
                    std::swap(x[1], x[2]);
                    std::swap(y[1], y[2]);
                    area = -area;
                }
                const int minX = std::max(clipX0, static_cast<int>(std::min({x[0], x[1], x[2]}) >> 4));
                const int maxX = std::min(clipX1 - 1, static_cast<int>((std::max({x[0], x[1], x[2]}) + 15) >> 4));
                const int minY = std::max(clipY0, static_cast<int>(std::min({y[0], y[1], y[2]}) >> 4));
                const int maxY = std::min(clipY1 - 1, static_cast<int>((std::max({y[0], y[1], y[2]}) + 15) >> 4));
                const auto bias = [&](int a, int b) {
                    const i64 ex = x[b] - x[a];
                    const i64 ey = y[b] - y[a];
                    return ((ey == 0 && ex < 0) || ey < 0) ? 0 : -1;
                };
                const i64 bias0 = bias(1, 2), bias1 = bias(2, 0), bias2 = bias(0, 1);
                const bool flatUv = v[0]->uv.x == v[1]->uv.x && v[0]->uv.x == v[2]->uv.x && v[0]->uv.y == v[1]->uv.y &&
                                    v[0]->uv.y == v[2]->uv.y;
                float flat[4] = {1, 1, 1, 1};
                if (flatUv) sample(texture, v[0]->uv.x, v[0]->uv.y, flat);
                float color[3][4];
                for (int k = 0; k < 3; ++k) {
                    for (int c = 0; c < 4; ++c) color[k][c] = static_cast<float>((v[k]->col >> (8 * c)) & 0xFF) / 255.0f;
                }
                for (int py = minY; py <= maxY; ++py) {
                    for (int px = minX; px <= maxX; ++px) {
                        const i64 sx = static_cast<i64>(px) * 16 + 8;
                        const i64 sy = static_cast<i64>(py) * 16 + 8;
                        const i64 w0 = (x[2] - x[1]) * (sy - y[1]) - (y[2] - y[1]) * (sx - x[1]);
                        const i64 w1 = (x[0] - x[2]) * (sy - y[2]) - (y[0] - y[2]) * (sx - x[2]);
                        const i64 w2 = (x[1] - x[0]) * (sy - y[0]) - (y[1] - y[0]) * (sx - x[0]);
                        if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) continue;
                        const float l0 = static_cast<float>(static_cast<double>(w0) / static_cast<double>(area));
                        const float l1 = static_cast<float>(static_cast<double>(w1) / static_cast<double>(area));
                        const float l2 = 1.0f - l0 - l1;
                        float texel[4] = {flat[0], flat[1], flat[2], flat[3]};
                        if (!flatUv) {
                            sample(texture, v[0]->uv.x * l0 + v[1]->uv.x * l1 + v[2]->uv.x * l2,
                                   v[0]->uv.y * l0 + v[1]->uv.y * l1 + v[2]->uv.y * l2, texel);
                        }
                        float src[4];
                        for (int c = 0; c < 4; ++c) src[c] = (color[0][c] * l0 + color[1][c] * l1 + color[2][c] * l2) * texel[c];
                        unsigned char* dst = &fb.pixels[(static_cast<std::size_t>(py) * static_cast<std::size_t>(fb.width) + static_cast<std::size_t>(px)) * 4];
                        for (int c = 0; c < 3; ++c) {
                            const float blended = src[c] * src[3] + static_cast<float>(dst[c]) / 255.0f * (1.0f - src[3]);
                            dst[c] = static_cast<unsigned char>(std::clamp(blended, 0.0f, 1.0f) * 255.0f + 0.5f);
                        }
                        dst[3] = 255;
                    }
                }
            }
        }
    }
}

void writePng(const char* path, const ImVec4& clear) {
    ImDrawData* drawData = ImGui::GetDrawData();
    Framebuffer fb;
    fb.width = static_cast<int>(kWidth) * kScale;
    fb.height = static_cast<int>(kHeight) * kScale;
    fb.pixels.resize(static_cast<std::size_t>(fb.width) * static_cast<std::size_t>(fb.height) * 4);
    for (std::size_t i = 0; i < fb.pixels.size(); i += 4) {
        fb.pixels[i] = static_cast<unsigned char>(clear.x * 255.0f);
        fb.pixels[i + 1] = static_cast<unsigned char>(clear.y * 255.0f);
        fb.pixels[i + 2] = static_cast<unsigned char>(clear.z * 255.0f);
        fb.pixels[i + 3] = 255;
    }
    rasterize(drawData, fb);
    stbi_write_png(path, fb.width, fb.height, 4, fb.pixels.data(), fb.width * 4);
    std::printf("wrote %s (%d x %d)\n", path, fb.width, fb.height);
}

}  // namespace

int main(int argc, char** argv) {
    const char* fontPath = argc > 1 ? argv[1] : "Roboto-Medium.ttf";
    const std::string outDir = argc > 2 ? argv[2] : ".";

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    ImGuiStyle& style = ImGui::GetStyle();
    const bool light = argc > 3 && std::strcmp(argv[3], "light") == 0;
    dh::applyStyle(style, light ? dh::kLight : dh::kDark);
    ImFontConfig config;
    config.OversampleH = 2;
    if (!io.Fonts->AddFontFromFileTTF(fontPath, dh::kFontBody, &config)) {
        std::fprintf(stderr, "cannot load %s\n", fontPath);
        return 1;
    }
    style.FontSizeBase = dh::kFontBody;

    State state;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };
    const auto frame = [&] {
        io.DisplaySize = ImVec2(kWidth, kHeight);
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(kScale), static_cast<float>(kScale));
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        drawFrame(state);
        ImGui::Render();
        serviceTextures(ImGui::GetDrawData());
    };
    const auto frames = [&](int count) { for (int i = 0; i < count; ++i) frame(); };
    const auto moveTo = [&](ImVec2 p) { io.AddMousePosEvent(p.x, p.y); frames(2); };
    const auto click = [&](ImVec2 p) {
        moveTo(p);
        io.AddMouseButtonEvent(0, true);
        frames(2);
        io.AddMouseButtonEvent(0, false);
        frames(2);
    };

    io.AddMousePosEvent(800.0f, 380.0f);
    frames(6);
    writePng((outDir + (light ? "/after-canvas-light.png" : "/after-canvas.png")).c_str(), style.Colors[ImGuiCol_WindowBg]);
    if (light) return 0;

    // --- Interaction checks ------------------------------------------------------
    const ImRect workspace = gRects["workspace"];
    click(ImVec2(workspace.Min.x + 20.0f, workspace.GetCenter().y));
    check(state.workspace == 0, "segmented control: clicking the first segment selects Catalog");
    click(ImVec2(workspace.Max.x - 20.0f, workspace.GetCenter().y));
    check(state.workspace == 2, "segmented control: clicking the last segment selects Split");
    click(ImVec2(workspace.GetCenter().x, workspace.GetCenter().y));
    check(state.workspace == 1, "segmented control: back to Canvas");

    const ImRect track = gRects["contrast.track"];
    moveTo(ImVec2(track.GetCenter().x, track.GetCenter().y));
    io.AddMouseButtonEvent(0, true);
    frames(2);
    io.AddMousePosEvent(track.Min.x + track.GetWidth() * 0.80f, track.GetCenter().y);
    frames(3);
    const float dragged = state.contrast;
    const int releasesBefore = state.contrastReleases;
    io.AddMouseButtonEvent(0, false);
    frames(3);
    std::printf("      contrast after drag to 80%% of the track: %+.1f\n", static_cast<double>(dragged));
    check(dragged > 50.0f && dragged < 70.0f, "slider row: dragging the track sets the value (expect about +60)");
    check(state.contrastReleases == releasesBefore + 1, "slider row: `released` fires once when the drag ends");

    const ImRect label = gRects["contrast.label"];
    moveTo(ImVec2(label.Min.x + 12.0f, label.GetCenter().y));
    for (int i = 0; i < 2; ++i) {
        io.AddMouseButtonEvent(0, true);
        frames(1);
        io.AddMouseButtonEvent(0, false);
        frames(1);
    }
    frames(2);
    check(state.contrast == 0.0f, "slider row: double-clicking the label resets to the default");

    const ImRect value = gRects["contrast.value"];
    click(ImVec2(value.GetCenter().x, value.GetCenter().y));
    frames(3);
    check(io.WantTextInput, "slider row: clicking the value opens a text field");
    for (const char* c = "37"; *c; ++c) io.AddInputCharacter(static_cast<unsigned int>(*c));
    frames(2);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frames(1);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frames(3);
    std::printf("      contrast after typing 37 + Return: %+.1f\n", static_cast<double>(state.contrast));
    check(state.contrast == 37.0f, "slider row: a typed value is applied");
    state.contrast = 0.0f;

    // --- Second image: a context menu, for the popup material ----------------------
    moveTo(ImVec2(700.0f, 300.0f));
    state.contextMenu = true;
    frames(1);
    state.contextMenu = false;
    io.AddMousePosEvent(gRects["thumb"].Max.x + 190.0f, gRects["thumb"].Min.y - 106.0f);
    frames(6);
    writePng((outDir + "/after-popup.png").c_str(), style.Colors[ImGuiCol_WindowBg]);

    std::printf("%s (%d failure%s)\n", failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
