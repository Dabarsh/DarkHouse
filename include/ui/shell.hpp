// DarkHouse — application shell (the GuiLayer drawn by GuiEngine).
//
// Owns the main-viewport dockspace and the chrome around it. Panels dock into
// the dockspace; the shell decides what is visible for the current AppMode.
#pragma once

#include "gui_engine.hpp"

namespace darkhouse::ui {

class DarkHouseShell final : public GuiLayer {
public:
    void draw(DarkHouseApp& app, const FrameContext& frame, GuiEngine& gui) override;

private:
    void drawStatusBar(DarkHouseApp& app, const FrameContext& frame, GuiEngine& gui);
    void drawEngineStats(DarkHouseApp& app, const FrameContext& frame, GuiEngine& gui);

    bool showEngineStats_ = true;
};

}  // namespace darkhouse::ui
