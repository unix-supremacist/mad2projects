#include "log.h"

#include "../../mad2sharedlog/mad2sharedlog.h"

// InitLog() no longer needs to do anything -- Mad2Log resolves the shared
// logs\mad2.log path and its cross-DLL mutex fresh on every call (see
// mad2sharedlog.h). Kept as a function purely so DllMain in all three of
// modloader/mad2assetloader/mad2saveloader (each has its own copy of this
// file) doesn't need to change.
void InitLog() {}

void Log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Mad2Log_Write("[mad2assetloader]", format, args);
    va_end(args);
}
