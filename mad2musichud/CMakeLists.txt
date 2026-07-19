cmake_minimum_required(VERSION 3.16)

# Now-playing music/podcast HUD (mad2/mods/mad2musichud.dll). Listens for
# mad2music's loopback UDP status broadcasts and draws up to two lines,
# top-left below mad2playercoords -- see src/musichud.cpp and
# ../mad2podcastmod/include/mad2podcast_protocol.h.
add_mad2_mod(mad2musichud
    SOURCES src/musichud.cpp
    LIBS ws2_32
)
