// DarkHouse — canonical order of the develop stack.
//
// A develop stack is a linear chain of nodes. Adjustments only commute in
// theory, so nodes are kept in a fixed processing order, as a raw developer
// does it:
//
//   denoise          sensor noise, on scene-linear data straight from the source
//   white_balance    chromatic adaptation before any tone change
//   exposure         exposure, highlights / shadows, contrast
//   hsl              colour mixer
//   color_grading    presence and 3-way colour wheels
//   (masks, lens corrections and other nodes follow; unknown types keep
//    their relative position at the end)
//
// The UI adds a node the first time its section is edited, so an untouched
// photo runs only what it needs.
#pragma once

#include "asset_manager.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace darkhouse {

// Position of a node type in the canonical order (lower runs first).
[[nodiscard]] int developStage(std::string_view nodeType) noexcept;

// Index of the first node of `nodeType`, if any.
[[nodiscard]] std::optional<std::size_t> findDevelopNode(const std::vector<EditNodeRecord>& stack,
                                                         std::string_view nodeType) noexcept;

// `stack` with `record` inserted after every node of an earlier or equal
// stage and before the first of a later one. Indices are renumbered.
[[nodiscard]] std::vector<EditNodeRecord> withDevelopNode(std::vector<EditNodeRecord> stack, EditNodeRecord record);

}  // namespace darkhouse
