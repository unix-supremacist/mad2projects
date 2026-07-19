cmake_minimum_required(VERSION 3.16)

# Shared config file mod (mad2/mods/mad2config.dll). Owns <exe dir>\config.cfg
# and exposes typed "read or seed default" accessors to other mods via
# exported functions -- see include/mad2config_api.h for the client
# contract other mods build against. Same shape as mad2xinput on purpose:
# one consistent pattern for "shared thing other mods depend on."
add_mad2_mod(mad2config
    SOURCES src/config_dll.cpp
    KILL_AT
)
