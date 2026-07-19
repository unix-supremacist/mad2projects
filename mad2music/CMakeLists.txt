cmake_minimum_required(VERSION 3.16)
project(mad2music LANGUAGES CXX)

# Background-music + podcast playback engine. Built two ways, both
# justfile-orchestrated, NOT part of the root project's MinGW i686
# cross-build (see the repo root CMakeLists.txt, which never
# add_subdirectory(mad2music)s this):
#   - Native Linux, via `just build-music-linux` -- the default/primary
#     target, forked/supervised by mad2relauncher for the whole Wine play
#     session (see src/main.cpp's file header).
#   - Best-effort native Windows, via `just build-music-windows`
#     (i686-w64-mingw32 cross-compile, mirroring build-sm64-windows's
#     precedent) -- for native-Windows Mad2 installs, where there's no
#     mad2relauncher to fork/supervise this process at all. mad2podcastmod
#     starts it itself in that case (see mad2podcastmod/src/podcastmod.cpp's
#     native-Windows branch). Uses the same SDL3-devel-*-mingw release
#     package (extern/.sdl3-mingw, fetched by that recipe) as build-sm64-
#     windows's extern/.sdl2-mingw for SDL2.
#
# On both platforms the actual audio decode is via an exec'd `ffmpeg`
# subprocess (see src/audio_engine.cpp) -- ffmpeg(.exe) must be on PATH at
# runtime either way; this project doesn't vendor or link against it.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# When cross-compiling for Windows, there's no host pkg-config entry for a
# mingw target, so `just build-music-windows` points PKG_CONFIG_LIBDIR/
# PKG_CONFIG_PATH at the fetched SDL3-devel-mingw release's own
# i686-w64-mingw32/lib/pkgconfig/sdl3.pc before invoking cmake -- its prefix
# is self-relative (${pcfiledir}/../..), so this works with no further
# sysroot wiring; pkg_check_modules below needs no changes for that case.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)

find_package(Threads REQUIRED)

add_executable(mad2music
    src/main.cpp
    src/audio_engine.cpp
    src/music_pool.cpp
    src/config.cpp
    src/log.cpp
)

target_include_directories(mad2music PRIVATE ${SDL3_INCLUDE_DIRS})
target_link_directories(mad2music PRIVATE ${SDL3_LIBRARY_DIRS})
target_link_libraries(mad2music PRIVATE ${SDL3_LIBRARIES} Threads::Threads)
target_compile_options(mad2music PRIVATE -Wall -Wextra ${SDL3_CFLAGS_OTHER})

if(WIN32)
    # platform_compat.h's Windows branch needs Winsock2 (ws2_32) -- not
    # pulled in by SDL3's own pkg-config Libs (that .pc's Libs.private list
    # covers SDL3's own Windows dependencies, not ours).
    target_link_libraries(mad2music PRIVATE ws2_32)
    # No console window -- this runs as a silent background process the
    # whole session (started by mad2podcastmod on native Windows, or
    # mad2relauncher under Wine); all its output already goes to
    # mad2music.log, not stdout.
    target_link_options(mad2music PRIVATE -mwindows)
endif()

set_target_properties(mad2music PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
