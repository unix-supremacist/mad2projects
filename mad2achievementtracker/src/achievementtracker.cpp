// Achievement detection driver (mad2/mods/mad2achievementtracker.dll) --
// mirrors mad2chaosmod's relationship to mad2effects.dll: mad2achievements
// is a pure registry with no game-specific logic, and this mod is one of
// (potentially several, over time) detectors that watch real game state and
// call Mad2Achievements_Unlock when a condition is met. First (and so far
// only) achievement: "BeatTheGame", unlocked the first time the tracked
// current level transitions from Dam_Busters to Credits -- the game's own
// dedicated end-credits level, per mad2rando5/levels.go's Transitions list.
//
// Uses mad2leveldetect.h (shared with mad2gameeffectsmod/mad2cheatapply)
// rather than talking to mad2levelredirectmod directly -- same "actual
// content level last opened, not the requested slot" signal, so this stays
// correct under ../mad2rando5's level redirection for free (redirecting
// Dam_Busters' content into a different slot still reports "Dam_Busters"
// here, since that's what actually got opened), with the same
// ScriptInfoLib fallback every other consumer of that header gets.
//
// No D3D9 hook -- a single background thread, same shape as
// mad2gameeffectsmod/mad2minigamemodefix's init/poll threads: wait for
// mad2achievements.dll to be resolvable, register this mod's achievement(s),
// then poll the current level on a plain timer and react to transitions.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#include "../../mad2levelredirectmod/include/mad2levelredirect_api.h"
#include "../../mad2achievements/include/mad2achievements_api.h"
#include "../../mad2leveldetect/mad2leveldetect.h"

#include "../../mad2sharedlog/mad2sharedlog.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2achievementtracker]", fmt, args);
    va_end(args);
}

static void RegisterAchievements() {
    Mad2AchievementDesc desc{};
    desc.name = "BeatTheGame";
    desc.displayName = "Beat The Game";
    desc.description = "Reach the Credits level after Dam Busters.";
    BOOL ok = Mad2Achievements_Resolve().Register(&desc);
    Log("Mad2Achievements_Register(\"BeatTheGame\") -> %d\n", ok);
}

// Case-insensitive on purpose, matching this codebase's existing level-name
// comparison convention elsewhere (e.g. mad2minigamemodefix's whitelist
// match) even though the archive names observed in practice are always
// exactly "Dam_Busters"/"Credits".
static bool LevelNameEquals(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }

static DWORD WINAPI TrackerThreadFunc(LPVOID) {
    const int kRetryDelayMs = 200;
    const int kWarnAfterAttempts = 25;  // ~5s

    int attempts = 0;
    bool warned = false;
    for (;;) {
        if (Mad2Achievements_Resolve().Unlock != nullptr) break;
        ++attempts;
        if (!warned && attempts >= kWarnAfterAttempts) {
            warned = true;
            Log("[Dependency] still waiting on mad2achievements.dll (retrying indefinitely)\n");
        }
        Sleep(kRetryDelayMs);
    }

    RegisterAchievements();

    std::string currentLevel = "None";
    std::string previousLevel = "None";
    for (;;) {
        Sleep(500);
        Mad2LevelDetect_UpdateCurrentLevel(currentLevel);
        if (currentLevel != previousLevel) {
            if (LevelNameEquals(previousLevel, "Dam_Busters") && LevelNameEquals(currentLevel, "Credits")) {
                Log("Detected Dam_Busters -> Credits transition -- unlocking BeatTheGame\n");
                Mad2Achievements_Resolve().Unlock("BeatTheGame");
            }
            previousLevel = currentLevel;
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            HANDLE h = CreateThread(nullptr, 0, TrackerThreadFunc, nullptr, 0, nullptr);
            if (h) {
                CloseHandle(h);
            } else {
                Log("CreateThread for tracker thread failed: %lu\n", GetLastError());
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
