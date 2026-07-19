cmake_minimum_required(VERSION 3.16)

# Active chaos-effects HUD (mad2/mods/mad2effectshud.dll). Reads
# mad2effects' registry every frame and draws a bottom-right list of
# whichever effects are currently active -- both a player-facing "what's
# happening to me right now" readout and a debugging aid for correlating
# what's on screen with what mad2effects thinks is active. See
# src/effectshud.cpp.
add_mad2_mod(mad2effectshud
    SOURCES src/effectshud.cpp
)
