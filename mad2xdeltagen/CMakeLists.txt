cmake_minimum_required(VERSION 3.16)
project(mad2xdeltagen LANGUAGES C CXX)

# Native Linux CLI: original-section-bytes + modified-section-bytes ->
# one *.mad2xdelta patch file (see src/main.cpp for the full picture, and
# ../mad2xdeltamod for the in-game DLL that applies these). Deliberately
# its own standalone CMake project, NOT added via the root CMakeLists.txt's
# add_subdirectory (that project's toolchain file cross-compiles everything
# for 32-bit Windows; this tool only ever runs on the host) -- same
# separation as mad2music/CMakeLists.txt.
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XDELTA3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/jak-project/third-party/xdelta3/xdelta3")

add_executable(mad2xdeltagen
    src/main.cpp
    "${XDELTA3_DIR}/xdelta3.c"
)

target_include_directories(mad2xdeltagen PRIVATE "${XDELTA3_DIR}")

# See ../mad2xdeltamod/CMakeLists.txt for what each of these controls --
# kept identical between the two so a patch this tool writes always
# matches the VCDIFF dialect mad2xdeltamod's decoder expects.
target_compile_definitions(mad2xdeltagen PRIVATE
    XD3_USE_LARGEFILE64=0
    XD3_USE_LARGESIZET=0
    SIZEOF_UNSIGNED_INT=4
    SIZEOF_SIZE_T=8
    XD3_ENCODER=1
    XD3_DEBUG=0
)

set_target_properties(mad2xdeltagen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
)
