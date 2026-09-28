# DarkHouse — third-party dependencies for the desktop front-end.
#
# Fetched at configure time and pinned to release tags:
#   GLFW       windowing, input and Vulkan surface creation
#   GLM        vector/matrix math (also bridged to ImVec2/ImVec4, see include/darkhouse_imconfig.hpp)
#   Dear ImGui docking branch, built here with the GLFW and Vulkan backends
#
# Offline or vendored builds can point FetchContent at a local checkout, e.g.
#   -DFETCHCONTENT_SOURCE_DIR_IMGUI=/src/imgui -DFETCHCONTENT_SOURCE_DIR_GLFW=/src/glfw
#   -DFETCHCONTENT_SOURCE_DIR_GLM=/src/glm
# or set FETCHCONTENT_FULLY_DISCONNECTED=ON once the sources are in the build tree.

include(FetchContent)

set(DARKHOUSE_GLFW_TAG  "3.5.1"             CACHE STRING "GLFW release tag")
set(DARKHOUSE_GLM_TAG   "1.0.3"             CACHE STRING "GLM release tag")
set(DARKHOUSE_IMGUI_TAG "v1.92.9b-docking"  CACHE STRING "Dear ImGui docking-branch release tag")
mark_as_advanced(DARKHOUSE_GLFW_TAG DARKHOUSE_GLM_TAG DARKHOUSE_IMGUI_TAG)

# Lets the option() calls in the dependencies honour the plain variables below.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        ${DARKHOUSE_GLFW_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   FALSE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        ${DARKHOUSE_GLM_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   FALSE)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${DARKHOUSE_IMGUI_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   FALSE)

# GLFW: library only. GLFW_BUILD_X11 / GLFW_BUILD_WAYLAND stay user-controllable.
set(GLFW_BUILD_DOCS     OFF)
set(GLFW_BUILD_TESTS    OFF)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_INSTALL        OFF)
# GLM: header-only, no tests or install rules.
set(GLM_BUILD_LIBRARY   OFF)
set(GLM_BUILD_TESTS     OFF)
set(GLM_BUILD_INSTALL   OFF)

# ImGui ships no CMakeLists.txt, so MakeAvailable only downloads it.
FetchContent_MakeAvailable(glfw glm imgui)

# Treat dependency headers as system headers so -Wall -Wextra -Wpedantic
# -Wshadow -Werror in DarkHouse sources never fire on third-party code.
# (Equivalent to the SYSTEM keyword of FetchContent_Declare, which needs CMake 3.25.)
function(darkhouse_mark_system_includes target)
    get_target_property(dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
        set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
    endif()
endfunction()
darkhouse_mark_system_includes(glfw)
darkhouse_mark_system_includes(glm-header-only)

# ------------------------------------------------------------------------------
# DarkHouse::imgui — core + docking, GLFW platform backend, Vulkan renderer backend
# ------------------------------------------------------------------------------
add_library(darkhouse_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
add_library(DarkHouse::imgui ALIAS darkhouse_imgui)

target_include_directories(darkhouse_imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")
# The user config must be seen by ImGui's own translation units too, or
# ImVec2/ImVec4 would have different layouts on either side of the library.
target_include_directories(darkhouse_imgui PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(darkhouse_imgui PUBLIC
    IMGUI_USER_CONFIG="darkhouse_imconfig.hpp"
    GLFW_INCLUDE_NONE)
target_link_libraries(darkhouse_imgui PUBLIC glfw glm::glm Vulkan::Vulkan)
set_target_properties(darkhouse_imgui PROPERTIES FOLDER "third_party")

# ------------------------------------------------------------------------------
# UI font: Roboto Medium (Apache License 2.0), shipped with Dear ImGui in
# misc/fonts, embedded as a byte array so the executable needs no font files.
# ------------------------------------------------------------------------------
set(DARKHOUSE_UI_FONT_FILE "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf")
set(DARKHOUSE_UI_FONT_HEADER "${CMAKE_BINARY_DIR}/generated/darkhouse_ui_font.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DARKHOUSE_UI_FONT_FILE}")
file(READ "${DARKHOUSE_UI_FONT_FILE}" darkhouse_font_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])"
       "\\1\n" darkhouse_font_hex "${darkhouse_font_hex}")  # 16 bytes per line
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," DARKHOUSE_UI_FONT_BYTES "${darkhouse_font_hex}")
file(CONFIGURE
    OUTPUT "${DARKHOUSE_UI_FONT_HEADER}"
    CONTENT [=[
// Generated by CMake from Dear ImGui's misc/fonts/Roboto-Medium.ttf
// (Roboto, Apache License 2.0). Do not edit.
#pragma once
namespace darkhouse::generated {
alignas(4) inline constexpr unsigned char kUiFontTtf[] = {
@DARKHOUSE_UI_FONT_BYTES@
};
}  // namespace darkhouse::generated
]=]
    @ONLY)
unset(darkhouse_font_hex)
