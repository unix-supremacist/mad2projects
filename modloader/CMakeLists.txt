cmake_minimum_required(VERSION 3.16)

# Deployed as version.dll: proxies AWL.dll's real version.dll import so it
# gets loaded very early (before Mad2.exe's own entry point runs), without
# touching d3d9/xinput/dinput8 which are already in use elsewhere.
add_mad2_mod(mad2modloader
    SOURCES
        src/dllmain.cpp
        src/log.cpp
        src/mod_loader.cpp
        src/version_proxy.cpp
    OUTPUT_NAME version
    RUNTIME_SUBDIR ""
    EXTRA_LINK_OPTIONS "${CMAKE_CURRENT_SOURCE_DIR}/src/version.def"
)
