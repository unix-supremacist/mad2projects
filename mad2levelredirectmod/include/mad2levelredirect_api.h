#pragma once
// Public client interface for mad2levelredirectmod.dll
// (mad2/mods/mad2levelredirectmod.dll). Owns two things:
//
//  1. Level redirection: IAT-hooks CreateFileA/W so that opening a level
//     archive (Content/Streams/win/<Slot>.arc or .bld) for a slot name
//     present in level_redirects.txt transparently opens the mapped
//     level's archive instead -- see src/levelredirect.cpp's file header
//     for the mapping file format and where it comes from (../mad2rando5).
//  2. Current-level tracking: every level archive open (redirected or
//     not) updates a "current level" value to the actual CONTENT that
//     was opened -- not the slot the game asked for. Any mod that needs
//     to know "what level is the player actually standing in" should
//     read this instead of inferring it from game memory: it's simpler,
//     more reliable (see src/levelredirect.cpp's file header for why the
//     obvious in-memory approach is flaky), and automatically correct
//     under level redirection since it observes the real file that got
//     opened, after any redirect. mad2gameeffectsmod's PlayerTeleport/
//     MangoMutate are the first consumers -- see gameeffects.cpp.
//
// mad2/mods/*.dll load order is not guaranteed, so consumers don't link
// against this DLL at build time -- resolve lazily via
// Mad2LevelRedirect_Resolve(), same pattern as every other shared-API mod
// (mad2xinput/mad2config/mad2effects).
#include <windows.h>

#define MAD2LEVELREDIRECT_DLL_NAME "mad2levelredirectmod.dll"

// Fills outBuf with the most recently observed actual content level (the
// real archive base name last opened under Content/Streams/win/, after any
// redirect, excluding "global"), NUL-terminated (truncated if it doesn't
// fit). Returns FALSE (outBuf untouched) if no level archive has been
// observed yet (e.g. still on the title screen, or this mod just loaded).
typedef BOOL(WINAPI* Mad2LevelRedirect_GetCurrentLevel_t)(char* outBuf, DWORD outBufSize);

#define MAD2LEVELREDIRECT_GETCURRENTLEVEL_PROC "Mad2LevelRedirect_GetCurrentLevel"

struct Mad2LevelRedirectApi {
    Mad2LevelRedirect_GetCurrentLevel_t GetCurrentLevel;
};

// Resolves mad2levelredirectmod.dll's exports by name. Cheap to call every
// frame (resolves once, then returns the cached result). If the DLL isn't
// loaded (e.g. missing from mad2/mods/ -- this mod is optional), every
// member is null -- callers must null-check and fall back to their own
// detection or a "no level known" default.
inline const Mad2LevelRedirectApi& Mad2LevelRedirect_Resolve() {
    static Mad2LevelRedirectApi api{};
    static bool resolved = false;
    if (!resolved) {
        if (HMODULE h = GetModuleHandleA(MAD2LEVELREDIRECT_DLL_NAME)) {
            api.GetCurrentLevel = reinterpret_cast<Mad2LevelRedirect_GetCurrentLevel_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2LEVELREDIRECT_GETCURRENTLEVEL_PROC)));
            resolved = api.GetCurrentLevel != nullptr;
        }
    }
    return api;
}
