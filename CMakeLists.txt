cmake_minimum_required(VERSION 3.22)

project(DarkHouse
    VERSION 0.1.0
    DESCRIPTION "Unified non-destructive RAW development, asset management, compositing and vector design"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(DARKHOUSE_ONNXRUNTIME "AUTO" CACHE STRING "ONNX Runtime AI segmentation backend: AUTO, ON or OFF")
set_property(CACHE DARKHOUSE_ONNXRUNTIME PROPERTY STRINGS AUTO ON OFF)
option(DARKHOUSE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DARKHOUSE_GUI "Build the Dear ImGui desktop front-end (GLFW window + Vulkan swapchain)" ON)
# Whether Vulkan validation layers are on when neither --validation nor
# --no-validation is passed. AUTO enables them for Debug and RelWithDebInfo
# builds; it is resolved per configuration, so multi-config generators
# (Visual Studio, Xcode, Ninja Multi-Config) get the right default for each.
set(DARKHOUSE_VULKAN_VALIDATION "AUTO" CACHE STRING "Default Vulkan validation layers: AUTO, ON or OFF")
set_property(CACHE DARKHOUSE_VULKAN_VALIDATION PROPERTY STRINGS AUTO ON OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
find_package(Vulkan REQUIRED)   # headers >= 1.3 are enforced in render_pipeline.hpp
find_package(SQLite3 REQUIRED)
# CMake 4.x renamed the imported target; 3.22 only provides the old name.
if(TARGET SQLite3::SQLite3)
    set(DARKHOUSE_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(DARKHOUSE_SQLITE_TARGET SQLite::SQLite3)
endif()
find_package(Threads REQUIRED)

# stb_image for photo decoding (engine), then GLFW, GLM and Dear ImGui
# (docking) for the desktop front-end.
include(DarkHouseCoreDependencies)
if(DARKHOUSE_GUI)
    include(DarkHouseDependencies)
endif()

# ------------------------------------------------------------------------------
# GLSL -> SPIR-V (build/shaders/*.spv; ignored by git)
# ------------------------------------------------------------------------------
set(DARKHOUSE_SHADER_SOURCES
    shaders/exposure.comp
    shaders/display_srgb.comp
    shaders/white_balance.comp
    shaders/hsl_adjust.comp
    shaders/color_grading.comp
    shaders/mask_generate.comp
    shaders/local_adjust.comp
    shaders/denoise_down0.comp
    shaders/denoise_down.comp
    shaders/denoise_sigma.comp
    shaders/denoise_up.comp
    shaders/denoise_final.comp)
# Headers pulled in with #include; every shader is rebuilt when one changes.
set(DARKHOUSE_SHADER_INCLUDES
    shaders/color_common.glsl
    shaders/mask_common.glsl
    shaders/tone_common.glsl
    shaders/denoise_common.glsl
    shaders/denoise_config.h)
list(TRANSFORM DARKHOUSE_SHADER_INCLUDES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/" OUTPUT_VARIABLE DARKHOUSE_SHADER_INCLUDE_PATHS)
set(DARKHOUSE_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")

find_program(DARKHOUSE_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
find_program(DARKHOUSE_GLSLANG_VALIDATOR NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
if(NOT DARKHOUSE_GLSLC AND NOT DARKHOUSE_GLSLANG_VALIDATOR)
    message(FATAL_ERROR
        "No GLSL compiler found. Install the Vulkan SDK, or `brew install shaderc` / "
        "`apt install glslc` (or glslang-tools).")
endif()

set(DARKHOUSE_SPIRV_OUTPUTS "")
foreach(shader_source IN LISTS DARKHOUSE_SHADER_SOURCES)
    get_filename_component(shader_name "${shader_source}" NAME_WE)
    set(shader_input "${CMAKE_CURRENT_SOURCE_DIR}/${shader_source}")
    set(shader_output "${DARKHOUSE_SHADER_OUTPUT_DIR}/${shader_name}.spv")
    if(DARKHOUSE_GLSLC)
        set(DARKHOUSE_GLSL_COMPILER "${DARKHOUSE_GLSLC}")
        set(shader_command "${DARKHOUSE_GLSLC}" --target-env=vulkan1.3 -O
            "-I${CMAKE_CURRENT_SOURCE_DIR}/shaders" -o "${shader_output}" "${shader_input}")
    else()
        set(DARKHOUSE_GLSL_COMPILER "${DARKHOUSE_GLSLANG_VALIDATOR}")
        set(shader_command "${DARKHOUSE_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3
            "-I${CMAKE_CURRENT_SOURCE_DIR}/shaders" -o "${shader_output}" "${shader_input}")
    endif()
    add_custom_command(
        OUTPUT "${shader_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DARKHOUSE_SHADER_OUTPUT_DIR}"
        COMMAND ${shader_command}
        DEPENDS "${shader_input}" ${DARKHOUSE_SHADER_INCLUDE_PATHS}
        COMMENT "Compiling ${shader_source} -> SPIR-V"
        VERBATIM)
    list(APPEND DARKHOUSE_SPIRV_OUTPUTS "${shader_output}")
endforeach()
add_custom_target(darkhouse_shaders ALL DEPENDS ${DARKHOUSE_SPIRV_OUTPUTS}
    SOURCES ${DARKHOUSE_SHADER_SOURCES} ${DARKHOUSE_SHADER_INCLUDES})

# ------------------------------------------------------------------------------
# Embed schema/catalog.sql so the binary always carries its own schema
# ------------------------------------------------------------------------------
set(DARKHOUSE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/schema/catalog.sql")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/schema/catalog.sql" DARKHOUSE_CATALOG_SQL)
file(CONFIGURE
    OUTPUT "${DARKHOUSE_GENERATED_DIR}/catalog_schema.hpp"
    CONTENT [=[
// Generated by CMake from schema/catalog.sql. Do not edit.
#pragma once
namespace darkhouse::generated {
inline constexpr const char kCatalogSchemaSql[] = R"darkhouse_sql(@DARKHOUSE_CATALOG_SQL@)darkhouse_sql";
}  // namespace darkhouse::generated
]=]
    @ONLY)

# ------------------------------------------------------------------------------
# Targets
#   darkhouse_core  engine: catalog, Vulkan context, swapchain, render pipeline,
#                   layers, AI, application controller and the library view
#                   model (no UI toolkit)
#   darkhouse_gui   desktop front-end: GLFW window, ImGui engine, shell, panels
#   DarkHouse       the executable: command line and front-end selection
# Tests link the libraries instead of recompiling sources.
# ------------------------------------------------------------------------------

# Strict warnings and platform flags for every first-party target.
function(darkhouse_apply_build_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        if(DARKHOUSE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        # Bounds-checked standard containers in Debug builds (ABI-neutral).
        target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
        if(DARKHOUSE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Where the Khronos validation layer manifest lives. When the loader cannot
# find the layer at runtime (SDK not installed system-wide), the context adds
# this directory through VK_ADD_LAYER_PATH, unless the environment already
# steers layer discovery.
find_path(DARKHOUSE_VK_LAYER_MANIFEST_DIR
    NAMES VkLayer_khronos_validation.json
    HINTS
        "$ENV{VULKAN_SDK}/share/vulkan/explicit_layer.d"
        "$ENV{VULKAN_SDK}/etc/vulkan/explicit_layer.d"
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
    PATHS
        /usr/share/vulkan/explicit_layer.d
        /usr/local/share/vulkan/explicit_layer.d
        /opt/homebrew/share/vulkan/explicit_layer.d
    NO_DEFAULT_PATH)
mark_as_advanced(DARKHOUSE_VK_LAYER_MANIFEST_DIR)

# --- darkhouse_core -----------------------------------------------------------
add_library(darkhouse_core STATIC
    src/app_controller.cpp
    src/asset_manager.cpp
    src/import_scan.cpp
    src/app_paths.cpp
    src/vulkan_context.cpp
    src/swapchain.cpp
    src/render_pipeline.cpp
    src/layer_stack.cpp
    src/ai_segmentation.cpp
    src/denoise.cpp
    src/denoise_node.cpp
    src/color_adjust.cpp
    src/color_nodes.cpp
    src/develop_stack.cpp
    src/mask_engine.cpp
    src/local_adjust_node.cpp
    src/image_decoder.cpp
    src/ui/library_model.cpp
    include/app_controller.hpp
    include/asset_manager.hpp
    include/import_scan.hpp
    include/app_paths.hpp
    include/vulkan_context.hpp
    include/vulkan_utils.hpp
    include/swapchain.hpp
    include/render_pipeline.hpp
    include/layer_stack.hpp
    include/ai_segmentation.hpp
    include/denoise.hpp
    include/denoise_node.hpp
    include/color_adjust.hpp
    include/color_nodes.hpp
    include/develop_stack.hpp
    include/mask_engine.hpp
    include/local_adjust_node.hpp
    include/image_decoder.hpp
    src/tiff_reader.hpp
    src/parallel.hpp
    shaders/denoise_config.h
    include/ui/library_model.hpp
    schema/catalog.sql)
add_dependencies(darkhouse_core darkhouse_shaders)
target_include_directories(darkhouse_core
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"   # denoise_config.h, shared with GLSL
    PRIVATE "${DARKHOUSE_GENERATED_DIR}")
target_link_libraries(darkhouse_core
    PUBLIC  Vulkan::Vulkan Threads::Threads
    PRIVATE ${DARKHOUSE_SQLITE_TARGET} darkhouse_stb)
target_compile_definitions(darkhouse_core PUBLIC DARKHOUSE_VERSION="${PROJECT_VERSION}")
if(DARKHOUSE_VK_LAYER_MANIFEST_DIR)
    target_compile_definitions(darkhouse_core PRIVATE DARKHOUSE_VK_LAYER_DIR="${DARKHOUSE_VK_LAYER_MANIFEST_DIR}")
endif()
darkhouse_apply_build_flags(darkhouse_core)

# --- darkhouse_gui ------------------------------------------------------------
if(DARKHOUSE_GUI)
    add_library(darkhouse_gui STATIC
        src/platform_window.cpp
        src/gui_engine.cpp
        src/ui/shell.cpp
        src/ui/panel.cpp
        src/ui/workspace_layout.cpp
        src/ui/theme.cpp
        src/ui/widgets.cpp
        src/ui/develop_sections.cpp
        src/ui/masking.cpp
        src/ui/panels/left_dock.cpp
        src/ui/panels/center_dock.cpp
        src/ui/panels/right_dock.cpp
        src/ui/panels/masking_panel.cpp
        include/platform_window.hpp
        include/gui_engine.hpp
        include/ui/shell.hpp
        include/ui/panel.hpp
        include/ui/workspace_layout.hpp
        include/ui/theme.hpp
        include/ui/widgets.hpp
        include/ui/develop_sections.hpp
        include/ui/masking.hpp
        include/ui/panels.hpp)
    target_link_libraries(darkhouse_gui PUBLIC darkhouse_core DarkHouse::imgui glfw glm::glm)
    target_include_directories(darkhouse_gui PRIVATE "${CMAKE_BINARY_DIR}/generated")  # darkhouse_ui_font.hpp
    target_compile_definitions(darkhouse_gui PUBLIC DARKHOUSE_WITH_GUI=1)
    darkhouse_apply_build_flags(darkhouse_gui)
endif()

# --- DarkHouse executable -----------------------------------------------------
add_executable(DarkHouse src/main.cpp)
target_link_libraries(DarkHouse PRIVATE darkhouse_core $<$<BOOL:${DARKHOUSE_GUI}>:darkhouse_gui>)

# Vulkan validation default (see DARKHOUSE_VULKAN_VALIDATION above).
if(DARKHOUSE_VULKAN_VALIDATION STREQUAL "AUTO")
    set(DARKHOUSE_VALIDATION_DEFAULT "$<IF:$<CONFIG:Debug,RelWithDebInfo>,1,0>")
elseif(DARKHOUSE_VULKAN_VALIDATION)
    set(DARKHOUSE_VALIDATION_DEFAULT 1)
else()
    set(DARKHOUSE_VALIDATION_DEFAULT 0)
endif()
target_compile_definitions(DarkHouse PRIVATE
    DARKHOUSE_SHADER_DIR="${DARKHOUSE_SHADER_OUTPUT_DIR}"
    DARKHOUSE_VALIDATION_DEFAULT=${DARKHOUSE_VALIDATION_DEFAULT})
set_target_properties(DarkHouse PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
darkhouse_apply_build_flags(DarkHouse)

# ------------------------------------------------------------------------------
# Optional: ONNX Runtime (AI subject / sky segmentation)
# ------------------------------------------------------------------------------
set(DARKHOUSE_HAVE_ONNXRUNTIME OFF)
if(NOT DARKHOUSE_ONNXRUNTIME STREQUAL "OFF")
    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
        PATH_SUFFIXES include include/onnxruntime include/onnxruntime/core/session onnxruntime)
    find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
        PATH_SUFFIXES lib lib64)
    if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
        set(DARKHOUSE_HAVE_ONNXRUNTIME ON)
        target_include_directories(darkhouse_core SYSTEM PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
        target_link_libraries(darkhouse_core PRIVATE "${ONNXRUNTIME_LIBRARY}")
        target_compile_definitions(darkhouse_core PRIVATE DARKHOUSE_WITH_ONNXRUNTIME=1)
    elseif(DARKHOUSE_ONNXRUNTIME STREQUAL "ON")
        message(FATAL_ERROR "DARKHOUSE_ONNXRUNTIME=ON but ONNX Runtime was not found; set ONNXRUNTIME_ROOT.")
    endif()
endif()

message(STATUS "DarkHouse ${PROJECT_VERSION}")
message(STATUS "  Vulkan:        ${Vulkan_LIBRARY}")
message(STATUS "  SQLite:        ${SQLite3_VERSION}")
message(STATUS "  GLSL compiler: ${DARKHOUSE_GLSL_COMPILER}")
message(STATUS "  ONNX Runtime:  ${DARKHOUSE_HAVE_ONNXRUNTIME}")
if(DARKHOUSE_GUI)
    message(STATUS "  GUI:           ON (GLFW ${DARKHOUSE_GLFW_TAG}, Dear ImGui ${DARKHOUSE_IMGUI_TAG}, GLM ${DARKHOUSE_GLM_TAG})")
else()
    message(STATUS "  GUI:           OFF (headless/CLI only)")
endif()
message(STATUS "  Validation:    ${DARKHOUSE_VULKAN_VALIDATION} by default; layer manifest: ${DARKHOUSE_VK_LAYER_MANIFEST_DIR}")

# ------------------------------------------------------------------------------
# Tests (CTest). GUI tests need a display; they exit 77 (skipped) without one.
# Headless CI can run them under Xvfb with a software Vulkan driver (lavapipe).
# ------------------------------------------------------------------------------
option(DARKHOUSE_BUILD_TESTS "Build the CTest test programs" ON)
if(DARKHOUSE_BUILD_TESTS)
    enable_testing()

    add_executable(darkhouse_library_model_test tests/library_model_test.cpp)
    target_link_libraries(darkhouse_library_model_test PRIVATE darkhouse_core)
    darkhouse_apply_build_flags(darkhouse_library_model_test)
    add_test(NAME library_model COMMAND darkhouse_library_model_test "${CMAKE_CURRENT_BINARY_DIR}/test_work")
    set_tests_properties(library_model PROPERTIES LABELS "catalog" TIMEOUT 60)

    add_executable(darkhouse_layer_stack_test tests/layer_stack_test.cpp)
    target_link_libraries(darkhouse_layer_stack_test PRIVATE darkhouse_core)
    target_include_directories(darkhouse_layer_stack_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")  # parallel.hpp
    darkhouse_apply_build_flags(darkhouse_layer_stack_test)
    add_test(NAME layer_stack COMMAND darkhouse_layer_stack_test)
    set_tests_properties(layer_stack PROPERTIES LABELS "layers" TIMEOUT 120)

    add_executable(darkhouse_color_adjust_test tests/color_adjust_test.cpp)
    target_link_libraries(darkhouse_color_adjust_test PRIVATE darkhouse_core)
    darkhouse_apply_build_flags(darkhouse_color_adjust_test)
    add_test(NAME color_adjust COMMAND darkhouse_color_adjust_test)
    set_tests_properties(color_adjust PROPERTIES LABELS "color" TIMEOUT 60)

    add_executable(darkhouse_color_gpu_test tests/color_gpu_test.cpp)
    target_link_libraries(darkhouse_color_gpu_test PRIVATE darkhouse_core)
    target_compile_definitions(darkhouse_color_gpu_test PRIVATE DARKHOUSE_SHADER_DIR="${DARKHOUSE_SHADER_OUTPUT_DIR}")
    darkhouse_apply_build_flags(darkhouse_color_gpu_test)
    add_test(NAME color_gpu COMMAND darkhouse_color_gpu_test)
    set_tests_properties(color_gpu PROPERTIES
        SKIP_RETURN_CODE 77 LABELS "color;vulkan" TIMEOUT 120
        ENVIRONMENT "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true")

    add_executable(darkhouse_mask_engine_test tests/mask_engine_test.cpp)
    target_link_libraries(darkhouse_mask_engine_test PRIVATE darkhouse_core)
    darkhouse_apply_build_flags(darkhouse_mask_engine_test)
    add_test(NAME mask_engine COMMAND darkhouse_mask_engine_test)
    set_tests_properties(mask_engine PROPERTIES LABELS "masking" TIMEOUT 60)

    add_executable(darkhouse_mask_gpu_test tests/mask_gpu_test.cpp)
    target_link_libraries(darkhouse_mask_gpu_test PRIVATE darkhouse_core)
    target_compile_definitions(darkhouse_mask_gpu_test PRIVATE DARKHOUSE_SHADER_DIR="${DARKHOUSE_SHADER_OUTPUT_DIR}")
    darkhouse_apply_build_flags(darkhouse_mask_gpu_test)
    add_test(NAME mask_gpu COMMAND darkhouse_mask_gpu_test)
    set_tests_properties(mask_gpu PROPERTIES
        SKIP_RETURN_CODE 77 LABELS "masking;vulkan" TIMEOUT 120
        ENVIRONMENT "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true")

    add_executable(darkhouse_denoise_test tests/denoise_test.cpp)
    target_link_libraries(darkhouse_denoise_test PRIVATE darkhouse_core)
    darkhouse_apply_build_flags(darkhouse_denoise_test)
    add_test(NAME denoise_reference COMMAND darkhouse_denoise_test)
    set_tests_properties(denoise_reference PROPERTIES LABELS "denoise" TIMEOUT 120)

    add_executable(darkhouse_image_decoder_test tests/image_decoder_test.cpp)
    target_link_libraries(darkhouse_image_decoder_test PRIVATE darkhouse_core darkhouse_stb)
    darkhouse_apply_build_flags(darkhouse_image_decoder_test)
    add_test(NAME image_decoder COMMAND darkhouse_image_decoder_test "${CMAKE_CURRENT_BINARY_DIR}/test_work")
    set_tests_properties(image_decoder PROPERTIES LABELS "preview" TIMEOUT 60)

    add_executable(darkhouse_photo_preview_test tests/photo_preview_test.cpp)
    target_link_libraries(darkhouse_photo_preview_test PRIVATE darkhouse_core darkhouse_stb)
    target_compile_definitions(darkhouse_photo_preview_test PRIVATE DARKHOUSE_SHADER_DIR="${DARKHOUSE_SHADER_OUTPUT_DIR}")
    darkhouse_apply_build_flags(darkhouse_photo_preview_test)
    add_test(NAME photo_preview COMMAND darkhouse_photo_preview_test "${CMAKE_CURRENT_BINARY_DIR}/test_work")
    set_tests_properties(photo_preview PROPERTIES
        SKIP_RETURN_CODE 77 LABELS "preview;vulkan" TIMEOUT 300
        ENVIRONMENT "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true")

    # Benchmark (not a test); the CTest entry is a tiny smoke run.
    add_executable(darkhouse_denoise_bench bench/denoise_bench.cpp)
    target_link_libraries(darkhouse_denoise_bench PRIVATE darkhouse_core)
    target_include_directories(darkhouse_denoise_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")  # test images
    target_compile_definitions(darkhouse_denoise_bench PRIVATE DARKHOUSE_SHADER_DIR="${DARKHOUSE_SHADER_OUTPUT_DIR}")
    darkhouse_apply_build_flags(darkhouse_denoise_bench)
    add_test(NAME denoise_bench_smoke COMMAND darkhouse_denoise_bench --size 320x240 --iterations 2 --cpu)
    set_tests_properties(denoise_bench_smoke PROPERTIES SKIP_RETURN_CODE 77 LABELS "denoise;vulkan;bench" TIMEOUT 120)

    add_executable(darkhouse_denoise_gpu_test tests/denoise_gpu_test.cpp)
    target_link_libraries(darkhouse_denoise_gpu_test PRIVATE darkhouse_core)
    target_compile_definitions(darkhouse_denoise_gpu_test PRIVATE DARKHOUSE_SHADER_DIR="${DARKHOUSE_SHADER_OUTPUT_DIR}")
    darkhouse_apply_build_flags(darkhouse_denoise_gpu_test)
    add_test(NAME denoise_gpu COMMAND darkhouse_denoise_gpu_test)
    set_tests_properties(denoise_gpu PROPERTIES
        SKIP_RETURN_CODE 77 LABELS "denoise;vulkan" TIMEOUT 300
        ENVIRONMENT "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true")

    if(DARKHOUSE_GUI)
        add_executable(darkhouse_present_smoke tests/present_smoke.cpp)
        target_link_libraries(darkhouse_present_smoke PRIVATE darkhouse_gui)
        darkhouse_apply_build_flags(darkhouse_present_smoke)
        add_test(NAME present_smoke COMMAND darkhouse_present_smoke --frames 120 --resize)
        # Synchronization validation on top of core validation: it catches
        # hazards (e.g. layout transitions vs. present) that core checks miss.
        set_tests_properties(present_smoke PROPERTIES
            SKIP_RETURN_CODE 77 LABELS "gui;vulkan" TIMEOUT 120
            ENVIRONMENT "VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true")
    endif()
endif()
