cmake_minimum_required(VERSION 3.16)

# Twitch chat overlay (mad2/mods/twitchchat.dll). Connects anonymously
# (read-only "justinfanNNNNNN" login, no OAuth token needed) to
# irc.chat.twitch.tv and renders incoming chat messages in-process via
# D3D9, with inline emotes loaded at runtime from a user-supplied directory
# (ships with none -- see [TwitchChat] EmotesDir in config.cfg).
#
# Hooking strategy: identical to mad2igttimer -- Direct3DCreate9/Ex hooked
# at the IAT level, then IDirect3D9::CreateDevice/IDirect3DDevice9::EndScene
# vtable-patched (slots 16/42). See igttimer.cpp's file header for why this
# composes with shadowfix/inputdisplay/igttimer regardless of mad2/mods/
# load order.
#
# PNG/GIF decoding uses a vendored copy of stb_image.h (single-header,
# public domain, third_party/stb_image.h) with only the PNG and GIF
# decoders compiled in -- no D3DX, no libwebp (see twitchchat.cpp for why
# .webp emotes aren't supported).
# gdi32 no longer needed directly -- GDI text rendering moved to
# mad2textrenderer.dll (see src/twitchchat.cpp). ws2_32 still needed for
# the Twitch IRC client (Winsock2).
add_mad2_mod(twitchchat
    SOURCES src/twitchchat.cpp
    LIBS ws2_32
    INCLUDE_DIRS third_party
)
