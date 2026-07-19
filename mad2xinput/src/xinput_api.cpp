// Generic XInput hook + injection API, shared by other mad2modloader mods.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Hooks XInputGetState at
// the IAT level (self-contained copy of the same technique used by
// mad2assetloader/mad2saveloader/mad2shadowfix/mad2igttimer -- see those
// for the pattern, and mad2assetloader/src/iat_hook.cpp for the most
// thoroughly-commented version). The game's only import of it is
// igDisplay.dll!XInputGetState (confirmed via objdump -p on the shipped
// DLLs) -- Mad2.exe itself does not import xinput1_3.dll directly.
//
// Every observed poll is cached (raw, pre-override) so other in-process
// mods can read "what is the player physically doing right now" via the
// exported Mad2XInput_GetState without installing their own hook -- see
// include/mad2xinput_api.h for the client-side contract. Mods can also add
// one or more LAYERED overrides per controller (forced/blocked/swapped
// buttons, inverted/scaled/blocked/forced stick axes) via
// Mad2XInput_AddOverride, each applied in sequence to the state the game
// itself receives on every subsequent poll -- see the struct/handle
// contract in include/mad2xinput_api.h for why this is layered rather than
// single-slot (multiple mad2inputeffectsmod chaos effects can be active on
// the same controller simultaneously).
#include <windows.h>
#include <xinput.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <vector>

#include "../include/mad2xinput_api.h"

#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

// ---------------------------------------------------------------------
// Logging -- writes into the shared logs\mad2.log (see mad2sharedlog.h),
// tagged "[mad2xinput]" so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2xinput]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Minimal IAT patcher (self-contained copy; see mad2assetloader/src/iat_hook.cpp
// for the canonical, fully-commented version of this same technique).
// ---------------------------------------------------------------------

// Unlike CreateFileA/Direct3DCreate9 (imported by name everywhere), the
// only module that imports XInputGetState -- igDisplay.dll -- imports it
// by ordinal (confirmed via objdump -p: three ordinal-only thunks, no
// Member-Name, for ordinals 5, 2, 3). Ordinal 2 is XInputGetState --
// cross-checked against both the real xinput1_3.dll's export table
// (xinput1_3.dll.bak in the game dir) and Wine's builtin xinput1_3.dll
// (GE-Proton's files/lib/wine/i386-windows/xinput1_3.dll), which agree:
// ordinal 2 = XInputGetState, 3 = XInputSetState, 5 = XInputEnable. (The
// other two ordinals igDisplay.dll imports, 5 and 3, are XInputEnable and
// XInputSetState -- not hooked here.) Getting this wrong hooks the wrong
// function with the wrong signature: igDisplay.dll's ordinal-5 call site
// pushes XInputEnable's single BOOL argument, and calling through a
// hook declared as XInputGetState's two-argument stdcall signature pops
// the wrong number of bytes off the stack on return, corrupting the
// caller's return address -- this is exactly what happened when this was
// hardcoded to 5 instead of 2 (crashed instantly with a wild jump into
// unmapped memory).
static const WORD kXInputGetStateOrdinal = 2;

// IAT hooking itself now provided by aa_mad2hookutil.dll (per decision
// #12a: this mod's ordinal-aware variant became the optional
// matchByOrdinal/importOrdinal parameter on the shared PatchIat/
// PatchIatAllModules, rather than staying a separate copy) -- see
// ../../mad2hookutil/include/mad2hookutil_api.h's Mad2HookUtil_PatchIatOrdinal/
// Mad2HookUtil_PatchIatAllModulesOrdinal. Resolved synchronously (not
// lazily) since aa_mad2hookutil.dll is guaranteed already loaded by the
// time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Per-controller cached state + layered overrides.
// ---------------------------------------------------------------------

struct OverrideLayer {
    Mad2XInputOverrideHandle handle;
    Mad2XInputOverride data;
};

struct UserSlot {
    XINPUT_STATE lastRaw{};
    XINPUT_STATE lastOverridden{};
    BOOL connected = FALSE;
    std::vector<OverrideLayer> layers;
};

static UserSlot g_Slots[XUSER_MAX_COUNT];
// Guards g_Slots[*].layers -- AddOverride/UpdateOverride/RemoveOverride can
// be called from any mod's own thread (e.g. mad2inputeffectsmod's periodic-
// press worker threads), concurrently with HookXInputGetState running on
// the game's own thread.
static CRITICAL_SECTION g_LayersLock;
static LONG g_NextHandle = 1;  // 0 is reserved as "invalid handle"

// Negates a stick axis in place. -32768 has no positive counterpart in a
// signed 16-bit range, so negating it directly would overflow; clamp that
// one case to 32767 instead.
static SHORT InvertAxis(SHORT v) { return v == -32768 ? 32767 : static_cast<SHORT>(-v); }

// Applies one override layer's transformations to gp, in a fixed order:
// hold/block buttons, then swap A/B and X/Y, then invert axes, then scale
// stick magnitude, then block specific stick directions, then force exact
// axis values (wins last, overriding everything above for that axis). See
// include/mad2xinput_api.h's Mad2XInputOverride comment for why this order
// was chosen.
static void ApplyOverrideLayer(XINPUT_GAMEPAD& gp, const Mad2XInputOverride& ov) {
    gp.wButtons = (gp.wButtons | ov.forceHoldButtons) & static_cast<WORD>(~ov.forceBlockButtons);

    if (ov.swapAB) {
        WORD a = gp.wButtons & XINPUT_GAMEPAD_A, b = gp.wButtons & XINPUT_GAMEPAD_B;
        gp.wButtons = static_cast<WORD>((gp.wButtons & ~(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B)) |
                                         (a ? XINPUT_GAMEPAD_B : 0) | (b ? XINPUT_GAMEPAD_A : 0));
    }
    if (ov.swapXY) {
        WORD x = gp.wButtons & XINPUT_GAMEPAD_X, y = gp.wButtons & XINPUT_GAMEPAD_Y;
        gp.wButtons = static_cast<WORD>((gp.wButtons & ~(XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y)) |
                                         (x ? XINPUT_GAMEPAD_Y : 0) | (y ? XINPUT_GAMEPAD_X : 0));
    }

    if (ov.invertLX) gp.sThumbLX = InvertAxis(gp.sThumbLX);
    if (ov.invertLY) gp.sThumbLY = InvertAxis(gp.sThumbLY);
    if (ov.invertRX) gp.sThumbRX = InvertAxis(gp.sThumbRX);
    if (ov.invertRY) gp.sThumbRY = InvertAxis(gp.sThumbRY);

    if (ov.enableScaleL) {
        gp.sThumbLX = static_cast<SHORT>(gp.sThumbLX * ov.scaleL);
        gp.sThumbLY = static_cast<SHORT>(gp.sThumbLY * ov.scaleL);
    }
    if (ov.enableScaleR) {
        gp.sThumbRX = static_cast<SHORT>(gp.sThumbRX * ov.scaleR);
        gp.sThumbRY = static_cast<SHORT>(gp.sThumbRY * ov.scaleR);
    }

    if (ov.blockStickDirections) {
        if ((ov.blockStickDirections & MAD2XINPUT_BLOCK_LEFT_UP) && gp.sThumbLY > 0) gp.sThumbLY = 0;
        if ((ov.blockStickDirections & MAD2XINPUT_BLOCK_LEFT_DOWN) && gp.sThumbLY < 0) gp.sThumbLY = 0;
        if ((ov.blockStickDirections & MAD2XINPUT_BLOCK_LEFT_LEFT) && gp.sThumbLX < 0) gp.sThumbLX = 0;
        if ((ov.blockStickDirections & MAD2XINPUT_BLOCK_LEFT_RIGHT) && gp.sThumbLX > 0) gp.sThumbLX = 0;
        if ((ov.blockStickDirections & MAD2XINPUT_BLOCK_RIGHT_UP) && gp.sThumbRY > 0) gp.sThumbRY = 0;
        if ((ov.blockStickDirections & MAD2XINPUT_BLOCK_RIGHT_DOWN) && gp.sThumbRY < 0) gp.sThumbRY = 0;
        if ((ov.blockStickDirections & MAD2XINPUT_BLOCK_RIGHT_LEFT) && gp.sThumbRX < 0) gp.sThumbRX = 0;
        if ((ov.blockStickDirections & MAD2XINPUT_BLOCK_RIGHT_RIGHT) && gp.sThumbRX > 0) gp.sThumbRX = 0;
    }

    if (ov.enableForceLX) gp.sThumbLX = ov.forceLX;
    if (ov.enableForceLY) gp.sThumbLY = ov.forceLY;
    if (ov.enableForceRX) gp.sThumbRX = ov.forceRX;
    if (ov.enableForceRY) gp.sThumbRY = ov.forceRY;
}

typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XINPUT_STATE*);
static XInputGetState_t RealXInputGetState = nullptr;

// Diagnostic for a Linux/Steam Input-specific theory: on Linux, Steam
// Input can present a single physical controller as more than one
// XInput-visible device (e.g. a virtual Xbox-mapped device alongside the
// raw one), which would mean overrides applied to userIndex 0 only affect
// HALF of what the game might actually be reading from if it polls (or
// bounces between) multiple indices. Logs every CONNECT/DISCONNECT
// transition per slot (not every poll -- this function can be called many
// times a second) plus which packet number each slot reports, so the log
// can answer "is more than one slot ever simultaneously connected" and
// "is the game (or Steam Input) changing which slot has a fresh packet."
static bool g_EverConnected[XUSER_MAX_COUNT] = {};

static DWORD WINAPI HookXInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
    if (!RealXInputGetState) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD result = RealXInputGetState(dwUserIndex, pState);

    if (dwUserIndex < XUSER_MAX_COUNT) {
        UserSlot& slot = g_Slots[dwUserIndex];
        bool wasConnected = slot.connected;
        if (result == ERROR_SUCCESS && pState) {
            slot.lastRaw = *pState;  // raw, pre-override -- what the player is physically doing
            slot.connected = TRUE;
            if (!wasConnected) {
                g_EverConnected[dwUserIndex] = true;
                int connectedCount = 0;
                for (bool c : g_EverConnected) connectedCount += c ? 1 : 0;
                Log("[Diag] userIndex=%lu CONNECTED (packet=%lu). Slots ever seen connected so far: %d/%d -- "
                    "if this exceeds 1, more than one XInput-visible device exists (possible Steam Input "
                    "double-exposure) and overrides only ever target userIndex 0.\n",
                    dwUserIndex, pState->dwPacketNumber, connectedCount, XUSER_MAX_COUNT);
            }

            XINPUT_GAMEPAD rawForLog = pState->Gamepad;
            EnterCriticalSection(&g_LayersLock);
            bool hadLayers = !slot.layers.empty();
            for (const auto& layer : slot.layers) ApplyOverrideLayer(pState->Gamepad, layer.data);
            LeaveCriticalSection(&g_LayersLock);

            // Post-override -- exactly what the game itself receives this
            // poll. Separate from lastRaw above (pre-override, "what the
            // player is physically doing") -- see Mad2XInput_GetOverriddenState
            // in mad2xinput_api.h for who reads this and why.
            slot.lastOverridden = *pState;

            // Throttled (not every poll) raw-vs-overridden dump for slot 0
            // while at least one override layer is active -- shows
            // directly in the log whether this slot is receiving live,
            // changing input that tracks what the player does (confirming
            // the override path itself is fine) or looks stale/frozen
            // (which would point at the game reading input from a
            // different slot/device entirely -- see the CONNECTED log
            // line above for the multi-device theory this is checking).
            if (dwUserIndex == 0 && hadLayers) {
                static uint64_t lastDumpMs = 0;
                uint64_t nowMs = GetTickCount64();
                if (nowMs - lastDumpMs >= 2000) {
                    lastDumpMs = nowMs;
                    Log("[Diag] userIndex=0 packet=%lu raw LX=%d LY=%d RX=%d RY=%d buttons=0x%04X -> "
                        "override LX=%d LY=%d RX=%d RY=%d buttons=0x%04X\n",
                        pState->dwPacketNumber, rawForLog.sThumbLX, rawForLog.sThumbLY, rawForLog.sThumbRX,
                        rawForLog.sThumbRY, rawForLog.wButtons, pState->Gamepad.sThumbLX, pState->Gamepad.sThumbLY,
                        pState->Gamepad.sThumbRX, pState->Gamepad.sThumbRY, pState->Gamepad.wButtons);
                }
            }
        } else {
            slot.connected = FALSE;
            if (wasConnected) Log("[Diag] userIndex=%lu DISCONNECTED\n", dwUserIndex);
        }
    }
    return result;
}

static void InstallHooks() {
    void* prev = nullptr;
    int n = Mad2HookUtil_PatchIatAllModulesOrdinal("xinput1_3.dll", "XInputGetState", kXInputGetStateOrdinal,
                                                    reinterpret_cast<void*>(HookXInputGetState), &prev);
    if (prev) RealXInputGetState = reinterpret_cast<XInputGetState_t>(prev);
    Log("Patched XInputGetState in %d module(s) (chained real=%p)\n", n, (void*)RealXInputGetState);
}

// ---------------------------------------------------------------------
// Public exports -- see include/mad2xinput_api.h for the client contract.
// ---------------------------------------------------------------------

extern "C" __declspec(dllexport) BOOL WINAPI Mad2XInput_GetState(DWORD dwUserIndex, XINPUT_STATE* outState) {
    if (dwUserIndex >= XUSER_MAX_COUNT || !outState) return FALSE;
    UserSlot& slot = g_Slots[dwUserIndex];
    if (!slot.connected) return FALSE;
    *outState = slot.lastRaw;
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2XInput_GetOverriddenState(DWORD dwUserIndex, XINPUT_STATE* outState) {
    if (dwUserIndex >= XUSER_MAX_COUNT || !outState) return FALSE;
    UserSlot& slot = g_Slots[dwUserIndex];
    if (!slot.connected) return FALSE;
    *outState = slot.lastOverridden;
    return TRUE;
}

extern "C" __declspec(dllexport) Mad2XInputOverrideHandle WINAPI
Mad2XInput_AddOverride(DWORD dwUserIndex, const Mad2XInputOverride* override_) {
    if (dwUserIndex >= XUSER_MAX_COUNT || !override_) return 0;

    Mad2XInputOverrideHandle handle = InterlockedIncrement(&g_NextHandle);
    EnterCriticalSection(&g_LayersLock);
    g_Slots[dwUserIndex].layers.push_back({handle, *override_});
    LeaveCriticalSection(&g_LayersLock);
    return handle;
}

extern "C" __declspec(dllexport) void WINAPI Mad2XInput_UpdateOverride(Mad2XInputOverrideHandle handle,
                                                                         const Mad2XInputOverride* override_) {
    if (!handle || !override_) return;
    EnterCriticalSection(&g_LayersLock);
    for (auto& slot : g_Slots) {
        for (auto& layer : slot.layers) {
            if (layer.handle == handle) {
                layer.data = *override_;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_LayersLock);
}

extern "C" __declspec(dllexport) void WINAPI Mad2XInput_RemoveOverride(Mad2XInputOverrideHandle handle) {
    if (!handle) return;
    EnterCriticalSection(&g_LayersLock);
    for (auto& slot : g_Slots) {
        auto& layers = slot.layers;
        layers.erase(std::remove_if(layers.begin(), layers.end(),
                                     [handle](const OverrideLayer& l) { return l.handle == handle; }),
                     layers.end());
    }
    LeaveCriticalSection(&g_LayersLock);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InitializeCriticalSection(&g_LayersLock);
            InstallHooks();
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
