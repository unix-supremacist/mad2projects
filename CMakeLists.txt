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

# Shared live-settings registry other mods build against
# (mad2/mods/mad2settings.dll) -- see mad2settings/include/mad2settings_api.h.
# mad2debugmenu.dll is the only consumer that renders/edits these.
add_subdirectory(mad2settings)

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

# Mirror Mode: persistent horizontal-flip + stick-mirror toggle
# (mad2/mods/zz_mad2mirrormod.dll -- also "zz_"-prefixed, see its own
# CMakeLists.txt/src/mirrormod.cpp for why). Not a mad2effects chaos effect.
add_subdirectory(mad2mirrormod)

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

# Diagnostic runtime logger for VolcanoRave's rhythm minigame's TFBScript
# execution (mad2/mods/mad2rhythmlogger.dll) -- NOT a gameplay mod, see
# mad2rhythmlogger/src/rhythmlogger.cpp and docs/SCRIPT_FORMAT.md.
add_subdirectory(mad2rhythmlogger)

# Diagnostic runtime memory-dump probe for the climb-ability reverse-
# engineering effort (mad2/mods/mad2climbprobe.dll) -- NOT a gameplay mod,
# same status as mad2rhythmlogger above. See mad2climbprobe/src/climbprobe.cpp.
add_subdirectory(mad2climbprobe)

# Diagnostic runtime reflection-metadata dumper (mad2/mods/mad2metadumper.dll)
# -- NOT a gameplay mod, same status as mad2climbprobe/mad2rhythmlogger
# above. See mad2metadumper/src/metadumper.cpp.
add_subdirectory(mad2metadumper)

# Diagnostic runtime logger for the Duty Free shop's item-list/membership
# logic (mad2/mods/mad2shoptrace.dll) -- NOT a gameplay mod, same status as
# mad2rhythmlogger/mad2climbprobe/mad2metadumper above. See
# mad2shoptrace/src/shoptrace.cpp.
add_subdirectory(mad2shoptrace)

# Diagnostic runtime probe for the "switch playable character at any time"
# feasibility question (mad2/mods/mad2characterswitchprobe.dll) -- NOT a
# gameplay mod, same status as mad2rhythmlogger/mad2climbprobe/
# mad2metadumper/mad2shoptrace above. See
# mad2characterswitchprobe/src/characterswitchprobe.cpp.
add_subdirectory(mad2characterswitchprobe)

# NES chaos effect (mad2/mods/mad2nesmod.dll) -- dlopens a vendored
# libretro core directly inside the Mad2 process, see
# mad2nesmod/src/nesmod.cpp. Core binary + ROM are fetched/staged
# separately (not CMake build outputs) -- see `just fetch-libretro-fceumm`
# and add_deploy_target()'s own copy commands below.
add_subdirectory(mad2nesmod)

# In-process xdelta patch applier for .pak string-table edits
# (mad2/mods/mad2xdeltamod.dll) -- applies <exe dir>/xdelta_patches/*.mad2xdelta
# to a loaded .pak's decompressed IGZ section bytes in memory, so mods can
# ship a few-KB binary diff instead of a whole patched .bld. Depends on
# aa_mad2hookutil.dll's InstallInlineHook. See mad2xdeltamod/src/xdeltamod.cpp
# and the offline patch-generation tool, mad2xdeltagen/ (a separate, native
# Linux CMake project -- not built by this one, see its own CMakeLists.txt).
add_subdirectory(mad2xdeltamod)

# Minigame-mode fix (mad2/mods/mad2minigamemodefix.dll) -- forces the
# game's own "Minigame Launch Mode" flag off on every level except a small
# whitelist of real minigames confirmed to need it, fixing DrMelmen/
# VolcanoRave and other story levels that merely reference shared minigame
# code always acting like a minigame. See src/minigamemodefix.cpp for the
# full investigation writeup and the live-object scan it's built on.
add_subdirectory(mad2minigamemodefix)

# In-game Nuklear debug menu (mad2/mods/aab_mad2debugmenu.dll -- see its own
# CMakeLists.txt for why the "aab_" prefix is load-bearing, not cosmetic).
# Renders/edits mad2settings.dll's registry and mad2effects.dll's/
# mad2achievements.dll's registries -- see src/debugmenu.cpp.
add_subdirectory(mad2debugmenu)

# Achievement registry + config.cfg-backed persistence + unlock-toast
# overlay (mad2/mods/mad2achievements.dll) -- see
# mad2achievements/include/mad2achievements_api.h.
add_subdirectory(mad2achievements)

# Achievement detection driver (mad2/mods/mad2achievementtracker.dll) --
# registers/unlocks achievements with mad2achievements.dll by watching real
# game state (first entry: "BeatTheGame" on the Dam_Busters -> Credits
# transition). See src/achievementtracker.cpp.
add_subdirectory(mad2achievementtracker)

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
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2mirrormod>" "${dir}/mods/zz_mad2mirrormod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2chaosmod>" "${dir}/mods/mad2chaosmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2effectshud>" "${dir}/mods/mad2effectshud.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2twitchcontrolsmod>" "${dir}/mods/mad2twitchcontrolsmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2companionmod>" "${dir}/mods/mad2companionmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2podcastmod>" "${dir}/mods/mad2podcastmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2musichud>" "${dir}/mods/mad2musichud.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2gameeffectsmod>" "${dir}/mods/mad2gameeffectsmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/mad2gameeffectsmod/data/coordinates.yaml" "${dir}/coordinates.yaml"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2levelredirectmod>" "${dir}/mods/mad2levelredirectmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2rhythmlogger>" "${dir}/mods/mad2rhythmlogger.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2climbprobe>" "${dir}/mods/mad2climbprobe.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2metadumper>" "${dir}/mods/mad2metadumper.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2shoptrace>" "${dir}/mods/mad2shoptrace.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2xdeltamod>" "${dir}/mods/mad2xdeltamod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2nesmod>" "${dir}/mods/mad2nesmod.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2minigamemodefix>" "${dir}/mods/mad2minigamemodefix.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2characterswitchprobe>" "${dir}/mods/mad2characterswitchprobe.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2settings>" "${dir}/mods/mad2settings.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2debugmenu>" "${dir}/mods/aab_mad2debugmenu.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2achievements>" "${dir}/mods/mad2achievements.dll"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:mad2achievementtracker>" "${dir}/mods/mad2achievementtracker.dll"
        DEPENDS mad2modloader mad2hookutil mad2textrenderer shadowfix mad2assetloader mad2saveloader mad2xinput mad2config igttimer playercoords inputdisplay twitchchat mad2effects mad2inputeffectsmod mad2graphicseffectmod mad2mirrormod mad2chaosmod mad2effectshud mad2twitchcontrolsmod mad2companionmod mad2podcastmod mad2musichud mad2gameeffectsmod mad2levelredirectmod mad2rhythmlogger mad2climbprobe mad2metadumper mad2shoptrace mad2xdeltamod mad2nesmod mad2minigamemodefix mad2characterswitchprobe mad2settings mad2debugmenu mad2achievements mad2achievementtracker
        COMMENT "Deploying version.dll and mods/*.dll to ${dir}"
    )
endfunction()

add_deploy_target(mad2 "${GAME_DIR}")
add_deploy_target(mad2russia "${CMAKE_CURRENT_SOURCE_DIR}/mad2russia")
add_deploy_target(mad2demo "${CMAKE_CURRENT_SOURCE_DIR}/mad2demo")

# Default `deploy` target keeps deploying to GAME_DIR (mad2/ by default) for
# backwards compatibility.
add_custom_target(deploy DEPENDS deploy-mad2)
