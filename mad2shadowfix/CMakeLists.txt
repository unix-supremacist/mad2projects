cmake_minimum_required(VERSION 3.16)

# Shadow-pass diagnostics/experiment mod: hooks Direct3DCreate9/Ex via IAT
# patching (works alongside whatever d3d9.dll backend is actually active
# instead of replacing it), loaded from mad2/mods/ by mad2modloader.
add_mad2_mod(shadowfix
    SOURCES src/shadowfix.cpp
)
