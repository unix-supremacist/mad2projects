cmake_minimum_required(VERSION 3.16)

# Shared IAT/vtable hooking primitives (deployed as
# mad2/mods/aa_mad2hookutil.dll -- see include/mad2hookutil_api.h for why
# the "aa_" prefix is load-bearing, not cosmetic: it must sort/load before
# every other mod so consumers can resolve it synchronously from their own
# DllMain). Exports PatchIat/PatchIatAllModules/PatchVTableSlot, replacing
# what used to be 11 mods' worth of near-identical inlined copies plus
# mad2xinput's separate ordinal-aware variant -- see src/hookutil.cpp.
add_mad2_mod(mad2hookutil
    SOURCES src/hookutil.cpp
    OUTPUT_NAME aa_mad2hookutil
    KILL_AT
)
