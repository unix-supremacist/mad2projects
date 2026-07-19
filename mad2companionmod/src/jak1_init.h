#pragma once
// Entry points for jak1mod.cpp's "JakChallenge" and mariolegacymod.cpp's
// "MarioLegacyChallenge" effects, called from sm64mod.cpp's
// DllMain/InitThreadFunc/DLL_PROCESS_DETACH -- all three effects live in the
// same DLL (mad2companionmod.dll, formerly mad2sm64mod.dll) and share one
// init thread's dependency-wait loop, see sm64mod.cpp's InitThreadFunc for
// why.
#include <cstdarg>

// Registers "JakChallenge" with mad2effects. Must be called after
// mad2config/mad2xinput/mad2effects are all confirmed resolvable (same
// precondition as SM64Challenge's own registration).
void Jak1_RegisterEffect();

// Mirrors sm64mod.cpp's own DLL_PROCESS_DETACH teardown: clears the effect
// if active, closes sockets. Does NOT touch the log file -- that's a single
// handle now owned entirely by sm64mod.cpp, see Companion_LogV below.
void Jak1_Shutdown();

// Same two entry points, for mariolegacymod.cpp's "MarioLegacyChallenge".
void MarioLegacy_RegisterEffect();
void MarioLegacy_Shutdown();

// Shared logger, defined in sm64mod.cpp: both source files in this DLL log
// to the same file (mad2companionmod.log) through this one mutex-guarded
// FILE*, rather than each keeping its own independent handle (see
// sm64mod.cpp's logging section header for the truncate-vs-append race that
// used to cause). jak1mod.cpp's own Log() wrapper forwards here with
// prefix="[Jak1] ".
void Companion_LogV(const char* prefix, const char* fmt, va_list args);
