cmake_minimum_required(VERSION 3.16)

# IGT/RTA/Playtime timer overlay. Hooks Direct3DCreate9 at the IAT level
# (same technique as mad2shadowfix) and then vtable-patches just
# IDirect3D9::CreateDevice and IDirect3DDevice9::EndScene -- unlike
# shadowfix this mod doesn't need to observe/rewrite most D3D9 calls, so a
# full COM proxy would be pure boilerplate; two targeted vtable patches
# are enough to get a per-frame draw callback. See igttimer.cpp for why
# this still composes correctly with shadowfix's full proxy regardless of
# mad2/mods/ load order.
# gdi32 no longer needed directly -- GDI text rendering moved to
# mad2textrenderer.dll (see src/igttimer.cpp).
add_mad2_mod(igttimer
    SOURCES src/igttimer.cpp
)
