cmake_minimum_required(VERSION 3.16)

# Shared GDI mask/outline text-to-texture rasterizer + D3D9 state save/
# restore + textured-quad drawing (mad2/mods/mad2textrenderer.dll) -- see
# include/mad2textrenderer_api.h for the client contract. Replaces what
# used to be 5 mods' worth of near-identical GDI text rendering plus
# quad-drawing boilerplate -- see src/textrenderer.cpp.
add_mad2_mod(mad2textrenderer
    SOURCES src/textrenderer.cpp
    LIBS gdi32
    KILL_AT
)
