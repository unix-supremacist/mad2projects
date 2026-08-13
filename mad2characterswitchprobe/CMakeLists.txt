cmake_minimum_required(VERSION 3.16)

# Diagnostic runtime probe (mad2/mods/mad2characterswitchprobe.dll) testing
# whether ActorInfo::_controller reassignment is enough to switch player
# control between two already-spawned characters -- NOT a gameplay mod, same
# "not meant to stay loaded permanently" status as mad2climbprobe/
# mad2rhythmlogger/mad2metadumper/mad2shoptrace. See
# src/characterswitchprobe.cpp for the full design and hotkeys.
add_mad2_mod(mad2characterswitchprobe
    SOURCES src/characterswitchprobe.cpp
)
