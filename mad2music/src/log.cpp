#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

static FILE* g_LogFile = nullptr;

void InitLog() {
    if (g_LogFile) return;
    g_LogFile = fopen("mad2music.log", "w");
    if (g_LogFile) setvbuf(g_LogFile, nullptr, _IONBF, 0);
}

void Log(const char* fmt, ...) {
    if (!g_LogFile) InitLog();
    if (!g_LogFile) return;

    time_t now = time(nullptr);
    struct tm tmNow;
#ifdef _WIN32
    localtime_s(&tmNow, &now);  // reversed arg order vs. POSIX localtime_r
#else
    localtime_r(&now, &tmNow);
#endif
    fprintf(g_LogFile, "[%02d:%02d:%02d] ", tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_LogFile, fmt, args);
    va_end(args);
}
