cmake_minimum_required(VERSION 3.16)

# Shared effects registry (mad2/mods/mad2effects.dll). Other mods register
# named effects (an apply/clear callback pair, plus a chance "weight" that
# is pure metadata -- mad2effects itself never reads it) and any mod can
# enumerate the registry or trigger an effect by name. This is the
# switchboard mad2chaosmod and mad2twitchcontrolsmod both drive -- see
# include/mad2effects_api.h for the client contract. Same shape as
# mad2xinput/mad2config on purpose: one consistent pattern for "shared
# thing other mods depend on."
add_mad2_mod(mad2effects
    SOURCES src/effects_dll.cpp
    KILL_AT
)
