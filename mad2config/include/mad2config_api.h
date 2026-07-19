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

#define MAD2CONFIG_GETSTRING_PROC "Mad2Config_GetString"
#define MAD2CONFIG_SETSTRING_PROC "Mad2Config_SetString"
#define MAD2CONFIG_GETINT_PROC "Mad2Config_GetInt"
#define MAD2CONFIG_GETFLOAT_PROC "Mad2Config_GetFloat"
#define MAD2CONFIG_GETBOOL_PROC "Mad2Config_GetBool"
#define MAD2CONFIG_GETVIRTUALKEY_PROC "Mad2Config_GetVirtualKey"
#define MAD2CONFIG_GETGAMEPADBUTTONMASK_PROC "Mad2Config_GetGamepadButtonMask"

struct Mad2ConfigApi {
    Mad2Config_GetString_t GetString;
    Mad2Config_SetString_t SetString;
    Mad2Config_GetInt_t GetInt;
    Mad2Config_GetFloat_t GetFloat;
    Mad2Config_GetBool_t GetBool;
    Mad2Config_GetVirtualKey_t GetVirtualKey;
    Mad2Config_GetGamepadButtonMask_t GetGamepadButtonMask;
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
            resolved = api.GetString != nullptr;
        }
    }
    return api;
}
