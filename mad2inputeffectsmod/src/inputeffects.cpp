// Input chaos effects for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Registers ten effects with
// mad2effects (mad2/mods/mad2effects.dll -- see
// ../../mad2effects/include/mad2effects_api.h), each implemented as one or
// more mad2xinput (mad2/mods/mad2xinput.dll -- see
// ../../mad2xinput/include/mad2xinput_api.h) override LAYERS on the
// player's controller:
//   - JoystickReversal: randomly picks 1-2 of the currently-enabled axes
//     (left/right stick X/Y, both sticks enabled by default) and inverts
//     just those, chosen fresh each trigger -- see ApplyJoystickReversal.
//   - StickPercentLimit: scales stick magnitude down to a random percent
//     of full, chosen fresh each trigger.
//   - SwapAB / SwapXY: swaps those two face buttons.
//   - RandomPressStart / RandomPressA / RandomPressB / PressY / PressX:
//     each spawns its own worker thread that pulses the button (a brief
//     forceHoldButtons override, added then removed) at a randomized
//     interval, for a fixed one-minute duration.
//   - StickDirectionDisable: picks one of the two sticks and one of its
//     four cardinal directions at random, then blocks that single
//     direction for a minute. Explicitly cannot stack with itself -- a
//     re-trigger while already active replaces the previous stick/
//     direction choice (and its override layer) rather than adding a
//     second one; see ApplyStickDirectionDisable.
//
// mad2xinput's override system is LAYERED (Mad2XInput_AddOverride returns
// a handle; multiple layers on the same controller compose), which is
// exactly what lets several of these effects be simultaneously active
// (e.g. JoystickReversal + SwapAB + a RandomPressA pulse all at once)
// without clobbering each other -- see mad2xinput_api.h's file header for
// why this replaced the older single-slot SetOverride/ClearOverride API.
//
// mad2inputdisplay needs NO changes for "show real input, not the chaos
// effects": Mad2XInput_GetState returns each controller's raw, pre-
// override state (captured before mad2xinput's layer-application loop
// runs), and inputdisplay reads exclusively through that function.
//
// This mod has no per-frame D3D9 hook (no HUD of its own), so unlike the
// overlay mods it can't lazily resolve its dependencies from an EndScene
// hook. Instead it uses an "init-only background thread": DllMain spawns
// a thread that retries (with a short sleep) until mad2config, mad2xinput,
// and mad2effects have all finished loading and exporting -- mad2/mods/*.dll
// load order is not guaranteed -- then registers every effect and exits.
// Each effect's own Apply/Clear callbacks (and, for the periodic-press
// effects, their worker threads) do the actual work after that; there's
// nothing left for the init thread itself to do.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2effects/include/mad2effects_api.h"
#include "../../mad2xinput/include/mad2xinput_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2inputeffectsmod]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [InputEffects] section -- one section for the
// whole mod, keys prefixed per effect, matching how [GraphicsEffects] etc.
// scope every other mod's settings under one section).
// ---------------------------------------------------------------------

struct Config {
    DWORD userIndex = 0;  // which controller (XInput user index) every effect targets when
                           // broadcastToAllControllers is false
    // Steam Input on Linux can present a single physical controller as more
    // than one XInput-visible device (confirmed via mad2xinput.log's [Diag]
    // CONNECTED lines showing both userIndex 0 and 1 connected simultaneously
    // for one controller) -- if the game happens to poll a different slot
    // than whatever this mod targets, overrides silently have no effect part
    // of the time, which is exactly what "reversed controls only sometimes
    // work" turned out to be. Defaulting to broadcasting every override to
    // all 4 possible slots sidesteps needing to know which one is "real":
    // applying the same override to an unconnected/phantom slot is a no-op
    // (mad2xinput only ever applies a slot's layers when that slot's own
    // XInputGetState poll succeeds), so this is safe even with only one
    // genuine controller.
    bool broadcastToAllControllers = true;

    // JoystickReversal -- these are ELIGIBILITY flags (which axes may be
    // picked), not "always inverted": on each trigger, 1-2 of the axes
    // enabled here are chosen at random and inverted -- see
    // ApplyJoystickReversal. Both sticks enabled by default.
    bool invertLeftX = true, invertLeftY = true, invertRightX = true, invertRightY = true;
    int reversalWeight = 10;
    DWORD reversalMinDurationMs = 10000, reversalMaxDurationMs = 20000;

    // StickPercentLimit
    int stickLimitMinPercent = 10, stickLimitMaxPercent = 70;
    bool stickLimitAffectLeft = true, stickLimitAffectRight = true;
    int stickLimitWeight = 10;
    DWORD stickLimitMinDurationMs = 10000, stickLimitMaxDurationMs = 20000;

    // SwapAB / SwapXY
    int swapABWeight = 10;
    DWORD swapABMinDurationMs = 10000, swapABMaxDurationMs = 20000;
    int swapXYWeight = 10;
    DWORD swapXYMinDurationMs = 10000, swapXYMaxDurationMs = 20000;

    // Periodic button presses -- each pulses its button at a randomized
    // interval for a fixed one-minute duration.
    int pressStartMinIntervalMs = 2000, pressStartMaxIntervalMs = 15000, pressStartWeight = 10;
    DWORD pressStartDurationMs = 60000;
    int pressAMinIntervalMs = 1000, pressAMaxIntervalMs = 5000, pressAWeight = 10;
    DWORD pressADurationMs = 60000;
    int pressBMinIntervalMs = 1000, pressBMaxIntervalMs = 2000, pressBWeight = 10;
    DWORD pressBDurationMs = 60000;
    int pressYIntervalMs = 1000, pressYWeight = 10;
    DWORD pressYDurationMs = 60000;
    int pressXMinIntervalMs = 10000, pressXMaxIntervalMs = 20000, pressXWeight = 10;
    DWORD pressXDurationMs = 60000;

    // StickDirectionDisable
    int stickDisableWeight = 10;
    DWORD stickDisableDurationMs = 60000;
} g_Config;

static void LoadConfig() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetInt) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in defaults\n");
        return;
    }

    g_Config.userIndex = static_cast<DWORD>(api.GetInt(
        "InputEffects", "UserIndex", static_cast<int>(g_Config.userIndex),
        "Which controller (XInput user index, 0-3) every effect targets when\n"
        "BroadcastToAllControllers is false."));
    g_Config.broadcastToAllControllers =
        api.GetBool("InputEffects", "BroadcastToAllControllers", g_Config.broadcastToAllControllers ? TRUE : FALSE,
                     "Apply every effect to all 4 possible XInput controller slots instead of just\n"
                     "UserIndex. Recommended on: Steam Input on Linux can expose one physical\n"
                     "controller as more than one XInput-visible slot, and if the game happens to\n"
                     "poll a slot other than UserIndex, overrides would silently do nothing part of\n"
                     "the time. Safe to leave on even with a single controller.") != FALSE;

    g_Config.invertLeftX = api.GetBool("InputEffects", "InvertLeftX", g_Config.invertLeftX,
                                        "JoystickReversal: whether the left stick's X axis is eligible to be\n"
                                        "randomly picked for inversion (1-2 eligible axes get inverted per\n"
                                        "trigger, not all of them).") != FALSE;
    g_Config.invertLeftY = api.GetBool("InputEffects", "InvertLeftY", g_Config.invertLeftY,
                                        "JoystickReversal: whether the left stick's Y axis is eligible to be\n"
                                        "randomly picked for inversion.") != FALSE;
    g_Config.invertRightX = api.GetBool("InputEffects", "InvertRightX", g_Config.invertRightX,
                                         "JoystickReversal: whether the right stick's X axis is eligible to be\n"
                                         "randomly picked for inversion.") != FALSE;
    g_Config.invertRightY = api.GetBool("InputEffects", "InvertRightY", g_Config.invertRightY,
                                         "JoystickReversal: whether the right stick's Y axis is eligible to be\n"
                                         "randomly picked for inversion.") != FALSE;
    g_Config.reversalWeight =
        api.GetInt("InputEffects", "ReversalWeight", g_Config.reversalWeight, "JoystickReversal: chance-pool weight.");
    g_Config.reversalMinDurationMs = static_cast<DWORD>(api.GetInt(
        "InputEffects", "ReversalMinDurationMs", static_cast<int>(g_Config.reversalMinDurationMs),
        "JoystickReversal: active duration (ms), randomized between Min/MaxDurationMs."));
    g_Config.reversalMaxDurationMs = static_cast<DWORD>(api.GetInt(
        "InputEffects", "ReversalMaxDurationMs", static_cast<int>(g_Config.reversalMaxDurationMs), ""));

    g_Config.stickLimitMinPercent =
        api.GetInt("InputEffects", "StickLimitMinPercent", g_Config.stickLimitMinPercent,
                    "StickPercentLimit: scales stick range to a random percent of full, chosen fresh each\n"
                    "trigger between StickLimitMinPercent and StickLimitMaxPercent.");
    g_Config.stickLimitMaxPercent =
        api.GetInt("InputEffects", "StickLimitMaxPercent", g_Config.stickLimitMaxPercent, "");
    g_Config.stickLimitAffectLeft = api.GetBool("InputEffects", "StickLimitAffectLeft", g_Config.stickLimitAffectLeft,
                                                 "StickPercentLimit: affect the left stick.") != FALSE;
    g_Config.stickLimitAffectRight =
        api.GetBool("InputEffects", "StickLimitAffectRight", g_Config.stickLimitAffectRight,
                     "StickPercentLimit: affect the right stick.") != FALSE;
    g_Config.stickLimitWeight =
        api.GetInt("InputEffects", "StickLimitWeight", g_Config.stickLimitWeight, "StickPercentLimit: chance-pool weight.");
    g_Config.stickLimitMinDurationMs = static_cast<DWORD>(
        api.GetInt("InputEffects", "StickLimitMinDurationMs", static_cast<int>(g_Config.stickLimitMinDurationMs),
                    "StickPercentLimit: active duration (ms)."));
    g_Config.stickLimitMaxDurationMs = static_cast<DWORD>(
        api.GetInt("InputEffects", "StickLimitMaxDurationMs", static_cast<int>(g_Config.stickLimitMaxDurationMs), ""));

    g_Config.swapABWeight =
        api.GetInt("InputEffects", "SwapABWeight", g_Config.swapABWeight, "SwapAB: chance-pool weight.");
    g_Config.swapABMinDurationMs = static_cast<DWORD>(api.GetInt(
        "InputEffects", "SwapABMinDurationMs", static_cast<int>(g_Config.swapABMinDurationMs), "SwapAB: active duration (ms)."));
    g_Config.swapABMaxDurationMs =
        static_cast<DWORD>(api.GetInt("InputEffects", "SwapABMaxDurationMs", static_cast<int>(g_Config.swapABMaxDurationMs), ""));

    g_Config.swapXYWeight =
        api.GetInt("InputEffects", "SwapXYWeight", g_Config.swapXYWeight, "SwapXY: chance-pool weight.");
    g_Config.swapXYMinDurationMs = static_cast<DWORD>(api.GetInt(
        "InputEffects", "SwapXYMinDurationMs", static_cast<int>(g_Config.swapXYMinDurationMs), "SwapXY: active duration (ms)."));
    g_Config.swapXYMaxDurationMs =
        static_cast<DWORD>(api.GetInt("InputEffects", "SwapXYMaxDurationMs", static_cast<int>(g_Config.swapXYMaxDurationMs), ""));

    g_Config.pressStartMinIntervalMs =
        api.GetInt("InputEffects", "PressStartMinIntervalMs", g_Config.pressStartMinIntervalMs,
                    "RandomPressStart: presses Start at a random interval (ms) between Min/MaxIntervalMs,\n"
                    "for PressStartDurationMs once triggered.");
    g_Config.pressStartMaxIntervalMs =
        api.GetInt("InputEffects", "PressStartMaxIntervalMs", g_Config.pressStartMaxIntervalMs, "");
    g_Config.pressStartWeight =
        api.GetInt("InputEffects", "PressStartWeight", g_Config.pressStartWeight, "RandomPressStart: chance-pool weight.");
    g_Config.pressStartDurationMs = static_cast<DWORD>(
        api.GetInt("InputEffects", "PressStartDurationMs", static_cast<int>(g_Config.pressStartDurationMs), ""));

    g_Config.pressAMinIntervalMs =
        api.GetInt("InputEffects", "PressAMinIntervalMs", g_Config.pressAMinIntervalMs,
                    "RandomPressA: presses A at a random interval (ms) between Min/MaxIntervalMs, for\n"
                    "PressADurationMs once triggered.");
    g_Config.pressAMaxIntervalMs = api.GetInt("InputEffects", "PressAMaxIntervalMs", g_Config.pressAMaxIntervalMs, "");
    g_Config.pressAWeight = api.GetInt("InputEffects", "PressAWeight", g_Config.pressAWeight, "RandomPressA: chance-pool weight.");
    g_Config.pressADurationMs =
        static_cast<DWORD>(api.GetInt("InputEffects", "PressADurationMs", static_cast<int>(g_Config.pressADurationMs), ""));

    g_Config.pressBMinIntervalMs =
        api.GetInt("InputEffects", "PressBMinIntervalMs", g_Config.pressBMinIntervalMs,
                    "RandomPressB: presses B at a random interval (ms) between Min/MaxIntervalMs, for\n"
                    "PressBDurationMs once triggered.");
    g_Config.pressBMaxIntervalMs = api.GetInt("InputEffects", "PressBMaxIntervalMs", g_Config.pressBMaxIntervalMs, "");
    g_Config.pressBWeight = api.GetInt("InputEffects", "PressBWeight", g_Config.pressBWeight, "RandomPressB: chance-pool weight.");
    g_Config.pressBDurationMs =
        static_cast<DWORD>(api.GetInt("InputEffects", "PressBDurationMs", static_cast<int>(g_Config.pressBDurationMs), ""));

    g_Config.pressYIntervalMs =
        api.GetInt("InputEffects", "PressYIntervalMs", g_Config.pressYIntervalMs,
                    "PressY: presses Y every PressYIntervalMs, for PressYDurationMs once triggered.");
    g_Config.pressYWeight = api.GetInt("InputEffects", "PressYWeight", g_Config.pressYWeight, "PressY: chance-pool weight.");
    g_Config.pressYDurationMs =
        static_cast<DWORD>(api.GetInt("InputEffects", "PressYDurationMs", static_cast<int>(g_Config.pressYDurationMs), ""));

    g_Config.pressXMinIntervalMs =
        api.GetInt("InputEffects", "PressXMinIntervalMs", g_Config.pressXMinIntervalMs,
                    "PressX: presses X at a random interval (ms) between Min/MaxIntervalMs, for\n"
                    "PressXDurationMs once triggered.");
    g_Config.pressXMaxIntervalMs = api.GetInt("InputEffects", "PressXMaxIntervalMs", g_Config.pressXMaxIntervalMs, "");
    g_Config.pressXWeight = api.GetInt("InputEffects", "PressXWeight", g_Config.pressXWeight, "PressX: chance-pool weight.");
    g_Config.pressXDurationMs =
        static_cast<DWORD>(api.GetInt("InputEffects", "PressXDurationMs", static_cast<int>(g_Config.pressXDurationMs), ""));

    g_Config.stickDisableWeight = api.GetInt("InputEffects", "StickDisableWeight", g_Config.stickDisableWeight,
                                              "StickDirectionDisable: chance-pool weight. Picks one stick and one of\n"
                                              "its four directions at random each trigger and blocks it; cannot\n"
                                              "stack with itself (a re-trigger just picks a new stick/direction).");
    g_Config.stickDisableDurationMs = static_cast<DWORD>(
        api.GetInt("InputEffects", "StickDisableDurationMs", static_cast<int>(g_Config.stickDisableDurationMs), ""));

    Log("Config loaded (userIndex=%lu)\n", g_Config.userIndex);
}

// ---------------------------------------------------------------------
// JoystickReversal.
// ---------------------------------------------------------------------

// One handle per possible XInput slot -- see Config::broadcastToAllControllers
// for why a single handle/single userIndex isn't enough (Steam Input can
// expose one physical controller as more than one XInput-visible slot).
struct HandleSet {
    Mad2XInputOverrideHandle h[XUSER_MAX_COUNT] = {};
};

// Re-triggering an already-active effect (mad2effects allows this -- it
// just re-applies and resets the clear timer, see mad2effects_api.h) must
// NOT accumulate a second override layer on top of the first: two
// simultaneous invertLX layers on the same axis cancel each other out
// (double negation), two simultaneous swapAB layers cancel out (double
// swap), and two simultaneous scale layers compound multiplicatively --
// none of that is "still applying the effect," it's silent corruption
// that gets worse the more times chaos mode happens to re-trigger the
// same effect while it's already running. Every AddOverride-based effect
// below must remove its own previous layer(s) (if any) before adding new
// ones -- this helper (and ClearOverrideSet below) make that the only way
// to do it.
static void ReplaceOverride(HandleSet& handles, const Mad2XInputOverride& ov) {
    const auto& api = Mad2XInput_Resolve();
    if (!api.AddOverride) return;
    auto replaceOne = [&](DWORD idx) {
        if (handles.h[idx] && api.RemoveOverride) api.RemoveOverride(handles.h[idx]);
        handles.h[idx] = api.AddOverride(idx, &ov);
    };
    if (g_Config.broadcastToAllControllers) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) replaceOne(i);
    } else {
        replaceOne(g_Config.userIndex);
    }
}

static void ClearOverrideSet(HandleSet& handles) {
    const auto& api = Mad2XInput_Resolve();
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (handles.h[i] && api.RemoveOverride) api.RemoveOverride(handles.h[i]);
        handles.h[i] = 0;
    }
}

static HandleSet g_ReversalHandles;

// Picks 1-2 of the currently config-enabled axes at random and inverts
// just those -- NOT "invert every enabled axis every time" (that was the
// old behavior: with only the left stick enabled by default, it always
// looked identical every trigger). BOOL*, not bool*, to point directly at
// the Mad2XInputOverride fields being set.
static void WINAPI ApplyJoystickReversal(void*) {
    struct AxisOption {
        const char* name;
        bool enabled;
        BOOL* target;
    };

    Mad2XInputOverride ov{};
    AxisOption axes[4] = {
        {"left X", g_Config.invertLeftX, &ov.invertLX},
        {"left Y", g_Config.invertLeftY, &ov.invertLY},
        {"right X", g_Config.invertRightX, &ov.invertRX},
        {"right Y", g_Config.invertRightY, &ov.invertRY},
    };

    int eligible[4];
    int eligibleCount = 0;
    for (int i = 0; i < 4; ++i) {
        if (axes[i].enabled) eligible[eligibleCount++] = i;
    }

    char pickedNames[128] = "none";
    if (eligibleCount > 0) {
        int pickCount = (eligibleCount >= 2) ? (1 + (rand() % 2)) : 1;
        // Partial Fisher-Yates over `eligible` to pick pickCount distinct
        // axis indices without repeats.
        for (int i = 0; i < pickCount; ++i) {
            int j = i + (rand() % (eligibleCount - i));
            int tmp = eligible[i];
            eligible[i] = eligible[j];
            eligible[j] = tmp;
        }

        pickedNames[0] = '\0';
        for (int i = 0; i < pickCount; ++i) {
            AxisOption& axis = axes[eligible[i]];
            *axis.target = TRUE;
            if (i > 0) strncat(pickedNames, ", ", sizeof(pickedNames) - strlen(pickedNames) - 1);
            strncat(pickedNames, axis.name, sizeof(pickedNames) - strlen(pickedNames) - 1);
        }
    }

    ReplaceOverride(g_ReversalHandles, ov);

    char displayName[160];
    snprintf(displayName, sizeof(displayName), "Reversed Controls (%s)", pickedNames);
    if (const auto& effects = Mad2Effects_Resolve(); effects.SetDisplayName) {
        effects.SetDisplayName("JoystickReversal", displayName);
    }

    Log("JoystickReversal applied (%s)\n", pickedNames);
}

static void WINAPI ClearJoystickReversal(void*) {
    ClearOverrideSet(g_ReversalHandles);
    Log("JoystickReversal cleared\n");
}

// ---------------------------------------------------------------------
// StickPercentLimit.
// ---------------------------------------------------------------------

static HandleSet g_StickLimitHandles;

static int RandomIntInclusive(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (rand() % (hi - lo + 1));
}

static void WINAPI ApplyStickPercentLimit(void*) {
    int percent = RandomIntInclusive(g_Config.stickLimitMinPercent, g_Config.stickLimitMaxPercent);
    float scale = static_cast<float>(percent) / 100.0f;

    Mad2XInputOverride ov{};
    ov.enableScaleL = g_Config.stickLimitAffectLeft;
    ov.scaleL = scale;
    ov.enableScaleR = g_Config.stickLimitAffectRight;
    ov.scaleR = scale;
    ReplaceOverride(g_StickLimitHandles, ov);
    Log("StickPercentLimit applied (%d%%)\n", percent);
}

static void WINAPI ClearStickPercentLimit(void*) {
    ClearOverrideSet(g_StickLimitHandles);
    Log("StickPercentLimit cleared\n");
}

// ---------------------------------------------------------------------
// SwapAB / SwapXY.
// ---------------------------------------------------------------------

static HandleSet g_SwapABHandles;
static HandleSet g_SwapXYHandles;

static void WINAPI ApplySwapAB(void*) {
    Mad2XInputOverride ov{};
    ov.swapAB = TRUE;
    ReplaceOverride(g_SwapABHandles, ov);
    Log("SwapAB applied\n");
}
static void WINAPI ClearSwapAB(void*) {
    ClearOverrideSet(g_SwapABHandles);
    Log("SwapAB cleared\n");
}

static void WINAPI ApplySwapXY(void*) {
    Mad2XInputOverride ov{};
    ov.swapXY = TRUE;
    ReplaceOverride(g_SwapXYHandles, ov);
    Log("SwapXY applied\n");
}
static void WINAPI ClearSwapXY(void*) {
    ClearOverrideSet(g_SwapXYHandles);
    Log("SwapXY cleared\n");
}

// ---------------------------------------------------------------------
// Periodic button presses (RandomPressStart/A/B, PressY, PressX). One
// worker thread per active effect pulses the configured button (a brief
// AddOverride/RemoveOverride pair) at a randomized interval until Clear
// signals it to stop. A manual-reset event lets Clear interrupt the wait
// between presses immediately rather than waiting out the current
// interval.
// ---------------------------------------------------------------------

struct PeriodicPressConfig {
    const char* name;
    WORD button;
    int minIntervalMs, maxIntervalMs;
    HANDLE stopEvent = nullptr;
    HANDLE thread = nullptr;
};

static const int kPressPulseDurationMs = 150;  // how long each individual press is held

static PeriodicPressConfig g_PressStartCfg{"RandomPressStart", XINPUT_GAMEPAD_START, 0, 0};
static PeriodicPressConfig g_PressACfg{"RandomPressA", XINPUT_GAMEPAD_A, 0, 0};
static PeriodicPressConfig g_PressBCfg{"RandomPressB", XINPUT_GAMEPAD_B, 0, 0};
static PeriodicPressConfig g_PressYCfg{"PressY", XINPUT_GAMEPAD_Y, 0, 0};
static PeriodicPressConfig g_PressXCfg{"PressX", XINPUT_GAMEPAD_X, 0, 0};

static DWORD WINAPI PeriodicPressThreadFunc(LPVOID param) {
    auto* cfg = reinterpret_cast<PeriodicPressConfig*>(param);

    for (;;) {
        int waitMs = RandomIntInclusive(cfg->minIntervalMs, cfg->maxIntervalMs);
        if (WaitForSingleObject(cfg->stopEvent, waitMs) == WAIT_OBJECT_0) return 0;

        Mad2XInputOverride ov{};
        ov.forceHoldButtons = cfg->button;
        HandleSet pulseHandles;
        ReplaceOverride(pulseHandles, ov);  // broadcasts per Config::broadcastToAllControllers, same as every other effect
        bool stopped = WaitForSingleObject(cfg->stopEvent, kPressPulseDurationMs) == WAIT_OBJECT_0;
        ClearOverrideSet(pulseHandles);
        if (stopped) return 0;
    }
}

static void StopPeriodicPress(PeriodicPressConfig& cfg) {
    if (cfg.stopEvent) SetEvent(cfg.stopEvent);
    if (cfg.thread) {
        WaitForSingleObject(cfg.thread, 2000);
        CloseHandle(cfg.thread);
        cfg.thread = nullptr;
    }
    if (cfg.stopEvent) {
        CloseHandle(cfg.stopEvent);
        cfg.stopEvent = nullptr;
    }
}

static void StartPeriodicPress(PeriodicPressConfig& cfg, int minMs, int maxMs) {
    StopPeriodicPress(cfg);  // idempotent -- a re-trigger while active restarts cleanly, doesn't stack threads
    cfg.minIntervalMs = minMs;
    cfg.maxIntervalMs = maxMs;
    cfg.stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    cfg.thread = CreateThread(nullptr, 0, PeriodicPressThreadFunc, &cfg, 0, nullptr);
    Log("%s applied (interval=[%d,%d]ms)\n", cfg.name, minMs, maxMs);
}

static void WINAPI ApplyPressStart(void*) {
    StartPeriodicPress(g_PressStartCfg, g_Config.pressStartMinIntervalMs, g_Config.pressStartMaxIntervalMs);
}
static void WINAPI ClearPressStart(void*) {
    StopPeriodicPress(g_PressStartCfg);
    Log("RandomPressStart cleared\n");
}

static void WINAPI ApplyPressA(void*) {
    StartPeriodicPress(g_PressACfg, g_Config.pressAMinIntervalMs, g_Config.pressAMaxIntervalMs);
}
static void WINAPI ClearPressA(void*) {
    StopPeriodicPress(g_PressACfg);
    Log("RandomPressA cleared\n");
}

static void WINAPI ApplyPressB(void*) {
    StartPeriodicPress(g_PressBCfg, g_Config.pressBMinIntervalMs, g_Config.pressBMaxIntervalMs);
}
static void WINAPI ClearPressB(void*) {
    StopPeriodicPress(g_PressBCfg);
    Log("RandomPressB cleared\n");
}

static void WINAPI ApplyPressY(void*) { StartPeriodicPress(g_PressYCfg, g_Config.pressYIntervalMs, g_Config.pressYIntervalMs); }
static void WINAPI ClearPressY(void*) {
    StopPeriodicPress(g_PressYCfg);
    Log("PressY cleared\n");
}

static void WINAPI ApplyPressX(void*) {
    StartPeriodicPress(g_PressXCfg, g_Config.pressXMinIntervalMs, g_Config.pressXMaxIntervalMs);
}
static void WINAPI ClearPressX(void*) {
    StopPeriodicPress(g_PressXCfg);
    Log("PressX cleared\n");
}

// ---------------------------------------------------------------------
// StickDirectionDisable -- picks a stick + direction at random and blocks
// it. Explicitly cannot stack with itself: Apply always removes any
// previous instance's layer first (tracked in the single static handle
// below) before picking a new stick/direction and adding a fresh one, so
// a re-trigger while already active replaces the block rather than
// creating a second concurrent one.
// ---------------------------------------------------------------------

static HandleSet g_StickDisableHandles;

static void WINAPI ApplyStickDirectionDisable(void*) {
    static const WORD kDirections[2][4] = {
        {MAD2XINPUT_BLOCK_LEFT_UP, MAD2XINPUT_BLOCK_LEFT_DOWN, MAD2XINPUT_BLOCK_LEFT_LEFT, MAD2XINPUT_BLOCK_LEFT_RIGHT},
        {MAD2XINPUT_BLOCK_RIGHT_UP, MAD2XINPUT_BLOCK_RIGHT_DOWN, MAD2XINPUT_BLOCK_RIGHT_LEFT,
         MAD2XINPUT_BLOCK_RIGHT_RIGHT},
    };
    static const char* kStickNames[2] = {"left", "right"};
    static const char* kDirectionNames[4] = {"up", "down", "left", "right"};

    int stick = rand() % 2;
    int direction = rand() % 4;

    Mad2XInputOverride ov{};
    ov.blockStickDirections = kDirections[stick][direction];
    ReplaceOverride(g_StickDisableHandles, ov);

    // The HUD/log otherwise only shows the generic effect name -- with 8
    // possible stick+direction combinations, a player has no way to know
    // which one is actually blocked without this. Updates the live display
    // name only, doesn't touch active/duration state -- see
    // Mad2Effects_SetDisplayName's contract.
    char displayName[128];
    snprintf(displayName, sizeof(displayName), "Disabled Stick Direction (%s stick, %s)", kStickNames[stick],
              kDirectionNames[direction]);
    if (const auto& effects = Mad2Effects_Resolve(); effects.SetDisplayName) {
        effects.SetDisplayName("StickDirectionDisable", displayName);
    }

    Log("StickDirectionDisable applied (%s stick, %s)\n", kStickNames[stick], kDirectionNames[direction]);
}

static void WINAPI ClearStickDirectionDisable(void*) {
    ClearOverrideSet(g_StickDisableHandles);
    Log("StickDirectionDisable cleared\n");
}

// ---------------------------------------------------------------------
// Init-only background thread: retries until every dependency is
// resolvable, then registers every effect and exits.
// ---------------------------------------------------------------------

static void RegisterEffect(const char* name, const char* displayName, int weight, Mad2Effects_ApplyFn apply,
                            Mad2Effects_ClearFn clear, DWORD minDurationMs, DWORD maxDurationMs) {
    Mad2EffectDesc desc{};
    desc.name = name;
    desc.displayName = displayName;
    desc.defaultWeight = weight;
    desc.apply = apply;
    desc.clear = clear;
    desc.defaultMinDurationMs = minDurationMs;
    desc.defaultMaxDurationMs = maxDurationMs;
    BOOL ok = Mad2Effects_Resolve().Register(&desc);
    Log("Mad2Effects_Register(\"%s\") -> %d\n", name, ok);
}

static DWORD WINAPI InitThreadFunc(LPVOID) {
    const int kRetryDelayMs = 200;
    const int kWarnAfterAttempts = 25;  // ~5s

    int attempts = 0;
    bool warned = false;
    for (;;) {
        bool configOk = Mad2Config_Resolve().GetInt != nullptr;
        bool xinputOk = Mad2XInput_Resolve().AddOverride != nullptr && Mad2XInput_Resolve().RemoveOverride != nullptr;
        bool effectsOk = Mad2Effects_Resolve().Register != nullptr;
        if (configOk && xinputOk && effectsOk) break;

        ++attempts;
        if (!warned && attempts >= kWarnAfterAttempts) {
            warned = true;
            Log("[Dependency] still waiting on: %s%s%s (retrying indefinitely)\n", configOk ? "" : "mad2config ",
                xinputOk ? "" : "mad2xinput ", effectsOk ? "" : "mad2effects ");
        }
        Sleep(kRetryDelayMs);
    }

    LoadConfig();

    RegisterEffect("JoystickReversal", "Reversed Controls", g_Config.reversalWeight, ApplyJoystickReversal,
                    ClearJoystickReversal, g_Config.reversalMinDurationMs, g_Config.reversalMaxDurationMs);
    RegisterEffect("StickPercentLimit", "Limited Stick Range", g_Config.stickLimitWeight, ApplyStickPercentLimit,
                    ClearStickPercentLimit, g_Config.stickLimitMinDurationMs, g_Config.stickLimitMaxDurationMs);
    RegisterEffect("SwapAB", "Swapped A/B", g_Config.swapABWeight, ApplySwapAB, ClearSwapAB,
                    g_Config.swapABMinDurationMs, g_Config.swapABMaxDurationMs);
    RegisterEffect("SwapXY", "Swapped X/Y", g_Config.swapXYWeight, ApplySwapXY, ClearSwapXY,
                    g_Config.swapXYMinDurationMs, g_Config.swapXYMaxDurationMs);
    RegisterEffect("RandomPressStart", "Phantom Start Presses", g_Config.pressStartWeight, ApplyPressStart,
                    ClearPressStart, g_Config.pressStartDurationMs, g_Config.pressStartDurationMs);
    RegisterEffect("RandomPressA", "Phantom A Presses", g_Config.pressAWeight, ApplyPressA, ClearPressA,
                    g_Config.pressADurationMs, g_Config.pressADurationMs);
    RegisterEffect("RandomPressB", "Phantom B Presses", g_Config.pressBWeight, ApplyPressB, ClearPressB,
                    g_Config.pressBDurationMs, g_Config.pressBDurationMs);
    RegisterEffect("PressY", "Phantom Y Presses", g_Config.pressYWeight, ApplyPressY, ClearPressY,
                    g_Config.pressYDurationMs, g_Config.pressYDurationMs);
    RegisterEffect("PressX", "Phantom X Presses", g_Config.pressXWeight, ApplyPressX, ClearPressX,
                    g_Config.pressXDurationMs, g_Config.pressXDurationMs);
    RegisterEffect("StickDirectionDisable", "Disabled Stick Direction", g_Config.stickDisableWeight,
                    ApplyStickDirectionDisable, ClearStickDirectionDisable, g_Config.stickDisableDurationMs,
                    g_Config.stickDisableDurationMs);

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            srand(static_cast<unsigned>(time(nullptr)) ^ GetCurrentProcessId());
            HANDLE h = CreateThread(nullptr, 0, InitThreadFunc, nullptr, 0, nullptr);
            if (h) {
                CloseHandle(h);
            } else {
                Log("CreateThread for init thread failed: %lu\n", GetLastError());
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
