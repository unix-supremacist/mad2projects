cmake_minimum_required(VERSION 3.16)

# Twitch channel-points-redemption -> effect trigger (mad2/mods/mad2twitchcontrolsmod.dll).
# Separate from mad2twitchchat (read-only anonymous chat overlay) --
# reward redemptions require an authenticated EventSub WebSocket
# connection, which anonymous IRC cannot see. Uses WinHTTP (verified
# working for WSS under this repo's Proton/Wine target via a standalone
# spike before this mod was written) instead of Winsock, since WinHTTP
# handles the WebSocket-over-TLS upgrade and the plain HTTPS subscription-
# creation call with the same API and no hand-rolled TLS. See
# src/twitchcontrols.cpp.
add_mad2_mod(mad2twitchcontrolsmod
    SOURCES src/twitchcontrols.cpp
    LIBS winhttp
)
