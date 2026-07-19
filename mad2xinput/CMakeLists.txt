cmake_minimum_required(VERSION 3.16)

# Generic XInput hook + injection API (mad2/mods/mad2xinput.dll). Hooks
# XInputGetState at the IAT level (via aa_mad2hookutil.dll's ordinal-aware
# PatchIat -- see include/mad2xinput_api.h) and exposes the observed/
# overridden state to other mods via exported functions -- see
# include/mad2xinput_api.h for the client contract other mods build
# against.
add_mad2_mod(mad2xinput
    SOURCES src/xinput_api.cpp
    KILL_AT
)
