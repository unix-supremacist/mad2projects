#pragma once
// Public client interface for mad2achievements.dll (mad2/mods/mad2achievements.dll).
//
// A generic achievement registry + persistence + unlock-toast overlay, same
// non-opinionated "registry, not owner" role as mad2effects.dll (see
// mad2effects/include/mad2effects_api.h): mad2achievements holds no
// game-specific detection logic of its own. Any mod can register an
// achievement (a stable name + display text) and call Unlock() once it
// observes that achievement's own condition -- mad2achievementtracker.dll
// is the first such detector (level-transition-based achievements), but
// nothing about this API is specific to it; a future achievement tied to,
// say, mango collection could just as well be detected and unlocked
// directly from mad2gameeffectsmod.
//
// Persistence is a small dedicated file, "<exe dir>achievements.dat" --
// deliberately NOT config.cfg (achievement unlocks are save progress, not
// a tunable setting, and don't belong interleaved with mod configuration).
// One unlocked achievement name per line, plain text, append-only. See
// src/achievements_dll.cpp.
//
// mad2/mods/*.dll load order is not guaranteed, so consumers don't link
// against mad2achievements.dll at build time -- resolve lazily via
// Mad2Achievements_Resolve(), same pattern as every other shared-API mod.
#include <windows.h>

#define MAD2ACHIEVEMENTS_DLL_NAME "mad2achievements.dll"
#define MAD2ACHIEVEMENTS_MAX_NAME 64
#define MAD2ACHIEVEMENTS_MAX_DISPLAYNAME 128
#define MAD2ACHIEVEMENTS_MAX_DESCRIPTION 256

struct Mad2AchievementDesc {
    const char* name;         // Stable unique id, e.g. "BeatTheGame". Matched
                               // case-insensitively by Unlock/IsUnlocked. Copied
                               // internally -- the pointer need not outlive this call.
    const char* displayName;  // Human-readable, e.g. "Beat The Game".
    const char* description;  // Longer flavor text, e.g. "Reach the credits."
};

struct Mad2AchievementInfo {
    char name[MAD2ACHIEVEMENTS_MAX_NAME];
    char displayName[MAD2ACHIEVEMENTS_MAX_DISPLAYNAME];
    char description[MAD2ACHIEVEMENTS_MAX_DESCRIPTION];
    BOOL unlocked;
};

// Registers (or, if `name` already exists, replaces the display text of) an
// achievement. Unlock state is resolved synchronously against the
// already-loaded achievements.dat at registration time (that file is
// read once, from this DLL's own DllMain, before any other code in it can
// run -- no "might not be ready yet" dependency). Returns FALSE only on a
// hard failure (null desc/name).
typedef BOOL(WINAPI* Mad2Achievements_Register_t)(const Mad2AchievementDesc* desc);

// Number of currently-registered achievements, for iterating GetInfo(0..count).
typedef int(WINAPI* Mad2Achievements_GetCount_t)(void);

// Snapshot of the achievement at `index` (registration order). Returns
// FALSE if index is out of range.
typedef BOOL(WINAPI* Mad2Achievements_GetInfo_t)(int index, Mad2AchievementInfo* outInfo);

// Unlocks the named achievement: no-op if already unlocked, so it's safe
// to call this every time the underlying condition is observed rather than
// needing your own "already unlocked" guard. On a fresh unlock: appends
// `name` to achievements.dat, logs, and shows an on-screen toast for a few
// seconds. No-op (logged) if `name` isn't registered.
typedef void(WINAPI* Mad2Achievements_Unlock_t)(const char* name);

// Current unlocked state. Returns FALSE if `name` isn't registered.
typedef BOOL(WINAPI* Mad2Achievements_IsUnlocked_t)(const char* name);

#define MAD2ACHIEVEMENTS_REGISTER_PROC "Mad2Achievements_Register"
#define MAD2ACHIEVEMENTS_GETCOUNT_PROC "Mad2Achievements_GetCount"
#define MAD2ACHIEVEMENTS_GETINFO_PROC "Mad2Achievements_GetInfo"
#define MAD2ACHIEVEMENTS_UNLOCK_PROC "Mad2Achievements_Unlock"
#define MAD2ACHIEVEMENTS_ISUNLOCKED_PROC "Mad2Achievements_IsUnlocked"

struct Mad2AchievementsApi {
    Mad2Achievements_Register_t Register;
    Mad2Achievements_GetCount_t GetCount;
    Mad2Achievements_GetInfo_t GetInfo;
    Mad2Achievements_Unlock_t Unlock;
    Mad2Achievements_IsUnlocked_t IsUnlocked;
};

// Resolves mad2achievements.dll's exports by name. Cheap to call every
// frame (resolves once, then returns the cached result). If
// mad2achievements.dll isn't loaded, every member is null -- callers must
// null-check before use, same convention as every other shared-API mod.
inline const Mad2AchievementsApi& Mad2Achievements_Resolve() {
    static Mad2AchievementsApi api{};
    static bool resolved = false;
    if (!resolved) {
        if (HMODULE h = GetModuleHandleA(MAD2ACHIEVEMENTS_DLL_NAME)) {
            api.Register = reinterpret_cast<Mad2Achievements_Register_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2ACHIEVEMENTS_REGISTER_PROC)));
            api.GetCount = reinterpret_cast<Mad2Achievements_GetCount_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2ACHIEVEMENTS_GETCOUNT_PROC)));
            api.GetInfo = reinterpret_cast<Mad2Achievements_GetInfo_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2ACHIEVEMENTS_GETINFO_PROC)));
            api.Unlock = reinterpret_cast<Mad2Achievements_Unlock_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2ACHIEVEMENTS_UNLOCK_PROC)));
            api.IsUnlocked = reinterpret_cast<Mad2Achievements_IsUnlocked_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2ACHIEVEMENTS_ISUNLOCKED_PROC)));
            resolved = api.Register != nullptr;
        }
    }
    return api;
}
