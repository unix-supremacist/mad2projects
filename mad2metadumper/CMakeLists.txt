cmake_minimum_required(VERSION 3.16)

# Diagnostic runtime reflection-metadata dumper (mad2/mods/mad2metadumper.dll)
# -- NOT a gameplay mod, same "not meant to stay loaded permanently" status
# as mad2rhythmlogger/mad2climbprobe. Walks the Alchemy engine's own live
# igArkCore class registry (igCore.dll) on F4 and dumps, for every
# registered igMetaObject class: its parent class and its full field list
# (name + byte offset), including inherited fields -- straight from the
# engine's real reflection data, not decompiled/guessed per class. See
# src/metadumper.cpp for the full design and docs/SCRIPT_FORMAT.md /
# docs/CLASS_SCHEMA.md for why this matters (current schema coverage is
# ~10-20% of real fields, derived by hand one arkRegisterInitialize at a
# time).
add_mad2_mod(mad2metadumper
    SOURCES src/metadumper.cpp
)
