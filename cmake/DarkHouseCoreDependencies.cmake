# DarkHouse — third-party dependencies of the engine (built in every configuration).
#
#   stb_image / stb_image_write (public domain / MIT): photo decoding for the
#   live preview (JPEG, PNG 8/16-bit, BMP, TGA, GIF, PSD, HDR), and image
#   writing in tests. stb has no release tags, so it is pinned by commit.
#
# Offline builds: -DFETCHCONTENT_SOURCE_DIR_STB=/path/to/stb

include(FetchContent)

set(DARKHOUSE_STB_COMMIT "2c980bb59875b0d32144a71867fbdebb2f77cd20" CACHE STRING "nothings/stb commit")
mark_as_advanced(DARKHOUSE_STB_COMMIT)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        ${DARKHOUSE_STB_COMMIT}
    GIT_PROGRESS   FALSE)
FetchContent_MakeAvailable(stb)  # header-only: nothing to add_subdirectory

# The implementations live in one translation unit compiled without the
# project's strict warnings; everything else only sees the headers, as
# system headers.
add_library(darkhouse_stb STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/stb_impl.c")
target_include_directories(darkhouse_stb SYSTEM PUBLIC "${stb_SOURCE_DIR}")
set_target_properties(darkhouse_stb PROPERTIES FOLDER "third_party")
