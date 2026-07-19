cmake_minimum_required(VERSION 3.16)

# Level-load interception (mad2/mods/mad2levelredirectmod.dll). IAT-hooks
# CreateFileA/W to redirect level archive opens per level_redirects.txt
# (written by ../mad2rando5), and tracks the actual current level for
# mad2gameeffectsmod's PlayerTeleport/MangoMutate to consume via
# include/mad2levelredirect_api.h. See src/levelredirect.cpp's file header
# for the full design, including why load order relative to
# mad2assetloader matters here.
#
# KILL_AT: this mod exports a shared API other mods resolve by name at
# runtime (Mad2LevelRedirect_GetCurrentLevel) -- see mad2effects/
# CMakeLists.txt's comment for why this flag is required for that to work.
add_mad2_mod(mad2levelredirectmod
    SOURCES src/levelredirect.cpp
    KILL_AT
)
