cmake_minimum_required(VERSION 3.16)

# The root project() only enables CXX (every other mod is C++-only); this
# is the one target that also needs a C compiler, for xdelta3.c below.
enable_language(C)

# In-process xdelta patch applier for .pak string-table edits
# (mad2/mods/mad2xdeltamod.dll) -- see src/xdeltamod.cpp for the full
# design and the igCore.dll reverse-engineering trail it rests on. Depends
# on aa_mad2hookutil.dll (its "aa_" prefix guarantees it's already loaded)
# for the InstallInlineHook primitive this mod added.
set(XDELTA3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/jak-project/third-party/xdelta3/xdelta3")

add_mad2_mod(mad2xdeltamod
    SOURCES
        src/xdeltamod.cpp
        "${XDELTA3_DIR}/xdelta3.c"
    INCLUDE_DIRS "${XDELTA3_DIR}"
)

# See ../mad2xdeltagen/CMakeLists.txt for the offline patch-generation
# side -- these must match (32-bit here vs. the native host's SIZEOF_SIZE_T
# there) so patches this repo generates are always readable by this mod's
# decoder.
target_compile_definitions(mad2xdeltamod PRIVATE
    XD3_USE_LARGEFILE64=0
    XD3_USE_LARGESIZET=0
    SIZEOF_UNSIGNED_INT=4
    SIZEOF_SIZE_T=4
    XD3_ENCODER=1
    XD3_DEBUG=0
)

# xdelta3.c is C, not C++ -- add_mad2_mod's target_compile_options applies
# -Wall -Wextra to the whole target regardless of source language, which is
# fine, but the naked-function asm in xdeltamod.cpp needs GNU inline asm
# (already the default for this toolchain, no extra flag needed).
