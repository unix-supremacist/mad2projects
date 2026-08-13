cmake_minimum_required(VERSION 3.16)

# Diagnostic runtime memory-dump probe (mad2/mods/mad2climbprobe.dll) for
# the climb-ability reverse-engineering effort -- NOT a gameplay mod, same
# "not meant to stay loaded permanently" status as mad2rhythmlogger. Press
# F9 to dump wide hex regions around the already-known player/mango/cheat
# pointer chains into logs\mad2.log -- see src/climbprobe.cpp for the full
# design and how to use the captures.
add_mad2_mod(mad2climbprobe
    SOURCES src/climbprobe.cpp
)
