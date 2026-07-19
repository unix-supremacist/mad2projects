cmake_minimum_required(VERSION 3.16)

# Chaos-mode director loop (mad2/mods/mad2chaosmod.dll). No D3D9 hook of
# its own -- a background thread that periodically triggers a randomly
# weighted registered effect via mad2effects. See src/chaosmod.cpp.
add_mad2_mod(mad2chaosmod
    SOURCES src/chaosmod.cpp
)
