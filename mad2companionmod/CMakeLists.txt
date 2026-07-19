cmake_minimum_required(VERSION 3.16)

# Companion-process chaos effects (mad2/mods/mad2companionmod.dll) -- one DLL
# per "launch a separate game process as a chaos effect" mod, sharing scaffolding
# (log file, dependency-wait init thread) since they're small and structurally
# identical. Registers "SM64Challenge" with mad2effects: launches a separate
# SM64 process (native Linux via mad2relauncher under Wine, or a Windows
# sm64.exe directly via CreateProcess on real Windows), blocks Mad2's own
# controller input via mad2xinput, and streams mad2xinput's raw state to SM64
# over a loopback UDP socket -- see src/sm64mod.cpp and
# include/mad2sm64_protocol.h. Also registers "JakChallenge" (src/jak1mod.cpp,
# include/mad2jak1_protocol.h), the same idea for Jak & Daxter via the vendored
# extern/jak-project -- Wine-only, see that file's header for why there's no
# native-Windows branch for it. Also registers "MarioLegacyChallenge"
# (src/mariolegacymod.cpp, include/mad2mariolegacy_protocol.h) -- same idea
# again for the vendored extern/mario-legacy (a *separate* jak-project fork,
# OpenGOAL-Mods/Super-Mario-Legacy, that swaps Mario in for Jak), Wine-only
# for the same reason.
#
# Formerly "mad2sm64mod" -- renamed once JakChallenge made that name stale
# (it registers two companion-process effects, not just SM64; now three).
# See progress.md for the rename history.
add_library(mad2companionmod SHARED
    src/sm64mod.cpp
    src/jak1mod.cpp
    src/mariolegacymod.cpp
)

set_target_properties(mad2companionmod PROPERTIES
    OUTPUT_NAME "mad2companionmod"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist/mods"
)

target_compile_options(mad2companionmod PRIVATE -Wall -Wextra)

# ws2_32 for the UDP input-streaming/control sockets (Winsock2).
target_link_libraries(mad2companionmod PRIVATE ws2_32)

target_link_options(mad2companionmod PRIVATE
    -static -static-libgcc -static-libstdc++
)
