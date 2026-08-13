cmake_minimum_required(VERSION 3.16)

# Diagnostic runtime logger for the Duty Free shop's item-list/membership
# logic (mad2/mods/mad2shoptrace.dll) -- NOT a gameplay mod, same
# "not meant to stay loaded permanently" status as mad2rhythmlogger, which
# this mod's technique is a direct copy of (vtable-patching the shared
# execute() virtual on specific Op* classes) -- see src/shoptrace.cpp for
# why: static analysis of DutyFreeBLD/level.bld.orig (see the shop-items
# investigation in this session's history) confirmed OpForEach's own
# target-set reference resolves to a shared/empty default object in the
# file and its cachedSet/cachedObject fields are always the runtime-unbound
# null sentinel -- exactly the same dead end mad2rhythmlogger's own Round 6
# hit for VolcanoRave. This mod hooks OpForEach/OpChangeMembership/
# OpFindSubSet/OpCheckMembership's execute() while the Duty Free level is
# actually loaded, to see which ScriptSet membership/subset operations
# really run when the shop's item lists get built.
add_mad2_mod(mad2shoptrace
    SOURCES src/shoptrace.cpp
)
