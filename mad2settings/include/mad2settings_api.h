#pragma once
// Public client interface for mad2settings.dll (mad2/mods/mad2settings.dll).
//
// A generic "live setting" registry other mods register into so
// mad2debugmenu.dll's Nuklear UI can display and edit them, with edits
// taking effect immediately -- no polling, no cross-DLL callback
// plumbing to keep alive. Same non-opinionated "registry, not owner" role
// as mad2effects.dll (see mad2effects/include/mad2effects_api.h):
// mad2settings itself holds no game state and does not persist anything
// to config.cfg itself -- a registering mod already owns a live
// in-process global it reads every frame (its own Config struct, seeded
// from mad2config.dll at its own init time); this registry just publishes
// a raw pointer to that same global plus enough metadata for a generic UI
// to render it, write through it, and persist edits back to config.cfg.
//
// Why a raw pointer instead of get/set callbacks: every setting here is a
// plain scalar (BOOL/int/float/WORD) already read fresh every frame by its
// owning mod's own per-frame hook or background loop -- writing through
// the pointer from mad2debugmenu.dll takes effect on the very next read,
// same as this codebase's existing tolerance for un-mutexed scalar
// cross-thread reads elsewhere (e.g. every mod's own Config struct).
// Settings whose owning mod needs to actually DO something when the value
// changes (not just "read a fresher value next frame") -- e.g.
// mad2mirrormod's Enabled, which must engage/disengage an XInput
// override -- provide an `onChanged` callback, called right after the
// write.
//
// mad2/mods/*.dll load order is not guaranteed, so consumers don't link
// against mad2settings.dll at build time -- resolve lazily via
// Mad2Settings_Resolve(), same pattern as every other shared-API mod.
// Register once per setting, right after seeding the live global from
// mad2config.dll (same lazy init-thread/first-frame timing every other
// registration in this codebase uses).
#include <windows.h>

#define MAD2SETTINGS_DLL_NAME "mad2settings.dll"
#define MAD2SETTINGS_MAX_CATEGORY 64
#define MAD2SETTINGS_MAX_LABEL 96
#define MAD2SETTINGS_MAX_SECTION 64
#define MAD2SETTINGS_MAX_KEY 64

enum Mad2SettingType {
    MAD2SETTING_BOOL,          // valuePtr -> bool (C++ bool, 1 byte -- NOT Win32 BOOL/int;
                                // every registering mod's own Config struct uses plain bool
                                // for its toggles, and pointing at the wrong width corrupts
                                // adjacent struct fields)
    MAD2SETTING_INT,           // valuePtr -> int, slider [minValue,maxValue]
    MAD2SETTING_FLOAT,         // valuePtr -> float, slider [minValue,maxValue]
    MAD2SETTING_VKEY,          // valuePtr -> int, a virtual-key code (0 = unbound)
    MAD2SETTING_GAMEPAD_MASK,  // valuePtr -> WORD, XINPUT_GAMEPAD_* bitmask
    MAD2SETTING_ACTION,        // valuePtr must be NULL -- a one-shot button, see onChanged
};

// For ACTION settings, called on button press. For every other type,
// called immediately after mad2debugmenu.dll writes the new value through
// valuePtr. Optional (NULL) for settings a per-frame check already picks
// up on its own (e.g. a hotkey vkey, a plain flag re-read every frame).
typedef void(WINAPI* Mad2Settings_ActionFn)(void* userdata);

struct Mad2SettingDesc {
    const char* category;       // UI grouping, e.g. "Mirror Mode"
    const char* label;          // e.g. "Toggle Key"
    const char* configSection;  // for persisting edits via Mad2Config_SetString;
    const char* configKey;      // ignored for ACTION (nothing to persist)
    Mad2SettingType type;
    void* valuePtr;              // live value pointer; NULL for ACTION, required otherwise
    float minValue, maxValue;    // INT/FLOAT slider range; ignored otherwise
    Mad2Settings_ActionFn onChanged;  // optional, see file header
    void* userdata;                    // passed to onChanged
};

struct Mad2SettingInfo {
    char category[MAD2SETTINGS_MAX_CATEGORY];
    char label[MAD2SETTINGS_MAX_LABEL];
    char configSection[MAD2SETTINGS_MAX_SECTION];
    char configKey[MAD2SETTINGS_MAX_KEY];
    Mad2SettingType type;
    void* valuePtr;
    float minValue, maxValue;
    Mad2Settings_ActionFn onChanged;
    void* userdata;
};

// Registers a new setting. Always appends -- unlike mad2effects' Register,
// settings have no stable name to replace by, so call this exactly once
// per setting (typically from each mod's own lazy init path, never on a
// per-frame hook). Returns FALSE only on a hard failure (null desc, a
// non-ACTION type with a null valuePtr, or an ACTION type with a non-null
// valuePtr).
typedef BOOL(WINAPI* Mad2Settings_Register_t)(const Mad2SettingDesc* desc);

// Number of currently-registered settings, for iterating GetInfo(0..count).
typedef int(WINAPI* Mad2Settings_GetCount_t)(void);

// Snapshot of the setting at `index` (registration order). Returns FALSE
// if index is out of range.
typedef BOOL(WINAPI* Mad2Settings_GetInfo_t)(int index, Mad2SettingInfo* outInfo);

#define MAD2SETTINGS_REGISTER_PROC "Mad2Settings_Register"
#define MAD2SETTINGS_GETCOUNT_PROC "Mad2Settings_GetCount"
#define MAD2SETTINGS_GETINFO_PROC "Mad2Settings_GetInfo"

struct Mad2SettingsApi {
    Mad2Settings_Register_t Register;
    Mad2Settings_GetCount_t GetCount;
    Mad2Settings_GetInfo_t GetInfo;
};

// Resolves mad2settings.dll's exports by name. Cheap to call every time
// (resolves once, then returns the cached result). If mad2settings.dll
// isn't loaded (e.g. missing from mad2/mods/), every member is null --
// callers must null-check before use, same convention as every other
// shared-API mod in this repo.
inline const Mad2SettingsApi& Mad2Settings_Resolve() {
    static Mad2SettingsApi api{};
    static bool resolved = false;
    if (!resolved) {
        if (HMODULE h = GetModuleHandleA(MAD2SETTINGS_DLL_NAME)) {
            api.Register = reinterpret_cast<Mad2Settings_Register_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2SETTINGS_REGISTER_PROC)));
            api.GetCount = reinterpret_cast<Mad2Settings_GetCount_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2SETTINGS_GETCOUNT_PROC)));
            api.GetInfo = reinterpret_cast<Mad2Settings_GetInfo_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2SETTINGS_GETINFO_PROC)));
            resolved = api.Register != nullptr;
        }
    }
    return api;
}
