// Shared effects registry (mad2/mods/mad2effects.dll) -- see
// include/mad2effects_api.h for the full client contract. This file is
// purely bookkeeping + a duration timer; it never interprets an effect's
// weight and never touches any game state itself (that's each registered
// effect's own Apply/Clear callback's job).
//
// Lock-discipline rule (the one thing that will bite someone extending
// this file): NEVER call an Apply/Clear callback while holding g_Lock.
// Every function below follows the same shape: lock -> look up/mutate
// bookkeeping and copy out the callback pointer(s) to invoke -> unlock ->
// invoke callback(s) unlocked. This matters for two reasons: a callback
// may legitimately re-enter mad2effects (e.g. a "chain reaction" effect
// that calls Trigger on a different effect from inside its own Apply), and
// calling into another DLL's arbitrary code while holding your own lock is
// a standing deadlock risk regardless.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <string>
#include <vector>

#include "../include/mad2effects_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2effects]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Registry.
// ---------------------------------------------------------------------

struct EffectEntry {
    std::string name;
    std::string displayName;
    int weight = 0;
    Mad2Effects_ApplyFn apply = nullptr;
    void* applyUserdata = nullptr;
    Mad2Effects_ClearFn clear = nullptr;
    void* clearUserdata = nullptr;
    DWORD minDurationMs = 0;
    DWORD maxDurationMs = 0;
    bool active = false;
    ULONGLONG clearTicks = 0;  // 0 == no auto-clear pending (manual-clear-only or inactive)
};

static std::vector<EffectEntry> g_Effects;
static CRITICAL_SECTION g_Lock;

// Caller must hold g_Lock.
static int FindEffectIndexLocked(const char* name) {
    for (size_t i = 0; i < g_Effects.size(); ++i) {
        if (_stricmp(g_Effects[i].name.c_str(), name) == 0) return static_cast<int>(i);
    }
    return -1;
}

static void FillInfo(const EffectEntry& e, Mad2EffectInfo& outInfo) {
    snprintf(outInfo.name, sizeof(outInfo.name), "%s", e.name.c_str());
    snprintf(outInfo.displayName, sizeof(outInfo.displayName), "%s", e.displayName.c_str());
    outInfo.weight = e.weight;
    outInfo.hasDuration = e.clear != nullptr;
    outInfo.isActive = e.active;
}

// ---------------------------------------------------------------------
// Ticker thread: polls for expired auto-revert durations and calls Clear.
// Started lazily on first Register (mad2effects has no per-frame hook of
// its own to piggyback on the way D3D9-hooking overlay mods do -- this is
// the closest equivalent to "not from DllMain, but as early as it's safe
// to do real work").
// ---------------------------------------------------------------------

static std::atomic<bool> g_TickerStarted{false};

static DWORD WINAPI TickerThreadFunc(LPVOID) {
    Log("Ticker thread started.\n");
    for (;;) {
        Sleep(75);

        std::vector<std::pair<Mad2Effects_ClearFn, void*>> toClear;
        ULONGLONG now = GetTickCount64();

        EnterCriticalSection(&g_Lock);
        for (auto& e : g_Effects) {
            if (e.active && e.clearTicks != 0 && now >= e.clearTicks) {
                toClear.emplace_back(e.clear, e.clearUserdata);
                e.active = false;
                e.clearTicks = 0;
            }
        }
        LeaveCriticalSection(&g_Lock);

        for (auto& [clearFn, userdata] : toClear) {
            if (clearFn) clearFn(userdata);
        }
    }
}

static void EnsureTickerThread() {
    bool expected = false;
    if (!g_TickerStarted.compare_exchange_strong(expected, true)) return;
    HANDLE h = CreateThread(nullptr, 0, TickerThreadFunc, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        Log("CreateThread for ticker thread failed: %lu\n", GetLastError());
        g_TickerStarted.store(false);
    }
}

// ---------------------------------------------------------------------
// Exported API -- see include/mad2effects_api.h for the client contract.
// ---------------------------------------------------------------------

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Effects_Register(const Mad2EffectDesc* desc) {
    if (!desc || !desc->name || !*desc->name || !desc->apply) return FALSE;

    EnterCriticalSection(&g_Lock);
    int idx = FindEffectIndexLocked(desc->name);
    if (idx < 0) {
        g_Effects.emplace_back();
        idx = static_cast<int>(g_Effects.size()) - 1;
    }
    EffectEntry& e = g_Effects[idx];
    e.name = desc->name;
    e.displayName = desc->displayName ? desc->displayName : desc->name;
    e.weight = desc->defaultWeight;
    e.apply = desc->apply;
    e.applyUserdata = desc->applyUserdata;
    e.clear = desc->clear;
    e.clearUserdata = desc->clearUserdata;
    e.minDurationMs = desc->defaultMinDurationMs;
    e.maxDurationMs = desc->defaultMaxDurationMs;
    // Re-registering an effect resets its live state -- the old Apply/Clear
    // pair (if different) shouldn't be left half-active under the new one.
    e.active = false;
    e.clearTicks = 0;
    LeaveCriticalSection(&g_Lock);

    Log("Registered effect '%s' (display='%s' weight=%d hasDuration=%d duration=[%lu,%lu]ms)\n", desc->name,
        e.displayName.c_str(), e.weight, desc->clear != nullptr, e.minDurationMs, e.maxDurationMs);

    EnsureTickerThread();
    return TRUE;
}

extern "C" __declspec(dllexport) int WINAPI Mad2Effects_GetCount(void) {
    EnterCriticalSection(&g_Lock);
    int count = static_cast<int>(g_Effects.size());
    LeaveCriticalSection(&g_Lock);
    return count;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Effects_GetInfo(int index, Mad2EffectInfo* outInfo) {
    if (!outInfo) return FALSE;
    EnterCriticalSection(&g_Lock);
    if (index < 0 || index >= static_cast<int>(g_Effects.size())) {
        LeaveCriticalSection(&g_Lock);
        return FALSE;
    }
    FillInfo(g_Effects[index], *outInfo);
    LeaveCriticalSection(&g_Lock);
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2Effects_Trigger(const char* name, DWORD durationMsOverride) {
    if (!name) return FALSE;

    EnterCriticalSection(&g_Lock);
    int idx = FindEffectIndexLocked(name);
    if (idx < 0) {
        LeaveCriticalSection(&g_Lock);
        return FALSE;
    }
    EffectEntry& e = g_Effects[idx];
    Mad2Effects_ApplyFn applyFn = e.apply;
    void* applyUserdata = e.applyUserdata;

    if (e.clear != nullptr) {
        DWORD dur = durationMsOverride;
        if (dur == 0) {
            if (e.minDurationMs > 0 || e.maxDurationMs > 0) {
                dur = (e.minDurationMs >= e.maxDurationMs)
                          ? e.minDurationMs
                          : e.minDurationMs + static_cast<DWORD>(rand() % (e.maxDurationMs - e.minDurationMs + 1));
            }
        }
        e.active = true;
        e.clearTicks = dur > 0 ? GetTickCount64() + dur : 0;
    }
    LeaveCriticalSection(&g_Lock);

    Log("Triggered effect '%s' (durationOverride=%lu)\n", name, durationMsOverride);
    applyFn(applyUserdata);
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI Mad2Effects_ClearByName(const char* name) {
    if (!name) return;

    EnterCriticalSection(&g_Lock);
    int idx = FindEffectIndexLocked(name);
    if (idx < 0 || !g_Effects[idx].active || !g_Effects[idx].clear) {
        LeaveCriticalSection(&g_Lock);
        return;
    }
    EffectEntry& e = g_Effects[idx];
    Mad2Effects_ClearFn clearFn = e.clear;
    void* clearUserdata = e.clearUserdata;
    e.active = false;
    e.clearTicks = 0;
    LeaveCriticalSection(&g_Lock);

    Log("Cleared effect '%s' (manual)\n", name);
    clearFn(clearUserdata);
}

extern "C" __declspec(dllexport) void WINAPI Mad2Effects_ClearAll(void) {
    std::vector<std::pair<Mad2Effects_ClearFn, void*>> toClear;

    EnterCriticalSection(&g_Lock);
    for (auto& e : g_Effects) {
        if (e.active && e.clear) {
            toClear.emplace_back(e.clear, e.clearUserdata);
            e.active = false;
            e.clearTicks = 0;
        }
    }
    LeaveCriticalSection(&g_Lock);

    Log("ClearAll: clearing %zu active effect(s)\n", toClear.size());
    for (auto& [clearFn, userdata] : toClear) {
        if (clearFn) clearFn(userdata);
    }
}

extern "C" __declspec(dllexport) void WINAPI Mad2Effects_SetDisplayName(const char* name, const char* displayName) {
    if (!name || !displayName) return;
    EnterCriticalSection(&g_Lock);
    int idx = FindEffectIndexLocked(name);
    if (idx >= 0) g_Effects[idx].displayName = displayName;
    LeaveCriticalSection(&g_Lock);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_Lock);
            srand(static_cast<unsigned>(time(nullptr)));
            Log("mad2effects loaded.\n");
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
