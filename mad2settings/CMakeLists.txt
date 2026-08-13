cmake_minimum_required(VERSION 3.16)

# Shared live-settings registry (mad2/mods/mad2settings.dll). Other mods
# register a setting (a category/label, a config.cfg section/key to persist
# edits to, a type, and a raw pointer to their own already-existing live
# config global, plus an optional onChanged callback for settings that need
# to react to an edit rather than just re-reading a fresher value next
# frame) -- mad2debugmenu.dll is the only consumer that renders/edits these.
# Same shape as mad2effects/mad2config on purpose -- see
# include/mad2settings_api.h for the client contract.
add_mad2_mod(mad2settings
    SOURCES src/settings_dll.cpp
    KILL_AT
)
