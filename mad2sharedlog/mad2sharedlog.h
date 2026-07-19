#pragma once
// Shared cross-DLL logging helper -- every DLL loaded into Mad2.exe's
// process (the modloader itself, plus every mad2/mods/*.dll) writes into
// ONE merged file, <exe dir>\logs\mad2.log, instead of each mod owning a
// separate <name>.log. Each line keeps that mod's existing tag prefix
// (e.g. "[mad2xinput]", "[SHADOWPASS]") so the merged file stays
// attributable -- see CLAUDE.md's "Logging conventions" section.
//
// Why a named Win32 mutex, not a CRITICAL_SECTION: these are ~21
// independent DLLs loaded into the SAME process, but DLLs do NOT share
// static/global state with each other -- each gets its own copy of every
// global, including a CRITICAL_SECTION. (This was a real, pre-existing bug
// in modloader/mad2assetloader/mad2saveloader's shared log.cpp: three
// separate DLL copies of the same source file, each with its OWN
// CRITICAL_SECTION instance guarding only that one DLL's own writes --
// never a real cross-DLL guarantee even before this file existed.) A
// NAMED mutex is a kernel object looked up by name: every DLL's
// independent CreateMutexA call with the same name gets a handle to the
// SAME underlying object, which is what actually serializes writes across
// DLL boundaries.
//
// Header-only/static inline, included directly by each mod's own tiny
// Log(fmt, ...) wrapper (kept at every existing call site so nothing else
// in the codebase needs to change) -- no GetProcAddress/load-order
// concerns, since a named mutex + a fixed file path need no DllMain-time
// setup in some other "provider" DLL that might not have loaded yet. This
// matters concretely: the modloader itself must be able to log before ANY
// mad2/mods/*.dll -- including a hypothetical logging-provider DLL -- has
// loaded at all.
//
// The file is opened, written, and closed on every call rather than kept
// open persistently -- persistent per-DLL FILE* handles to the same
// underlying file would each buffer/seek independently and can't be made
// safe just by adding a mutex around the write. Open+append+close under
// the mutex is simple and correct; these are diagnostic logs, not a
// per-frame hot path (every mod that logs per-frame diagnostics already
// throttles those calls -- see e.g. mad2xinput's 2s-throttled dump,
// mad2shadowfix's default-silent log level).
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#define MAD2_LOG_MUTEX_NAME "Local\\Mad2ModLoaderSharedLogMutex"

// Resolves <exe dir>\logs\mad2.log from the EXE's own path
// (GetModuleFileNameA(NULL, ...)), not the current working directory --
// the modloader's DllMain can run before Mad2.exe's cwd is necessarily
// settled (this mirrors modloader/src/log.cpp's pre-existing reasoning).
// Also ensures the logs\ subdirectory exists. Recomputed on every call
// instead of cached: it's one cheap WinAPI call, and every DLL has its own
// copy of any "cached" static anyway (see file header), so caching here
// wouldn't actually save the cross-DLL cost.
static inline void Mad2Log_GetPath(char* outPath, size_t outSize) {
    char exeDir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    char* slash2 = strrchr(exeDir, '/');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    if (slash) {
        *(slash + 1) = '\0';
    } else {
        exeDir[0] = '\0';
    }

    char logsDir[MAX_PATH];
    snprintf(logsDir, sizeof(logsDir), "%slogs", exeDir);
    CreateDirectoryA(logsDir, nullptr);  // no-op (GetLastError()==ALREADY_EXISTS) if it exists

    snprintf(outPath, outSize, "%s\\mad2.log", logsDir);
}

// Core write, taking an already-started va_list -- see Mad2Log below for
// the varargs entry point every mod's own Log(fmt, ...) wrapper should
// call from its body.
static inline void Mad2Log_Write(const char* tag, const char* fmt, va_list args) {
    static HANDLE s_mutex = nullptr;
    if (!s_mutex) s_mutex = CreateMutexA(nullptr, FALSE, MAD2_LOG_MUTEX_NAME);
    // WAIT_ABANDONED is treated the same as WAIT_OBJECT_0 (ownership still
    // granted to us) -- if some other mod's DLL crashed while holding this,
    // Windows already self-heals the mutex state, no special handling needed.
    if (s_mutex) WaitForSingleObject(s_mutex, INFINITE);

    char logPath[MAX_PATH];
    Mad2Log_GetPath(logPath, sizeof(logPath));

    FILE* f = fopen(logPath, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag);
        vfprintf(f, fmt, args);
        fclose(f);
    }

    if (s_mutex) ReleaseMutex(s_mutex);
}

// Varargs entry point. `tag` is this mod's own bracketed tag (e.g.
// "[mad2xinput]"), prefixed on every line so the merged mad2.log file
// stays attributable per mod.
static inline void Mad2Log(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write(tag, fmt, args);
    va_end(args);
}
