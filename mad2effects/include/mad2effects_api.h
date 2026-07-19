#pragma once
// Public client interface for mad2effects.dll (mad2/mods/mad2effects.dll).
//
// A generic "chaos effect" registry: any mod can register a named effect
// (an Apply callback, and optionally a Clear callback + a default
// auto-revert duration range), and any mod can enumerate the registry or
// trigger an effect by name. mad2effects owns no game state itself -- it
// is purely a switchboard + duration timer. mad2chaosmod drives it by
// picking a random registered effect, weighted by each effect's
// registered `defaultWeight` (with an optional per-effect override read
// from config.cfg); mad2twitchcontrolsmod drives it directly from a
// configured Twitch channel-points reward mapping. mad2inputeffectsmod
// and mad2graphicseffectmod are the effect *providers* -- see those for
// concrete Apply/Clear implementations.
//
// The `defaultWeight` field is intentionally uninterpreted by mad2effects
// itself: registration always succeeds regardless of its value, and
// Mad2Effects_Trigger doesn't consult it at all. It exists purely as
// metadata for whichever mod decides to use it (e.g. as a ticket-pool
// weight) -- "up to the calling mod to use if it wants."
//
// mad2/mods/*.dll load order is not guaranteed (see mad2modloader's
// mod_loader.cpp), so consumer mods don't link against mad2effects.dll at
// build time. Instead call Mad2Effects_Resolve() to look up its exports by
// name at runtime; do this lazily (e.g. the first time your per-frame
// hook fires, or from a background init thread if you have no per-frame
// hook -- see mad2inputeffectsmod/mad2chaosmod for that pattern) rather
// than from DllMain, since mad2effects.dll is guaranteed to have finished
// loading by then even if it loaded after you.
//
// Known v1 limitation: mad2effects has no concept of which piece of game
// state an effect "owns." If two registered effects both drive the same
// underlying resource (e.g. two XInput-override-based effects on the same
// controller, which mad2xinput only has a single override slot for), the
// second Apply silently clobbers the first's state. Not exercised by the
// effects registered so far (only one XInput-override effect exists) --
// worth keeping in mind for future effect authors.
#include <windows.h>

#define MAD2EFFECTS_DLL_NAME "mad2effects.dll"
#define MAD2EFFECTS_MAX_NAME 64
#define MAD2EFFECTS_MAX_DISPLAYNAME 128

// Called synchronously on whatever thread calls Mad2Effects_Trigger (may
// be a chaos-mode/twitch-controls background thread, NOT necessarily the
// D3D9 render thread) -- implementations must be safe to invoke off the
// render thread and must not block for long (mad2effects' internal ticker
// thread also calls Clear from its own thread on auto-expiry).
typedef void(WINAPI* Mad2Effects_ApplyFn)(void* userdata);
typedef void(WINAPI* Mad2Effects_ClearFn)(void* userdata);

struct Mad2EffectDesc {
    const char* name;         // Stable unique id, e.g. "JoystickReversal". Matched
                               // case-insensitively by Trigger/ClearByName. Copied
                               // internally -- the pointer need not outlive this call.
    const char* displayName;  // Human-readable, e.g. "Reversed Controls". Copied too.
    int defaultWeight;        // Chance metadata ONLY -- see file header. Consumers like
                               // mad2chaosmod read this via Mad2Effects_GetInfo.
    Mad2Effects_ApplyFn apply;  // Required.
    void* applyUserdata;
    Mad2Effects_ClearFn clear;  // NULL => instantaneous/fire-and-forget: Apply is called
                                 // once, Clear is never called, duration fields below are
                                 // ignored.
    void* clearUserdata;
    DWORD defaultMinDurationMs;  // Ignored if clear == NULL. If both are 0 (and clear !=
    DWORD defaultMaxDurationMs;  // NULL), the effect has no auto-revert timer -- it stays
                                  // active until Mad2Effects_ClearByName/ClearAll, or a
                                  // Trigger call with a nonzero durationMsOverride.
};

struct Mad2EffectInfo {
    char name[MAD2EFFECTS_MAX_NAME];
    char displayName[MAD2EFFECTS_MAX_DISPLAYNAME];
    int weight;
    BOOL hasDuration;  // TRUE if a Clear callback was registered.
    BOOL isActive;      // Currently applied, not yet cleared.
};

// Registers (or, if `name` already exists, replaces in place) an effect.
// Always succeeds (returns FALSE only on a hard failure, e.g. null desc/
// name/apply) regardless of defaultWeight's value.
typedef BOOL(WINAPI* Mad2Effects_Register_t)(const Mad2EffectDesc* desc);

// Number of currently-registered effects, for iterating GetInfo(0..count).
typedef int(WINAPI* Mad2Effects_GetCount_t)(void);

// Snapshot of the effect at `index` (registration order). Returns FALSE if
// index is out of range.
typedef BOOL(WINAPI* Mad2Effects_GetInfo_t)(int index, Mad2EffectInfo* outInfo);

// Triggers the named effect: calls its Apply callback, and if it has a
// Clear callback, schedules an automatic Clear after `durationMsOverride`
// milliseconds -- or, if durationMsOverride is 0, after a duration
// randomized between the effect's own registered
// [defaultMinDurationMs, defaultMaxDurationMs] (or never, if both are 0).
// Re-triggering an already-active effect just re-applies and resets its
// clear timer. Returns FALSE if `name` isn't registered.
typedef BOOL(WINAPI* Mad2Effects_Trigger_t)(const char* name, DWORD durationMsOverride);

// Immediately calls the named effect's Clear callback (if any) and marks
// it inactive, regardless of any pending auto-revert timer. No-op if the
// effect isn't registered, isn't active, or has no Clear callback.
typedef void(WINAPI* Mad2Effects_ClearByName_t)(const char* name);

// Clears every currently-active effect that has a Clear callback.
typedef void(WINAPI* Mad2Effects_ClearAll_t)(void);

// Updates only the display name of an already-registered effect, without
// touching its active/duration state -- unlike calling Register again
// (which resets active state, since it's meant for initial setup, not a
// live update). For effects that randomize a parameter each trigger (e.g.
// StickDirectionDisable's stick+direction choice), call this from the
// Apply callback so Mad2Effects_GetInfo/the effects HUD show which
// specific variant is currently active instead of just the effect's
// generic name. No-op if the effect isn't registered.
typedef void(WINAPI* Mad2Effects_SetDisplayName_t)(const char* name, const char* displayName);

#define MAD2EFFECTS_REGISTER_PROC "Mad2Effects_Register"
#define MAD2EFFECTS_GETCOUNT_PROC "Mad2Effects_GetCount"
#define MAD2EFFECTS_GETINFO_PROC "Mad2Effects_GetInfo"
#define MAD2EFFECTS_TRIGGER_PROC "Mad2Effects_Trigger"
#define MAD2EFFECTS_CLEARBYNAME_PROC "Mad2Effects_ClearByName"
#define MAD2EFFECTS_CLEARALL_PROC "Mad2Effects_ClearAll"
#define MAD2EFFECTS_SETDISPLAYNAME_PROC "Mad2Effects_SetDisplayName"

struct Mad2EffectsApi {
    Mad2Effects_Register_t Register;
    Mad2Effects_GetCount_t GetCount;
    Mad2Effects_GetInfo_t GetInfo;
    Mad2Effects_Trigger_t Trigger;
    Mad2Effects_ClearByName_t ClearByName;
    Mad2Effects_ClearAll_t ClearAll;
    Mad2Effects_SetDisplayName_t SetDisplayName;
};

// Resolves mad2effects.dll's exports by name. Cheap to call every frame
// (resolves once, then returns the cached result). If mad2effects.dll
// isn't loaded (e.g. missing from mad2/mods/), every member is null --
// callers must null-check before use (see other mods' "[Dependency]" log
// lines for the convention).
inline const Mad2EffectsApi& Mad2Effects_Resolve() {
    static Mad2EffectsApi api{};
    static bool resolved = false;
    if (!resolved) {
        if (HMODULE h = GetModuleHandleA(MAD2EFFECTS_DLL_NAME)) {
            api.Register = reinterpret_cast<Mad2Effects_Register_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2EFFECTS_REGISTER_PROC)));
            api.GetCount = reinterpret_cast<Mad2Effects_GetCount_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2EFFECTS_GETCOUNT_PROC)));
            api.GetInfo = reinterpret_cast<Mad2Effects_GetInfo_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2EFFECTS_GETINFO_PROC)));
            api.Trigger = reinterpret_cast<Mad2Effects_Trigger_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2EFFECTS_TRIGGER_PROC)));
            api.ClearByName = reinterpret_cast<Mad2Effects_ClearByName_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2EFFECTS_CLEARBYNAME_PROC)));
            api.ClearAll = reinterpret_cast<Mad2Effects_ClearAll_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2EFFECTS_CLEARALL_PROC)));
            api.SetDisplayName = reinterpret_cast<Mad2Effects_SetDisplayName_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2EFFECTS_SETDISPLAYNAME_PROC)));
            resolved = api.Register != nullptr;
        }
    }
    return api;
}
