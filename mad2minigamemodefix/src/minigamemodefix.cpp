// Minigame-mode fix (mad2/mods/mad2minigamemodefix.dll).
//
// Background: the game's own front-end tracks a persistent, live
// ScriptInfoLib.dll variable named "Minigame Launch Mode" (a plain
// OpCreateVariable-family object -- found via mad2climbprobe's
// live-object-naming scan, itself built on a documented lead in
// docs/CLASS_SCHEMA.md's "Naming investigation" section: live, runtime-
// spawned objects' igNamedObject name field resolves to a real readable
// string, unlike the same field on static file objects). It's set true by
// selecting a minigame slot in the main menu, or loading a minigame-only
// level, and per the game's own logic is only ever cleared once another
// non-minigame level loads -- but in practice, ANY level that merely
// *references* code shared with a minigame (Dam_Busters/IslandFever
// borrowing Soccer's kicking code, FixThePlane borrowing Card_Match_Game's,
// WooingGloria borrowing HungryHippo's, RitesOfPassage borrowing
// RoP_MusicalChairs'/Minigame_HotDurian's) spawns the minigame start/quit
// front-end regardless of whether it's actually a minigame, making those
// story levels incompletable whenever minigame mode is still active from
// an earlier minigame. Confirmed via a live memory diff (title screen vs.
// an active minigame): every byte of the "Minigame Launch Mode" object is
// identical between the two states except a 4-byte field at +0x24
// (0 = off, 1 = on).
//
// The fix: force that field to 0 on every level EXCEPT a small whitelist
// of real minigames hand-confirmed (by playing each one with it forced
// off) to actually need it on -- everything else blackscreens without it,
// everything not listed either doesn't care or is actively better off
// (DrMelmen/VolcanoRave -- the two levels that are legitimately both a
// story level and a minigame depending on this exact flag -- always
// behave as the story version; several minigames like Card_Match_Game,
// golf, and animal chess incidentally gain their story-mode goal display,
// like on Wii, instead of the minigame-only HUD).
//
// No D3D9 hook needed -- same "init-only, then a persistent background
// thread" shape as mad2mirrormod's toggle loop, just with no hotkey and a
// per-tick level check instead of a one-time apply.
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2levelredirectmod/include/mad2levelredirect_api.h"
#include "../../mad2leveldetect/mad2leveldetect.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2minigamemodefix]", fmt, args);
    va_end(args);
}

static bool IsPlausiblePointer(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

static bool IsAddressSafeToRead(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
    return true;
}

// [start, end) of one committed, readable region. Same shape as
// mad2climbprobe's own scan -- see that mod's src/climbprobe.cpp for the
// full reasoning (a naive per-candidate-VirtualQuery version made a
// syscall for nearly every 4-byte word in the process's writable memory
// and crashed the game outright; this builds the range table once and
// binary-searches it in memory instead).
using AddrRange = std::pair<uintptr_t, uintptr_t>;

static std::vector<AddrRange> EnumerateReadableRanges() {
    std::vector<AddrRange> ranges;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    const uintptr_t kMaxAddr = 0x7FFF0000;
    const SIZE_T kMaxRegionSize = 600ull * 1024 * 1024;
    while (addr < kMaxAddr) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.RegionSize == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
            mbi.RegionSize < kMaxRegionSize) {
            uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            ranges.emplace_back(start, start + mbi.RegionSize);
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return ranges;
}

static uintptr_t FindContainingRangeEnd(const std::vector<AddrRange>& ranges, uintptr_t addr, size_t minLen) {
    size_t lo = 0, hi = ranges.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ranges[mid].second <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= ranges.size()) return 0;
    const AddrRange& r = ranges[lo];
    if (addr < r.first || addr + minLen > r.second) return 0;
    return r.second;
}

static bool LooksLikeName(const std::vector<AddrRange>& ranges, uintptr_t addr, size_t maxLen) {
    uintptr_t rangeEnd = FindContainingRangeEnd(ranges, addr, 1);
    if (rangeEnd == 0) return false;
    size_t avail = static_cast<size_t>(rangeEnd - addr);
    size_t limit = (avail < maxLen) ? avail : maxLen;
    const char* s = reinterpret_cast<const char*>(addr);
    size_t len = 0;
    while (len < limit) {
        char c = s[len];
        if (c == '\0') break;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
                  c == '_' || c == '-';
        if (!ok) return false;
        ++len;
    }
    return len >= 2 && len < limit;
}

// Finds the live "Minigame Launch Mode" object by exact name match (the
// igNamedObject convention -- *(this+8) as a plain char*). Returns its
// address, or 0 if it isn't currently spawned/found.
static uintptr_t FindMinigameLaunchModeObject() {
    std::vector<AddrRange> ranges = EnumerateReadableRanges();
    for (const AddrRange& range : ranges) {
        uintptr_t start = range.first;
        uintptr_t end = range.second;
        if (end < start + 12) continue;
        uintptr_t lastObj = end - 12;
        for (uintptr_t objAddr = start; objAddr <= lastObj; objAddr += 4) {
            uint32_t namePtr = *reinterpret_cast<const uint32_t*>(objAddr + 8);
            if (!IsPlausiblePointer(namePtr)) continue;
            if (!LooksLikeName(ranges, namePtr, 64)) continue;
            const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(namePtr));
            if (_stricmp(name, "Minigame Launch Mode") == 0) return objAddr;
        }
    }
    return 0;
}

static const uintptr_t kMinigameLaunchModeValueOffset = 0x24;

// Levels confirmed (by hand, playing each one with the flag forced off) to
// actually need "Minigame Launch Mode" active -- everything else either
// doesn't care or is better off with it forced off. Matched
// case-insensitively as a substring, not an exact archive name, so this
// still works under level redirection (../mad2rando5) or if the exact
// archive naming differs slightly from what's listed here. "DivingLocation"
// deliberately does NOT match "Minigame_Diving_Location_Menu" (which has
// underscores between the words and works fine either way) -- only the
// individual DivingLocation_<Name> play levels, which do need it.
static bool LevelAllowsMinigameMode(const std::string& level) {
    static const char* kWhitelist[] = {
        "HotDurian", "MusicalChairs", "Soccer", "HungryHippo", "DivingLocation",
    };
    for (const char* w : kWhitelist) {
        size_t wlen = strlen(w);
        if (level.size() < wlen) continue;
        for (size_t i = 0; i + wlen <= level.size(); ++i) {
            if (_strnicmp(level.c_str() + i, w, wlen) == 0) return true;
        }
    }
    return false;
}

struct Config {
    bool enabled = true;
};
static Config g_Config;

static void LoadConfig() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetBool) {
        Log("[Dependency] mad2config not available -- using built-in default (enabled)\n");
        return;
    }
    g_Config.enabled =
        api.GetBool("MinigameModeFix", "Enabled", TRUE,
                    "Forces the game's own \"Minigame Launch Mode\" flag off on every level except a small\n"
                    "whitelist of real minigames confirmed to need it (Minigame_HotDurian, RoP_MusicalChairs,\n"
                    "Soccer, HungryHippo, and the DivingLocation_* levels -- all blackscreen without it).\n"
                    "Fixes DrMelmen/VolcanoRave (and other story levels that merely reference shared\n"
                    "minigame code) always acting like a minigame even when entered normally, and as a side\n"
                    "effect gives several other minigames their story-mode goal display instead of the\n"
                    "minigame-only HUD.") != FALSE;
}

static uintptr_t g_MinigameModeObjAddr = 0;
static ULONGLONG g_NextRescanTick = 0;

static void Tick(const std::string& currentLevel) {
    if (LevelAllowsMinigameMode(currentLevel)) {
        // Leave the flag alone entirely on a whitelisted level -- drop any
        // cached address so the next non-whitelisted level re-finds it
        // fresh rather than assuming it's still at the same spot.
        g_MinigameModeObjAddr = 0;
        return;
    }

    if (g_MinigameModeObjAddr != 0) {
        uintptr_t valueAddr = g_MinigameModeObjAddr + kMinigameLaunchModeValueOffset;
        if (IsAddressSafeToRead(valueAddr)) {
            *reinterpret_cast<uint32_t*>(valueAddr) = 0;
            return;
        }
        g_MinigameModeObjAddr = 0;  // stale, fall through to re-scan below
    }

    ULONGLONG now = GetTickCount64();
    if (now < g_NextRescanTick) return;
    g_NextRescanTick = now + 2000;  // the scan itself isn't free, don't retry every tick
    g_MinigameModeObjAddr = FindMinigameLaunchModeObject();
}

static DWORD WINAPI MainThreadFunc(LPVOID) {
    for (int i = 0; i < 100; ++i) {
        if (Mad2Config_Resolve().GetBool) break;
        Sleep(50);
    }
    LoadConfig();
    if (!g_Config.enabled) {
        Log("Disabled via config -- exiting, no forcing will happen this session\n");
        return 0;
    }
    Log("Enabled -- forcing \"Minigame Launch Mode\" off except on whitelisted levels\n");

    std::string currentLevel = "None";
    std::string prevLevel;
    for (;;) {
        Mad2LevelDetect_UpdateCurrentLevel(currentLevel);
        if (currentLevel != prevLevel) {
            bool allowed = LevelAllowsMinigameMode(currentLevel);
            Log("Level changed to '%s' -- minigame mode %s\n", currentLevel.c_str(),
                allowed ? "ALLOWED (not forcing)" : "forced OFF");
            prevLevel = currentLevel;
        }
        // "None" is mad2leveldetect's own pre-boot default -- this DLL's
        // DllMain (and therefore this thread) runs extremely early, before
        // Mad2.exe's real entry point does (version.dll loads before AWL.dll
        // finishes its own imports -- see CLAUDE.md's "Load chain" section),
        // so the process's memory layout is still churning heavily (DLLs
        // still being loaded/relocated, big initial allocations in flight)
        // at this point. EnumerateReadableRanges()/FindMinigameLaunchModeObject()
        // were only ever exercised interactively, well into a stable running
        // session (mad2climbprobe's 'K' toggle) -- doing that same scan this
        // early was a real, live-tested mistake: it reliably crashed the
        // game on load. Skip all scanning/writing until the game has
        // actually reached a real level (title screen or beyond), the same
        // point every manual test this fix is based on was performed at.
        if (currentLevel != "None") {
            Tick(currentLevel);
        }
        Sleep(50);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            HANDLE h = CreateThread(nullptr, 0, MainThreadFunc, nullptr, 0, nullptr);
            if (h) {
                CloseHandle(h);
            } else {
                Log("CreateThread failed: %lu\n", GetLastError());
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
