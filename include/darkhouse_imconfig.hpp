// DarkHouse — Dear ImGui user configuration (IMGUI_USER_CONFIG).
//
// Compiled into ImGui itself and into every DarkHouse translation unit that
// includes imgui.h, so both sides agree on the layout of ImVec2 / ImVec4.
#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

// Old names are compiled out, so new code cannot come to depend on them.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS

// Arithmetic operators on ImVec2 / ImVec4 (a + b, a * 2.0f, ...).
#define IMGUI_DEFINE_MATH_OPERATORS

// Implicit conversions between ImGui vectors and GLM, so viewport and colour
// math can be written with GLM and handed straight to ImGui calls.
#define IM_VEC2_CLASS_EXTRA                                            \
    constexpr ImVec2(const glm::vec2& v) : x(v.x), y(v.y) {}           \
    operator glm::vec2() const { return glm::vec2(x, y); }

#define IM_VEC4_CLASS_EXTRA                                            \
    constexpr ImVec4(const glm::vec4& v) : x(v.x), y(v.y), z(v.z), w(v.w) {} \
    operator glm::vec4() const { return glm::vec4(x, y, z, w); }
