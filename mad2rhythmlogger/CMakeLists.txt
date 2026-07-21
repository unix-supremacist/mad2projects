cmake_minimum_required(VERSION 3.16)

# Diagnostic runtime logger for VolcanoRave's rhythm minigame's TFBScript
# execution (mad2/mods/mad2rhythmlogger.dll) -- NOT a gameplay mod. Patches
# 4 vtable slots (the shared `execute()` virtual, confirmed at slot 29
# across the whole Op* hierarchy via Ghidra) directly in ScriptInfoLib.dll:
# OpSetValue, OpCheckValue, OpForEach, OpFindVariable. Exists because static
# analysis of level.bld alone (see docs/SCRIPT_FORMAT.md) hit a real wall:
# which local variable a given opcode reads/writes is only resolved via a
# genuine runtime arg-stack, not any field baked into the file. Logging the
# real resolved pointers while the game actually plays a song sidesteps
# that entirely -- see src/rhythmlogger.cpp for the full design and how to
# cross-reference the log back against a `mad2repack script-dump` JSON.
add_mad2_mod(mad2rhythmlogger
    SOURCES src/rhythmlogger.cpp
)
