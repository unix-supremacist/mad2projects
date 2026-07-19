#pragma once
// Shared current-level detection routine for Madagascar 2 mods that need to
// know "what level is the player in right now" without depending on a new
// DLL API. Used by mad2gameeffectsmod (PlayerTeleport/MangoMutate gating)
// and mad2cheatapply (title-screen gating) -- both previously carried a
// verbatim ~40-line copy of this exact logic (see improvement-plan.md item
// 12). This routine needs no runtime shared state -- each consumer keeps
// its own local g_CurrentLevel-style cache -- so it's a header-only,
// static inline drop-in rather than a third DLL/GetProcAddress dance.
//
// Primary source: mad2levelredirectmod's Mad2LevelRedirect_GetCurrentLevel,
// which tracks the actual level archive last opened under Content/Streams/
// win/ (see mad2levelredirectmod/src/levelredirect.cpp's file header).
// That's a strictly better signal than the ScriptInfoLib heuristic below
// even with no level redirection involved -- it has no gap where "current
// level" goes stale, and it reports the real CONTENT level rather than the
// requested slot, so it stays correct if mad2levelredirectmod is also
// redirecting level loads (../mad2rando5). Falls back to the ScriptInfoLib
// heuristic only if mad2levelredirectmod isn't installed.
//
// Consumers must #include, before this header:
//   - <windows.h>, <cstdint>, <string>
//   - mad2levelredirectmod/include/mad2levelredirect_api.h (for
//     Mad2LevelRedirect_Resolve/Mad2LevelRedirect_GetCurrentLevel)

// Local pointer-plausibility check -- deliberately independent of any
// per-mod IsPlausiblePointer() used for that mod's OWN unrelated pointer
// chains (player position, mango count, cheat struct, ...), so this header
// stays a fully self-contained drop-in and consumers don't need to
// reconcile two similarly-named helpers.
static inline bool Mad2LevelDetect_IsPlausiblePointer(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

// Fallback used only when mad2levelredirectmod isn't present. ScriptInfoLib
// .dll's _levelLoadRequest pointer (+0xA2708) only holds a valid StringInfo
// pointer while a level transition is actively in flight -- it resets to
// null/invalid once the load completes (per ../mad2mod's equivalent code
// path), so this misses "current level" entirely outside that narrow
// window, which is most of normal gameplay. Kept as a best-effort fallback
// rather than removed, so callers still do *something* without their
// sibling mod installed. Writes into currentLevel only when it finds a
// plausible new value; leaves it untouched otherwise (never resets to
// "None"/empty on a failed read).
static inline void Mad2LevelDetect_UpdateCurrentLevelFromScriptInfo(std::string& currentLevel) {
    HMODULE scriptInfoLib = GetModuleHandleA("ScriptInfoLib.dll");
    if (!scriptInfoLib) return;
    uintptr_t scriptBase = reinterpret_cast<uintptr_t>(scriptInfoLib);

    uintptr_t requestAddr = scriptBase + 0x000A2708;
    uint32_t pStringInfo = *reinterpret_cast<uint32_t*>(requestAddr);
    if (!Mad2LevelDetect_IsPlausiblePointer(pStringInfo)) return;  // no settled load-request pointer yet

    uint32_t vtable = *reinterpret_cast<uint32_t*>(pStringInfo);
    uintptr_t expectedVtable1 = scriptBase + 0x000377c0;
    uintptr_t expectedVtable2 = scriptBase + 0x00037748;  // second vtable ../mad2mod observed in practice
    if (vtable != expectedVtable1 && vtable != expectedVtable2) return;

    uint32_t pStrBuf = *reinterpret_cast<uint32_t*>(pStringInfo + 0x20);
    if (!Mad2LevelDetect_IsPlausiblePointer(pStrBuf)) {
        pStrBuf = *reinterpret_cast<uint32_t*>(pStringInfo + 0x24);
        if (!Mad2LevelDetect_IsPlausiblePointer(pStrBuf)) return;
    }

    const char16_t* utf16 = reinterpret_cast<const char16_t*>(pStrBuf);
    std::string levelName;
    for (int i = 0; i < 127 && utf16[i] != 0; ++i) levelName += static_cast<char>(utf16[i]);
    if (!levelName.empty()) currentLevel = levelName;
}

// Primary entry point -- call this (not the ScriptInfo fallback directly)
// wherever the old per-mod UpdateCurrentLevel() was called. Prefers
// mad2levelredirectmod's tracked level; falls through to the ScriptInfoLib
// heuristic if that mod isn't resolved, or is resolved but hasn't observed
// a level archive open yet (e.g. still on the title screen).
static inline void Mad2LevelDetect_UpdateCurrentLevel(std::string& currentLevel) {
    const auto& redirect = Mad2LevelRedirect_Resolve();
    if (redirect.GetCurrentLevel) {
        char buf[256];
        if (redirect.GetCurrentLevel(buf, sizeof(buf))) {
            currentLevel = buf;
            return;
        }
        // Resolved but hasn't observed a level archive open yet -- fall
        // through to the heuristic rather than leaving currentLevel stuck
        // at a previous call's value.
    }
    Mad2LevelDetect_UpdateCurrentLevelFromScriptInfo(currentLevel);
}
