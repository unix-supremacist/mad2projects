#pragma once
// Public client interface for mad2config.dll (mad2/mods/mad2config.dll).
//
// mad2config owns a single human-editable config file (<exe dir>\config.cfg)
// shared by every mod that has user-facing settings, resolved the same way
// mad2xinput's shared controller state is -- see
// mad2xinput/include/mad2xinput_api.h for the sibling pattern this mirrors.
//
// mad2/mods/*.dll load order is not guaranteed (see mad2modloader's
// mod_loader.cpp), so consumer mods don't link against mad2config.dll at
// build time. Instead call Mad2Config_Resolve() to look up its exports by
// name at runtime; do this lazily (e.g. the first time your per-frame hook
// or file hook fires) rather than from DllMain, since mad2config.dll is
// guaranteed to have finished loading by then even if it loaded after you.
//
// Every Get* is a "read or seed default" call: if (section, key) isn't in
// config.cfg yet, defaultValue is written back immediately, with `comment`
// (may contain embedded '\n' for multiple lines) rendered as '#'-prefixed
// lines directly above it -- so the file is self-documenting on first run.
// Pass a comment that names the valid values and, where useful, a link to
// the authoritative spec (e.g. virtual-key codes or XINPUT_GAMEPAD button
// names) so a user hand-editing the file knows what's legal to put there.
#include <windows.h>

#define MAD2CONFIG_DLL_NAME "mad2config.dll"

// section/key/defaultValue/comment are UTF-8/ASCII, NUL-terminated, owned by
// the caller for the duration of the call only. outBuf receives a
// NUL-terminated value, truncated (still NUL-terminated) if it doesn't fit
// in outBufSize. Returns FALSE only on a hard failure (e.g. null outBuf).
typedef BOOL(WINAPI* Mad2Config_GetString_t)(const char* section, const char* key, const char* defaultValue,
                                              const char* comment, char* outBuf, DWORD outBufSize);

// Updates (section, key)'s value in place, or appends it (with no comment)
// if it doesn't exist yet. For persisting values the game itself changes
// (e.g. a language selector) back into config.cfg.
typedef void(WINAPI* Mad2Config_SetString_t)(const char* section, const char* key, const char* value);

typedef int(WINAPI* Mad2Config_GetInt_t)(const char* section, const char* key, int defaultValue,
                                          const char* comment);
typedef float(WINAPI* Mad2Config_GetFloat_t)(const char* section, const char* key, float defaultValue,
                                              const char* comment);
typedef BOOL(WINAPI* Mad2Config_GetBool_t)(const char* section, const char* key, BOOL defaultValue,
                                            const char* comment);

// Accepts either a 0xNN hex literal or a bare VK_ name with the VK_ prefix
// stripped (A-Z, 0-9, F1-F24, SPACE/ENTER/ESCAPE/TAB/BACKSPACE). Falls back
// to defaultVk if the stored value doesn't parse as either.
typedef int(WINAPI* Mad2Config_GetVirtualKey_t)(const char* section, const char* key, int defaultVk,
                                                 const char* comment);

// Accepts a comma-separated list of XINPUT_GAMEPAD_* names with the
// XINPUT_GAMEPAD_ prefix stripped (e.g. "START,BACK"); returns the OR of
// the matched bits. Unrecognized names are skipped.
typedef WORD(WINAPI* Mad2Config_GetGamepadButtonMask_t)(const char* section, const char* key,
                                                          const char* defaultCombo, const char* comment);

// Flat enumeration of every "Key=Value" line in config.cfg, across every
// section -- what lets a generic UI (mad2debugmenu.dll's "Raw Config" tab)
// show/edit every setting that exists, not just the ones a mod has
// explicitly registered with mad2settings.dll. Mirrors mad2launcher's own
// iniconfig.go parser (a separate, independent implementation of the same
// file format, since that one runs in a different process) -- comment is
// the raw '\n'-joined text seeded above the key, directive lines (e.g. a
// "@select(...)"/"@indexed(...)" line -- see iniconfig.go's own directives)
// included verbatim; interpreting those is the UI layer's job, same
// separation of concerns as the Go side's iniconfig.go/ui_category.go.
#define MAD2CONFIG_MAX_SECTION 64
#define MAD2CONFIG_MAX_KEY 64
#define MAD2CONFIG_MAX_VALUE 256
#define MAD2CONFIG_MAX_COMMENT 1024
#define MAD2CONFIG_MAX_TYPEHINT 16

// typeHint values -- "" (unknown/plain string), "vkey", or "gamepadmask".
// Recorded the first time a key is actually read via GetVirtualKey/
// GetGamepadButtonMask this session (whichever mod owns that key always
// calls the matching typed getter to seed/read it, so by the time a UI
// enumerates entries -- well after every mod's own init has run -- the
// hint is already populated). Lets a generic editor render the same
// press-a-key rebind capture / per-button checkboxes typed settings get,
// instead of a raw hex/name text field, without the UI needing to know
// which specific mod owns which key.
#define MAD2CONFIG_TYPEHINT_VKEY "vkey"
#define MAD2CONFIG_TYPEHINT_GAMEPADMASK "gamepadmask"

struct Mad2ConfigEntry {
    char section[MAD2CONFIG_MAX_SECTION];
    char key[MAD2CONFIG_MAX_KEY];
    char value[MAD2CONFIG_MAX_VALUE];
    char comment[MAD2CONFIG_MAX_COMMENT];
    char typeHint[MAD2CONFIG_MAX_TYPEHINT];
};

// Re-reads and re-parses config.cfg from disk, caches the result, and
// returns the entry count -- call this ONCE per "refresh" (e.g. once per
// frame while a raw-config UI tab is visible), then index the same cached
// parse via GetEntryInfo(0..count); calling GetEntryInfo alone does NOT
// re-parse, so a whole-file read doesn't happen once per entry per frame.
typedef int(WINAPI* Mad2Config_GetEntryCount_t)(void);

// Snapshot of the entry at `index` (file order) from the cache GetEntryCount
// last built. Returns FALSE if index is out of range or GetEntryCount was
// never called.
typedef BOOL(WINAPI* Mad2Config_GetEntryInfo_t)(int index, Mad2ConfigEntry* outEntry);

#define MAD2CONFIG_GETSTRING_PROC "Mad2Config_GetString"
#define MAD2CONFIG_SETSTRING_PROC "Mad2Config_SetString"
#define MAD2CONFIG_GETINT_PROC "Mad2Config_GetInt"
#define MAD2CONFIG_GETFLOAT_PROC "Mad2Config_GetFloat"
#define MAD2CONFIG_GETBOOL_PROC "Mad2Config_GetBool"
#define MAD2CONFIG_GETVIRTUALKEY_PROC "Mad2Config_GetVirtualKey"
#define MAD2CONFIG_GETGAMEPADBUTTONMASK_PROC "Mad2Config_GetGamepadButtonMask"
#define MAD2CONFIG_GETENTRYCOUNT_PROC "Mad2Config_GetEntryCount"
#define MAD2CONFIG_GETENTRYINFO_PROC "Mad2Config_GetEntryInfo"

struct Mad2ConfigApi {
    Mad2Config_GetString_t GetString;
    Mad2Config_SetString_t SetString;
    Mad2Config_GetInt_t GetInt;
    Mad2Config_GetFloat_t GetFloat;
    Mad2Config_GetBool_t GetBool;
    Mad2Config_GetVirtualKey_t GetVirtualKey;
    Mad2Config_GetGamepadButtonMask_t GetGamepadButtonMask;
    Mad2Config_GetEntryCount_t GetEntryCount;
    Mad2Config_GetEntryInfo_t GetEntryInfo;
};

// Resolves mad2config.dll's exports by name. Cheap to call every frame
// (resolves once, then returns the cached result). If mad2config.dll isn't
// loaded (e.g. missing from mad2/mods/), every member is null -- callers
// must null-check before use and fall back to their own hardcoded default,
// logging that the dependency is missing (see other mods' "[Dependency]"
// log lines for the convention).
inline const Mad2ConfigApi& Mad2Config_Resolve() {
    static Mad2ConfigApi api{};
    static bool resolved = false;
    if (!resolved) {
        if (HMODULE h = GetModuleHandleA(MAD2CONFIG_DLL_NAME)) {
            api.GetString = reinterpret_cast<Mad2Config_GetString_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2CONFIG_GETSTRING_PROC)));
            api.SetString = reinterpret_cast<Mad2Config_SetString_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2CONFIG_SETSTRING_PROC)));
            api.GetInt = reinterpret_cast<Mad2Config_GetInt_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2CONFIG_GETINT_PROC)));
            api.GetFloat = reinterpret_cast<Mad2Config_GetFloat_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2CONFIG_GETFLOAT_PROC)));
            api.GetBool = reinterpret_cast<Mad2Config_GetBool_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2CONFIG_GETBOOL_PROC)));
            api.GetVirtualKey = reinterpret_cast<Mad2Config_GetVirtualKey_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2CONFIG_GETVIRTUALKEY_PROC)));
            api.GetGamepadButtonMask = reinterpret_cast<Mad2Config_GetGamepadButtonMask_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2CONFIG_GETGAMEPADBUTTONMASK_PROC)));
            api.GetEntryCount = reinterpret_cast<Mad2Config_GetEntryCount_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2CONFIG_GETENTRYCOUNT_PROC)));
            api.GetEntryInfo = reinterpret_cast<Mad2Config_GetEntryInfo_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2CONFIG_GETENTRYINFO_PROC)));
            resolved = api.GetString != nullptr;
        }
    }
    return api;
}
