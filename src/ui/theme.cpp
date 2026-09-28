#include "ui/theme.hpp"

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

}  // namespace darkhouse::ui::theme
