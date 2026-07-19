cmake_minimum_required(VERSION 3.16)

# Podcast chaos effect (mad2/mods/mad2podcastmod.dll). Registers
# "PodcastBreak" with mad2effects: asks mad2music (a native Linux process,
# always running for the whole session -- see ../mad2relauncher/music.go) to
# play a random spoken-word clip over loopback UDP, and waits for a
# completion reply to self-clear -- see src/podcastmod.cpp and
# include/mad2podcast_protocol.h.
add_mad2_mod(mad2podcastmod
    SOURCES src/podcastmod.cpp
    LIBS ws2_32
)
