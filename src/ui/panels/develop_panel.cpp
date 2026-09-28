// Develop panels: the parametric develop sections of the open photo.

#include "ui/panels.hpp"

namespace darkhouse::ui {

// -----------------------------------------------------------------------------
// Adjustments
// -----------------------------------------------------------------------------

void AdjustmentsPanel::draw(PanelContext& ctx) {
    const AssetRecord* asset = ctx.library.findAsset(ctx.app.activeAssetId());
    ImGui::TextDisabled("Develop  |  %s", asset ? asset->fileName.c_str() : "document (no photo open)");
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Basic", ImGuiTreeNodeFlags_DefaultOpen)) basic_.draw(ctx);
    if (ImGui::CollapsingHeader("Color Mixer (HSL)", ImGuiTreeNodeFlags_DefaultOpen)) mixer_.draw(ctx);
    if (ImGui::CollapsingHeader("Color Grading", ImGuiTreeNodeFlags_DefaultOpen)) grading_.draw(ctx);
    if (ImGui::CollapsingHeader("Detail", ImGuiTreeNodeFlags_DefaultOpen)) detail_.draw(ctx);

    ImGui::Spacing();
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("Every control runs live on the GPU develop graph and is saved with the photo when released.");
    ImGui::PopTextWrapPos();
}

}  // namespace darkhouse::ui
