cmake_minimum_required(VERSION 3.16)
project(mad2mods LANGUAGES CXX)

# Use ccache to speed up rebuilds when available.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mad2" CACHE PATH "Game install directory to deploy into")

# add_mad2_mod() -- shared add_library/set_target_properties/link-options
# boilerplate for every mod DLL target below (see cmake/add_mad2_mod.cmake
# and improvement-plan.md item 5).
include(cmake/add_mad2_mod.cmake)

# Core mod loader: builds version.dll (deployed to GAME_DIR).
add_subdirectory(modloader)

# Shared IAT/vtable hooking primitives (mad2/mods/aa_mad2hookutil.dll) --
# see mad2hookutil/include/mad2hookutil_api.h for the client contract and
# why it's deployed with an "aa_" sorts-first prefix. Added early since
# most other mods below resolve it from their own DllMain.
add_subdirectory(mad2hookutil)

# Shared GDI text-to-texture rasterizer + quad-drawing overlay helpers
# (mad2/mods/mad2textrenderer.dll) -- see
# mad2textrenderer/include/mad2textrenderer_api.h.
add_subdirectory(mad2textrenderer)

# Shadow-pass diagnostics/experiment mod (mad2/mods/shadowfix.dll).
add_subdirectory(mad2shadowfix)

# Content/Streams asset redirection mod (mad2/mods/mad2assetloader.dll).
add_subdirectory(mad2assetloader)

# Save-file redirection mod (mad2/mods/mad2saveloader.dll).
add_subdirectory(mad2saveloader)

# Generic XInput hook + injection API other mods build against
# (mad2/mods/mad2xinput.dll) -- see mad2xinput/include/mad2xinput_api.h.
add_subdirectory(mad2xinput)

# Shared config file mod other mods build against
# (mad2/mods/mad2config.dll) -- see mad2config/include/mad2config_api.h.
add_subdirectory(mad2config)

# Shared chaos-effects registry other mods build against
# (mad2/mods/mad2effects.dll) -- see mad2effects/include/mad2effects_api.h.
add_subdirectory(mad2effects)

# IGT/RTA/Playtime timer overlay (mad2/mods/igttimer.dll).
add_subdirectory(mad2igttimer)

# Player coordinate overlay (mad2/mods/playercoords.dll).
add_subdirectory(mad2playercoords)

# Controller input display overlay (mad2/mods/inputdisplay.dll).
add_subdirectory(mad2inputdisplay)

# Twitch chat overlay with runtime-loaded emotes (mad2/mods/twitchchat.dll).
add_subdirectory(mad2twitchchat)

# Joystick-reversal chaos effect (mad2/mods/mad2inputeffectsmod.dll).
add_subdirectory(mad2inputeffectsmod)

# Graphics chaos effects (mad2/mods/zz_mad2graphicseffectmod.dll -- see its
# own CMakeLists.txt for why the "zz_" prefix is load-bearing, not cosmetic).
add_subdirectory(mad2graphicseffectmod)

# Chaos-mode director loop (mad2/mods/mad2chaosmod.dll).
add_subdirectory(mad2chaosmod)

# Active chaos-effects HUD (mad2/mods/mad2effectshud.dll).
add_subdirectory(mad2effectshud)

# Twitch channel-points-redemption -> effect trigger (mad2/mods/mad2twitchcontrolsmod.dll).
add_subdirectory(mad2twitchcontrolsmod)

# SM64 + Jak1 companion-process chaos effects (mad2/mods/mad2companionmod.dll --
# formerly mad2sm64mod, renamed once it also grew JakChallenge).
add_subdirectory(mad2companionmod)

# Podcast chaos effect (mad2/mods/mad2podcastmod.dll) -- triggers mad2music,
# a native Linux process supervised by mad2relauncher (not built by this
# CMake project -- see mad2music/CMakeLists.txt and `just build-music-linux`).
add_subdirectory(mad2podcastmod)

# Now-playing music/podcast HUD (mad2/mods/mad2musichud.dll) -- listens for
# mad2music's status broadcasts, see mad2musichud/src/musichud.cpp.
add_subdirectory(mad2musichud)

# Game-state chaos effects: object shuffle, mango mutate, player teleport
# (mad2/mods/mad2gameeffectsmod.dll).
add_subdirectory(mad2gameeffectsmod)

# Level-load interception + current-level tracking for ../mad2rando5
# (mad2/mods/mad2levelredirectmod.dll). Runtime mad2/mods/ load order is
# alphabetical by filename (mod_loader.cpp), not add_subdirectory order --
# see mad2levelredirectmod/src/levelredirect.cpp's file header for why this
# mod's name has to keep sorting after "mad2assetloader".
add_subdirectory(mad2levelredirectmod)

# Title-screen cheat-apply mod (mad2/mods/mad2cheatapply.dll) -- ported from
# ../mad2mod's enable_all_cheats/load_cheats_config, see
# mad2cheatapply/src/cheatapply.cpp for the pointer chain and cheat table.
add_subdirectory(mad2cheatapply)

# Defines a `deploy-<name>` target that copies version.dll + mods/*.dll into
# `dir`. Used for each game install (mad2, mad2russia, mad2demo) so the same
# build output can be deployed to whichever install(s) you're testing.
function(add_deploy_target name dir)
    add_custom_target(deploy-${name}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}/mods"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2modloader>" "${dir}/version.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2hookutil>" "${dir}/mods/aa_mad2hookutil.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2textrenderer>" "${dir}/mods/mad2textrenderer.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:shadowfix>" "${dir}/mods/shadowfix.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2assetloader>" "${dir}/mods/mad2assetloader.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2saveloader>" "${dir}/mods/mad2saveloader.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2xinput>" "${dir}/mods/mad2xinput.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2config>" "${dir}/mods/mad2config.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:igttimer>" "${dir}/mods/igttimer.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:playercoords>" "${dir}/mods/playercoords.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:inputdisplay>" "${dir}/mods/inputdisplay.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:twitchchat>" "${dir}/mods/twitchchat.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2effects>" "${dir}/mods/mad2effects.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2inputeffectsmod>" "${dir}/mods/mad2inputeffectsmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2graphicseffectmod>" "${dir}/mods/zz_mad2graphicseffectmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2chaosmod>" "${dir}/mods/mad2chaosmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2effectshud>" "${dir}/mods/mad2effectshud.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2twitchcontrolsmod>" "${dir}/mods/mad2twitchcontrolsmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2companionmod>" "${dir}/mods/mad2companionmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2podcastmod>" "${dir}/mods/mad2podcastmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2musichud>" "${dir}/mods/mad2musichud.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2gameeffectsmod>" "${dir}/mods/mad2gameeffectsmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/mad2gameeffectsmod/data/coordinates.yaml" "${dir}/coordinates.yaml"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2levelredirectmod>" "${dir}/mods/mad2levelredirectmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2cheatapply>" "${dir}/mods/mad2cheatapply.dll"
        DEPENDS mad2modloader mad2hookutil mad2textrenderer shadowfix mad2assetloader mad2saveloader mad2xinput mad2config igttimer playercoords inputdisplay twitchchat mad2effects mad2inputeffectsmod mad2graphicseffectmod mad2chaosmod mad2effectshud mad2twitchcontrolsmod mad2companionmod mad2podcastmod mad2musichud mad2gameeffectsmod mad2levelredirectmod mad2cheatapply
        COMMENT "Deploying version.dll and mods/*.dll to ${dir}"
    )
endfunction()

add_deploy_target(mad2 "${GAME_DIR}")
add_deploy_target(mad2russia "${CMAKE_CURRENT_SOURCE_DIR}/mad2russia")
add_deploy_target(mad2demo "${CMAKE_CURRENT_SOURCE_DIR}/mad2demo")

# Default `deploy` target keeps deploying to GAME_DIR (mad2/ by default) for
# backwards compatibility.
add_custom_target(deploy DEPENDS deploy-mad2)
