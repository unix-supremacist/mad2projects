cmake_minimum_required(VERSION 3.16)

# In-game Nuklear debug menu -- deployed as "aab_mad2debugmenu.dll" (NOT
# "mad2debugmenu.dll") so it sorts immediately after "aa_mad2hookutil.dll"
# and before every other mod, guaranteeing it's always the earliest-loaded
# D3D9-hooking mod and therefore always drawn LAST (closest to Present) --
# see src/debugmenu.cpp's file header and CLAUDE.md's "D3D9 overlay
# hooking" section for why load order maps to draw order this way.
add_mad2_mod(mad2debugmenu
    OUTPUT_NAME aab_mad2debugmenu
    SOURCES src/debugmenu.cpp
)
