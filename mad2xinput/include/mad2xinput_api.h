#pragma once
// Public client interface for mad2xinput.dll (mad2/mods/mad2xinput.dll).
//
// mad2xinput hooks XInputGetState at the IAT level (same technique as
// mad2assetloader/mad2saveloader/mad2shadowfix/mad2igttimer -- see those
// for the pattern) so it observes every poll the game makes, caches the
// raw per-player state, and lets other mods loaded into the same process
// read it -- or inject overrides into what the game itself receives --
// without each mod having to install its own XInput hook.
//
// mad2/mods/*.dll load order is not guaranteed (see mad2modloader's
// mod_loader.cpp), so consumer mods don't link against mad2xinput.dll at
// build time. Instead call Mad2XInput_Resolve() to look up its exports by
// name at runtime; do this lazily (e.g. the first time your per-frame
// hook fires) rather than from DllMain, since mad2xinput.dll is
// guaranteed to have finished loading by then even if it loaded after you.
//
// Overrides are LAYERED, not single-slot: each Mad2XInput_AddOverride call
// returns a handle representing one independent "layer" for a controller.
// Every active layer for a controller is applied in sequence (in the order
// they were added) to the state the game receives, so e.g.
// mad2inputeffectsmod's JoystickReversal and RandomPressA effects can both
// be active on the same controller at once without clobbering each other
// -- each is its own layer. Remove a layer with Mad2XInput_RemoveOverride
// when its effect ends; the handle becomes invalid immediately after.
#include <windows.h>
#include <xinput.h>

// Opaque per-layer handle returned by Mad2XInput_AddOverride. 0 is never a
// valid handle.
typedef LONG Mad2XInputOverrideHandle;

// Bitmask for Mad2XInputOverride::blockStickDirections -- zeroes that
// axis's positive/negative component on the affected stick when the
// player pushes it that way (used by the "disable one stick direction"
// chaos effect). Independent of forceBlockButtons, which only affects the
// digital face/shoulder/dpad buttons in wButtons.
#define MAD2XINPUT_BLOCK_LEFT_UP 0x0001
#define MAD2XINPUT_BLOCK_LEFT_DOWN 0x0002
#define MAD2XINPUT_BLOCK_LEFT_LEFT 0x0004
#define MAD2XINPUT_BLOCK_LEFT_RIGHT 0x0008
#define MAD2XINPUT_BLOCK_RIGHT_UP 0x0010
#define MAD2XINPUT_BLOCK_RIGHT_DOWN 0x0020
#define MAD2XINPUT_BLOCK_RIGHT_LEFT 0x0040
#define MAD2XINPUT_BLOCK_RIGHT_RIGHT 0x0080

// One override layer's worth of transformations. Fields left at their
// default (0 / FALSE) are pass-through for that layer. Every active
// layer for a controller is applied to the gamepad state in this same
// field order (hold/block buttons, then swap, then invert, then scale,
// then direction-block, then force -- force wins last within a layer)
// -- see mad2xinput/src/xinput_api.cpp's HookXInputGetState for the exact
// per-layer pipeline.
struct Mad2XInputOverride {
    WORD forceHoldButtons;   // OR'd into wButtons
    WORD forceBlockButtons;  // AND-NOT'd out of wButtons
    BOOL swapAB;             // swaps the A/B bits of wButtons (after hold/block)
    BOOL swapXY;             // swaps the X/Y bits of wButtons (after hold/block)
    BOOL invertLX, invertLY, invertRX, invertRY;  // negates the axis
    BOOL enableScaleL, enableScaleR;              // scales stick magnitude by scaleL/scaleR
    float scaleL, scaleR;                         // 0.0-1.0 (applied to both axes of that stick)
    WORD blockStickDirections;  // MAD2XINPUT_BLOCK_* bitmask -- see above
    BOOL enableForceLX, enableForceLY, enableForceRX, enableForceRY;
    SHORT forceLX, forceLY, forceRX, forceRY;  // applied last, overrides everything above for that axis
};

// Returns the most recent raw (pre-override) state observed for
// dwUserIndex, i.e. what the player is physically doing regardless of any
// active override layers -- what hotkey detection and input-display mods
// want. Returns FALSE (and leaves *outState untouched) if that controller
// hasn't been seen connected yet.
typedef BOOL(WINAPI* Mad2XInput_GetState_t)(DWORD dwUserIndex, XINPUT_STATE* outState);

// Returns the most recent POST-override state for dwUserIndex, i.e. exactly
// what the game itself received on its last poll (every active override
// layer already applied, in the same order the game sees them). Intended
// for mods that need to mirror the game's actual effective input elsewhere
// (e.g. mad2sm64mod streaming it to a separate SM64 process, so chaos
// effects like JoystickReversal apply there too) rather than reacting to
// the player's raw physical input. Returns FALSE (and leaves *outState
// untouched) if that controller hasn't been seen connected yet.
typedef BOOL(WINAPI* Mad2XInput_GetOverriddenState_t)(DWORD dwUserIndex, XINPUT_STATE* outState);

// Adds a new override layer for dwUserIndex, active on every subsequent
// XInputGetState call the game makes until removed. Returns a handle
// (never 0 on success) to later pass to Mad2XInput_UpdateOverride/
// RemoveOverride. Independent of any other layers already active for the
// same controller.
typedef Mad2XInputOverrideHandle(WINAPI* Mad2XInput_AddOverride_t)(DWORD dwUserIndex, const Mad2XInputOverride* override_);

// Replaces the transformation data for an existing layer in place (same
// position in the per-controller application order). No-op if handle is
// unknown/already removed.
typedef void(WINAPI* Mad2XInput_UpdateOverride_t)(Mad2XInputOverrideHandle handle, const Mad2XInputOverride* override_);

// Removes a layer; the game goes back to seeing whatever the remaining
// layers (if any) produce. No-op if handle is unknown/already removed.
typedef void(WINAPI* Mad2XInput_RemoveOverride_t)(Mad2XInputOverrideHandle handle);

#define MAD2XINPUT_DLL_NAME "mad2xinput.dll"
#define MAD2XINPUT_GETSTATE_PROC "Mad2XInput_GetState"
#define MAD2XINPUT_GETOVERRIDDENSTATE_PROC "Mad2XInput_GetOverriddenState"
#define MAD2XINPUT_ADDOVERRIDE_PROC "Mad2XInput_AddOverride"
#define MAD2XINPUT_UPDATEOVERRIDE_PROC "Mad2XInput_UpdateOverride"
#define MAD2XINPUT_REMOVEOVERRIDE_PROC "Mad2XInput_RemoveOverride"

struct Mad2XInputApi {
    Mad2XInput_GetState_t GetState;
    Mad2XInput_GetOverriddenState_t GetOverriddenState;
    Mad2XInput_AddOverride_t AddOverride;
    Mad2XInput_UpdateOverride_t UpdateOverride;
    Mad2XInput_RemoveOverride_t RemoveOverride;
};

// Resolves mad2xinput.dll's exports by name. Cheap to call every frame
// (resolves once, then returns the cached result). If mad2xinput.dll
// isn't loaded (e.g. missing from mad2/mods/), every member is null --
// callers must null-check GetState/AddOverride/UpdateOverride/RemoveOverride
// before use.
inline const Mad2XInputApi& Mad2XInput_Resolve() {
    static Mad2XInputApi api{};
    static bool resolved = false;
    if (!resolved) {
        if (HMODULE h = GetModuleHandleA(MAD2XINPUT_DLL_NAME)) {
            api.GetState = reinterpret_cast<Mad2XInput_GetState_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2XINPUT_GETSTATE_PROC)));
            api.GetOverriddenState = reinterpret_cast<Mad2XInput_GetOverriddenState_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2XINPUT_GETOVERRIDDENSTATE_PROC)));
            api.AddOverride = reinterpret_cast<Mad2XInput_AddOverride_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2XINPUT_ADDOVERRIDE_PROC)));
            api.UpdateOverride = reinterpret_cast<Mad2XInput_UpdateOverride_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2XINPUT_UPDATEOVERRIDE_PROC)));
            api.RemoveOverride = reinterpret_cast<Mad2XInput_RemoveOverride_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2XINPUT_REMOVEOVERRIDE_PROC)));
            resolved = api.GetState != nullptr;
        }
    }
    return api;
}
