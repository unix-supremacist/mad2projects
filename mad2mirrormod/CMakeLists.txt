cmake_minimum_required(VERSION 3.16)

# Mirror Mode: horizontally flips the rendered frame and inverts the
# left/right analog stick X axis to match -- a persistent full-session
# toggle (config.cfg [Mirror] Enabled, or a live ToggleKey), not a
# randomized mad2effects chaos effect. Deployed as
# mad2/mods/zz_mad2mirrormod.dll -- same "zz_" load-order reasoning as
# zz_mad2graphicseffectmod (destructively captures+redraws the whole
# backbuffer every frame); see src/mirrormod.cpp's file header for the
# HUD-legibility tradeoff this load-order choice buys.
add_mad2_mod(mad2mirrormod
    SOURCES src/mirrormod.cpp
    OUTPUT_NAME zz_mad2mirrormod
)
