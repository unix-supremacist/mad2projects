#pragma once

// Own log file (mad2music.log), same per-process-logs-to-its-own-file
// convention every mod in this repo follows -- see CLAUDE.md's "Logging
// conventions" section. mad2music inherits its cwd from mad2relauncher,
// which is already the game install directory (see justfile's `_play`
// recipe), so a bare relative filename lands in the right place same as
// every DLL mod's log.
void InitLog();
void Log(const char* fmt, ...);
