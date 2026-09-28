#include "develop_stack.hpp"

#include <array>
#include <utility>

namespace darkhouse {

int developStage(std::string_view nodeType) noexcept {
    constexpr std::array<std::string_view, 7> kOrder{"denoise",      "white_balance", "exposure",    "tone_curve",
                                                     "hsl",          "color_grading", "local_adjust"};
    for (std::size_t i = 0; i < kOrder.size(); ++i) {
        if (kOrder[i] == nodeType) return static_cast<int>(i);
    }
    return static_cast<int>(kOrder.size());  // everything else: after the global adjustments
}

std::optional<std::size_t> findDevelopNode(const std::vector<EditNodeRecord>& stack, std::string_view nodeType) noexcept {
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (stack[i].nodeType == nodeType) return i;
    }
    return std::nullopt;
}

std::vector<EditNodeRecord> withDevelopNode(std::vector<EditNodeRecord> stack, EditNodeRecord record) {
    const int stage = developStage(record.nodeType);
    std::size_t at = stack.size();
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (developStage(stack[i].nodeType) > stage) {
            at = i;
            break;
        }
    }
    stack.insert(stack.begin() + static_cast<std::ptrdiff_t>(at), std::move(record));
    for (std::size_t i = 0; i < stack.size(); ++i) stack[i].nodeIndex = static_cast<std::int32_t>(i);
    return stack;
}

}  // namespace darkhouse
