cmake_minimum_required(VERSION 3.16)

# Minigame-mode fix (mad2/mods/mad2minigamemodefix.dll). Forces the game's
# own "Minigame Launch Mode" flag (a live ScriptInfoLib.dll variable found
# via mad2climbprobe's live-object-naming scan -- see docs/ and that mod's
# own commit history for the full investigation) to 0 on every level except
# a small whitelist of real minigames confirmed to blackscreen without it.
# Fixes: DrMelmen and VolcanoRave (the two "dual mode" levels) always
# behaving as minigames even when entered as story levels; and, as a nice
# side effect, gives several minigames (Card_Match_Game, golf, animal
# chess) their story-mode goal display instead of the minigame-only HUD.
# See src/minigamemodefix.cpp for the whitelist and the live-object scan.
add_mad2_mod(mad2minigamemodefix
    SOURCES src/minigamemodefix.cpp
)
