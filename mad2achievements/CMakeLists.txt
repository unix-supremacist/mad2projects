cmake_minimum_required(VERSION 3.16)

# Achievement registry + config.cfg-backed persistence + unlock-toast
# overlay (mad2/mods/mad2achievements.dll). No game-specific detection
# logic of its own -- see src/achievements_dll.cpp and
# include/mad2achievements_api.h. mad2achievementtracker.dll is the first
# consumer/detector.
add_mad2_mod(mad2achievements
    SOURCES src/achievements_dll.cpp
    KILL_AT
)
