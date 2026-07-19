// Title-screen cheat-apply mod for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. While the player is on the
// title screen, pressing a configurable keyboard key (default V) or holding
// a configurable controller button combo (default BACK) writes the engine's
// built-in cheat toggles directly into game memory -- a straight in-process
// port of ../mad2mod/src/main.cpp's enable_all_cheats()/load_cheats_config(),
// which did the same thing from an external process via ptrace-style
// read_mem/write_mem. Being in-process, this mod just dereferences the same
// pointer chain directly instead.
//
// Pointer chain: AlchemyCommonLib.dll base + 0x535DC, dereferenced once,
// then walked through six more offsets (see kCheatStructOffsets below) to
// land on the engine's cheat-toggle struct; each cheat is then a fixed
// int32 offset from that struct's address, written 2 (on) or 0 (off) --
// exactly what ../mad2mod's enable_all_cheats wrote over IPC. The chain and
// every offset/name in kCheats below are taken verbatim from that tool's
// own (externally, already reverse-engineered) mapping -- not independently
// re-derived here, and what several of these names actually do in-game
// (e.g. "buddyPointer", "debugMode") is unverified beyond the name itself.
//
// Title-screen gating uses mad2leveldetect.h's shared current-level
// detection routine (also used by mad2gameeffectsmod) -- previously each
// mod carried its own verbatim ~40-line copy of this logic; see
// improvement-plan.md item 12.
//
// This mod has no D3D9 hook and doesn't register anything with
// mad2effects -- it's a standalone hotkey action, so it uses the same
// "init-only background thread" shape as mad2inputeffectsmod, except the
// thread here doesn't exit after setup: it keeps polling for the trigger
// key/combo for the lifetime of the process, same as mad2igttimer's
// per-frame reset-hotkey check just without a D3D9 hook to piggyback on.
#include <windows.h>
#include <xinput.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2levelredirectmod/include/mad2levelredirect_api.h"
#include "../../mad2xinput/include/mad2xinput_api.h"

#include "../../mad2leveldetect/mad2leveldetect.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2cheatapply]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Cheat table (config.cfg's [CheatApply] section).
// ---------------------------------------------------------------------

struct CheatDef {
    const char* key;      // config.cfg key -- matches ../mad2mod/config.txt's cheat_* names
    const char* display;  // clean name for logging -- matches mad2launcher's save-editor cheat
                           // list (saveeditor.go's cheatsList[].Name) so the same cheat reads
                           // the same everywhere in the toolchain
    const char* desc;     // short human-readable description, used verbatim as the config.cfg
                           // comment -- also taken from saveeditor.go's cheatsList[].Desc
    int32_t offset;       // offset from the resolved cheat-struct address
    bool enabled;         // loaded from config.cfg, default off
};

static CheatDef g_Cheats[] = {
    {"cheat_infiniteSprint", "Infinite Sprint", "Sprint without depleting energy", -0x08, false},
    {"cheat_superMangos", "Super Mangos", "Increases standard health limits", -0x60, false},
    {"cheat_headOfTheGame", "Head of the Game", "Enables bobble head scaling", -0x34, false},
    {"cheat_penguinProjectile", "Penguin Projectile", "Allows throwing penguin projectiles", 0x24, false},
    {"cheat_unlockLevels", "Unlock Levels", "Unlocks stage select levels", 0x50, false},
    {"cheat_buddyPointer", "Buddy Pointer", "Shows compass/arrow helpers", 0xA8, false},
    {"cheat_debugMode", "Debug Mode", "Developer debug logs/mode", 0xD4, false},
    {"cheat_fastMango", "Fast Mango", "Increases gameplay speed", 0x100, false},
    {"cheat_giantFrogs", "Giant Frogs", "Scales frog targets larger", 0x12C, false},
    {"cheat_superButtBounce", "Super Butt Bounce", "Improves ground pound velocity", 0x158, false},
    {"cheat_pepperAtWill", "Pepper at Will", "Marty gets infinite pepper sprints", 0x184, false},
    {"cheat_swimBackwards", "Swim Backwards", "Reverses swimming speed", 0x1B0, false},
    {"cheat_invulnAlex", "Invulnerable Alex", "Invincibility for Alex", 0x1DC, false},
    {"cheat_invulnGloria", "Invulnerable Gloria", "Invincibility for Gloria", 0x208, false},
    {"cheat_invulnMarty", "Invulnerable Marty", "Invincibility for Marty", 0x234, false},
    {"cheat_invisNewYork", "Invis New York", "Turns character invisible", 0x260, false},
    {"cheat_golfHole", "Golf Hole in One", "Easier golf physics", 0xEEC, false},
    {"cheat_ballStop", "Stop Ball", "Allows stopping ball instantly", 0xFC8, false},
};
static const size_t kCheatCount = sizeof(g_Cheats) / sizeof(g_Cheats[0]);

struct Config {
    int triggerVk = 'V';   // matches ../mad2mod's keyboard trigger
    WORD triggerCombo = 0;  // resolved from config; default "BACK" matches ../mad2mod's controller trigger
} g_Config;

static void LoadConfig() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetBool) {
        Log("[Dependency] mad2config.dll not found or missing exports -- all cheats default OFF, "
            "trigger key defaults to V, no controller trigger (place mad2config.dll in mad2/mods/)\n");
        return;
    }

    g_Config.triggerVk = api.GetVirtualKey(
        "CheatApply", "TriggerKey", g_Config.triggerVk,
        "Keyboard key that applies the cheats below, while on the title screen.\n"
        "Virtual-key code name with the VK_ prefix stripped (e.g. V), or a 0xNN hex code.\n"
        "See https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes");
    g_Config.triggerCombo = api.GetGamepadButtonMask(
        "CheatApply", "TriggerComboButtons", "BACK",
        "Comma-separated controller buttons that apply the cheats below, while on the title screen.\n"
        "XINPUT_GAMEPAD_* names with the prefix stripped (e.g. BACK). Leave empty to disable.\n"
        "See https://learn.microsoft.com/windows/win32/api/xinput/ns-xinput-xinput_gamepad");

    for (size_t i = 0; i < kCheatCount; ++i) {
        CheatDef& c = g_Cheats[i];
        c.enabled = api.GetBool("CheatApply", c.key, FALSE, c.desc) != FALSE;
    }

    Log("Config loaded: triggerVk=0x%X triggerCombo=0x%04X\n", g_Config.triggerVk, g_Config.triggerCombo);
}

// ---------------------------------------------------------------------
// Pointer-chain helpers (same technique as mad2gameeffectsmod/mad2playercoords).
// ---------------------------------------------------------------------

static bool IsPlausiblePointer(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

static uintptr_t FollowChain(uintptr_t base, const uintptr_t* offsets, size_t count) {
    if (!IsPlausiblePointer(base)) return 0;
    uintptr_t addr = *reinterpret_cast<uint32_t*>(base);
    for (size_t i = 0; i < count; ++i) {
        if (!IsPlausiblePointer(addr)) return 0;
        addr = *reinterpret_cast<uint32_t*>(addr + offsets[i]);
    }
    return addr;
}

// AlchemyCommonLib.dll base + 0x535DC, then this chain, lands on the
// engine's cheat-toggle struct -- see file header for provenance.
static const uintptr_t kCheatStructOffsets[] = {0x38, 0x1C, 0x14, 0x240, 0xA0, 0x164};

static uintptr_t ResolveCheatStructPtr() {
    HMODULE alchemyCommon = GetModuleHandleA("AlchemyCommonLib.dll");
    if (!alchemyCommon) return 0;
    return FollowChain(reinterpret_cast<uintptr_t>(alchemyCommon) + 0x000535DC, kCheatStructOffsets,
                        sizeof(kCheatStructOffsets) / sizeof(kCheatStructOffsets[0]));
}

// ---------------------------------------------------------------------
// Current level detection -- logic now lives in mad2leveldetect.h, shared
// with mad2gameeffectsmod (previously a verbatim ~40-line copy here). Only
// used to gate the trigger to the title screen; not exposed to anything
// else.
// ---------------------------------------------------------------------

static std::string g_CurrentLevel = "None";

static void UpdateCurrentLevel() { Mad2LevelDetect_UpdateCurrentLevel(g_CurrentLevel); }

static bool IsOnTitleScreen() { return g_CurrentLevel == "None" || g_CurrentLevel == "title"; }

// ---------------------------------------------------------------------
// Apply.
// ---------------------------------------------------------------------

static void ApplyCheats() {
    uintptr_t structAddr = ResolveCheatStructPtr();
    if (!IsPlausiblePointer(structAddr)) {
        Log("ApplyCheats: failed to resolve AlchemyCommonLib.dll cheat-struct pointer chain -- "
            "game may not be fully loaded yet\n");
        return;
    }
    Log("ApplyCheats: cheat struct resolved to 0x%08X\n", static_cast<unsigned int>(structAddr));

    int enabledCount = 0;
    for (size_t i = 0; i < kCheatCount; ++i) {
        const CheatDef& c = g_Cheats[i];
        int32_t val = c.enabled ? 2 : 0;
        *reinterpret_cast<int32_t*>(structAddr + c.offset) = val;
        if (c.enabled) ++enabledCount;
        Log("  %s = %s (offset 0x%X)\n", c.display, c.enabled ? "ON" : "OFF", c.offset);
    }
    Log("ApplyCheats: done, %d/%zu cheats enabled\n", enabledCount, kCheatCount);
}

// ---------------------------------------------------------------------
// Poll thread -- no D3D9 hook to piggyback on, so this just polls at a
// fixed interval for the lifetime of the process (unlike
// mad2inputeffectsmod's init-only thread, which exits after registering).
// ---------------------------------------------------------------------

static DWORD WINAPI PollThreadFunc(LPVOID) {
    const int kRetryDelayMs = 200;
    const int kWarnAfterAttempts = 25;  // ~5s
    int attempts = 0;
    bool warned = false;
    for (;;) {
        if (Mad2Config_Resolve().GetBool) break;
        ++attempts;
        if (!warned && attempts >= kWarnAfterAttempts) {
            warned = true;
            Log("[Dependency] still waiting on mad2config (retrying indefinitely)\n");
        }
        Sleep(kRetryDelayMs);
    }

    LoadConfig();
    Log("Ready -- press the configured key/controller combo on the title screen to apply cheats\n");

    bool wasKeyPressed = false;
    bool wasComboPressed = false;
    for (;;) {
        Sleep(50);

        UpdateCurrentLevel();

        bool isKeyPressed = (GetAsyncKeyState(g_Config.triggerVk) & 0x8000) != 0;

        bool isComboPressed = false;
        if (const auto& api = Mad2XInput_Resolve(); api.GetState) {
            XINPUT_STATE state{};
            if (api.GetState(0, &state) && g_Config.triggerCombo != 0) {
                isComboPressed = (state.Gamepad.wButtons & g_Config.triggerCombo) == g_Config.triggerCombo;
            }
        }

        bool triggered = (isKeyPressed && !wasKeyPressed) || (isComboPressed && !wasComboPressed);
        wasKeyPressed = isKeyPressed;
        wasComboPressed = isComboPressed;

        if (!triggered) continue;

        if (IsOnTitleScreen()) {
            Log("Trigger pressed on title screen (current='%s') -- applying cheats\n", g_CurrentLevel.c_str());
            ApplyCheats();
        } else {
            Log("Trigger pressed but not on title screen (current='%s') -- ignoring\n", g_CurrentLevel.c_str());
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            HANDLE h = CreateThread(nullptr, 0, PollThreadFunc, nullptr, 0, nullptr);
            if (h) {
                CloseHandle(h);
            } else {
                Log("CreateThread for poll thread failed: %lu\n", GetLastError());
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
