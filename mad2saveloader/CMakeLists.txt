cmake_minimum_required(VERSION 3.16)

# Redirects the game's save files from the Wine prefix's fake per-user
# Documents\Activision\Madagascar 2\ folder to a "saves" folder next to
# Mad2.exe. Hooks CreateFileA/W the same way mad2assetloader does,
# chaining onto whatever hook (if any) another mod already installed on
# the same import so both compose regardless of mad2/mods/ load order.
add_mad2_mod(mad2saveloader
    SOURCES
        src/dllmain.cpp
        src/save_redirect.cpp
        src/registry_redirect.cpp
        src/loadlibrary_hook.cpp
        src/log.cpp
)
