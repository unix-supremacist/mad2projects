cmake_minimum_required(VERSION 3.16)

# Title-screen cheat-apply mod (mad2/mods/mad2cheatapply.dll). Press a
# configurable key/controller combo while on the title screen to write the
# engine's built-in cheat toggles directly into game memory -- ported from
# ../mad2mod/src/main.cpp's enable_all_cheats/load_cheats_config (see
# src/cheatapply.cpp for the pointer chain and offset table). No D3D9 hook,
# no mad2effects registration -- just a background poll thread, same shape
# as mad2inputeffectsmod's "init-only background thread" pattern.
add_mad2_mod(mad2cheatapply
    SOURCES src/cheatapply.cpp
)
