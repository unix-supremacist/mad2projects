cmake_minimum_required(VERSION 3.16)

# Asset redirection mod: redirects Content/Streams/win/* file opens to
# Content/Streams/mod/* when a matching file exists there, letting mods
# override game assets without touching the original archives. Loaded
# from mad2/mods/ by mad2modloader like any other mod DLL.
add_mad2_mod(mad2assetloader
    SOURCES
        src/dllmain.cpp
        src/file_redirect.cpp
        src/loadlibrary_hook.cpp
        src/log.cpp
)
