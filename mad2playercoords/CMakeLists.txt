cmake_minimum_required(VERSION 3.16)

# Player coordinate overlay: same D3D9 hook technique as mad2igttimer
# (IAT-patch Direct3DCreate9, vtable-patch IDirect3D9::CreateDevice and
# IDirect3DDevice9::EndScene) composing with both shadowfix and igttimer
# regardless of mad2/mods/ load order. Loaded from mad2/mods/ by
# mad2modloader like any other mod.
# gdi32 no longer needed directly -- GDI text rendering moved to
# mad2textrenderer.dll (see src/playercoords.cpp).
add_mad2_mod(playercoords
    SOURCES src/playercoords.cpp
)
