# add_mad2_mod(<target> SOURCES <src1> [<src2> ...] [OUTPUT_NAME <name>]
#               [LIBS <lib1> ...] [INCLUDE_DIRS <dir1> ...] [KILL_AT]
#               [RUNTIME_SUBDIR <subdir>] [EXTRA_LINK_OPTIONS <opt1> ...])
#
# Captures the add_library/set_target_properties/target_compile_options/
# target_link_options boilerplate shared by every one of this repo's
# ~21+ MinGW-cross-compiled mod DLL targets (see improvement-plan.md item
# 5 -- previously ~260-280 LOC of near-identical CMakeLists.txt content).
#
# <target> is the CMake target name (what add_deploy_target's DEPENDS/
# $<TARGET_FILE:...> in the root CMakeLists.txt reference). Unless
# OUTPUT_NAME overrides it, <target> is also the deployed DLL's base
# filename (e.g. target "mad2xinput" -> mad2/mods/mad2xinput.dll).
# OUTPUT_NAME exists for the mods whose deployed name is load-order-
# engineered rather than matching their target name:
#   - mad2graphicseffectmod -> zz_mad2graphicseffectmod (sorts LAST --
#     see mad2graphicseffectmod/CMakeLists.txt for why)
#   - mad2hookutil -> aa_mad2hookutil (sorts FIRST -- see
#     mad2hookutil/include/mad2hookutil_api.h for why)
#   - mad2modloader -> version (the version.dll proxy trick -- see
#     modloader/CMakeLists.txt)
#
# KILL_AT adds -Wl,--kill-at, needed by every mod that exports a shared API
# other mods resolve by name via GetProcAddress (mad2xinput, mad2config,
# mad2effects, mad2levelredirectmod, mad2hookutil, mad2textrenderer) --
# without it the export table would hold "SomeFunc@8" instead of
# "SomeFunc", and every GetProcAddress(h, "SomeFunc") lookup would fail.
#
# RUNTIME_SUBDIR defaults to "mods" (i.e. dist/mods/<name>.dll, where every
# ordinary mod deploys); the modloader itself passes RUNTIME_SUBDIR "" for
# dist/version.dll (deployed to the game dir root, not mods/).
#
# EXTRA_LINK_OPTIONS appends arbitrary additional linker arguments after
# the standard static-runtime flags (and -Wl,--kill-at if KILL_AT is set)
# -- used by the modloader for its version.def export-list file.
function(add_mad2_mod target)
    set(options KILL_AT)
    set(oneValueArgs OUTPUT_NAME RUNTIME_SUBDIR)
    set(multiValueArgs SOURCES LIBS INCLUDE_DIRS EXTRA_LINK_OPTIONS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "add_mad2_mod(${target}): SOURCES is required")
    endif()

    if(NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME "${target}")
    endif()

    if(NOT DEFINED ARG_RUNTIME_SUBDIR)
        set(ARG_RUNTIME_SUBDIR "mods")
    endif()
    if(ARG_RUNTIME_SUBDIR STREQUAL "")
        set(runtime_dir "${CMAKE_BINARY_DIR}/dist")
    else()
        set(runtime_dir "${CMAKE_BINARY_DIR}/dist/${ARG_RUNTIME_SUBDIR}")
    endif()

    add_library(${target} SHARED ${ARG_SOURCES})

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${ARG_OUTPUT_NAME}"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${runtime_dir}"
    )

    target_compile_options(${target} PRIVATE -Wall -Wextra)

    if(ARG_LIBS)
        target_link_libraries(${target} PRIVATE ${ARG_LIBS})
    endif()

    if(ARG_INCLUDE_DIRS)
        target_include_directories(${target} PRIVATE ${ARG_INCLUDE_DIRS})
    endif()

    set(link_opts -static -static-libgcc -static-libstdc++)
    if(ARG_KILL_AT)
        list(APPEND link_opts -Wl,--kill-at)
    endif()
    if(ARG_EXTRA_LINK_OPTIONS)
        list(APPEND link_opts ${ARG_EXTRA_LINK_OPTIONS})
    endif()
    target_link_options(${target} PRIVATE ${link_opts})
endfunction()
