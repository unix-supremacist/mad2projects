cmake_minimum_required(VERSION 3.16)

# Achievement detection driver (mad2/mods/mad2achievementtracker.dll) --
# background thread, no D3D9 hook. Registers achievement(s) with
# mad2achievements.dll and watches real game state (here: the shared
# mad2leveldetect.h current-level signal) to call Unlock() when a
# condition is met. First entry: "BeatTheGame" on the Dam_Busters ->
# Credits transition. See src/achievementtracker.cpp.
add_mad2_mod(mad2achievementtracker
    SOURCES src/achievementtracker.cpp
)
