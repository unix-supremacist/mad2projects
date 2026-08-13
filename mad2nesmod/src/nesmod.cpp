// NES/FDS/N64 chaos effects for Madagascar 2 (mad2/mods/mad2nesmod.dll).
//
// Unlike mad2companionmod's SM64Challenge/JakChallenge/MarioLegacyChallenge
// (which spawn a whole separate emulator/engine process and stream
// controller state to it over loopback UDP -- see that mod's file header),
// this mod runs emulators directly INSIDE the Mad2 process: it
// LoadLibraryA's prebuilt libretro cores (fceumm for NES/FDS -- see
// extern/.libretro-cores/, fetched by `just fetch-libretro-fceumm` --  and
// mupen64plus_next for N64, fetched by `just fetch-libretro-mupen64plus`),
// implements the handful of libretro frontend callbacks by hand (no
// RetroArch, no separate frontend process), and drives retro_run() once per
// frame from this mod's own D3D9 EndScene hook -- same "IAT-hook
// Direct3DCreate9 -> vtable-patch CreateDevice -> vtable-patch EndScene"
// technique as mad2igttimer, via aa_mad2hookutil.dll (see
// mad2hookutil/include/mad2hookutil_api.h). The active core's
// video_refresh_cb writes straight into a D3D9 texture (locked/unlocked
// inline, on the render thread, since retro_run only ever runs from
// HookEndScene -- no cross-thread synchronization needed anywhere in this
// file), drawn as one full-screen opaque, nearest-neighbor-filtered quad
// (see DrawEmuQuadPointFiltered) that completely replaces Mad2's own frame
// for as long as an effect below is active.
//
// TWO independent cores, TWO+ games: NES/FDS games share the FCEUmm core
// (both Super Mario Bros. titles vendored here share an (almost)
// byte-identical engine -- Lost Levels' own SMB2J disassembly
// (threecreepio/smb2j-disassembly on GitHub, itself a ca65 port of
// doppelganger's original) defines every RAM address this file relies on
// at the EXACT same offsets as SMBDIS.ASM (SMB1's own disassembly) --
// fetched and grepped directly to confirm this, not assumed from "they're
// similar games"), while N64Sm64Challenge gets its own separate
// mupen64plus_next core instance -- libretro cores are global C API state
// per loaded module, so two different SYSTEMS can't share one core the way
// two NES games share FCEUmm. Both cores can be resident simultaneously
// (see Engine/g_Engines below); only whichever game is the currently
// ACTIVE mad2effects entry actually gets its retro_run() called and its
// frame drawn each tick. Within a single core, only one game can be
// resident at a time (retro_unload_game/retro_load_game to switch, see
// EnsureGameLoaded); across cores, each keeps its own independent
// loaded-game state, so switching from (say) NesChallenge to
// N64Sm64Challenge and back leaves the NES game exactly where it was
// paused, same as switching between the two NES games already did.
//
// N64Sm64Challenge is a SEPARATE effect from mad2companionmod's own
// SM64Challenge -- that one launches the vendored, natively-built
// decompiled PC port (extern/sm64ex-alo) as a whole separate process; this
// one instead emulates an actual N64 ROM dump (see roms/README.md) via
// mupen64plus_next running directly inside Mad2's own address space, same
// technique as the NES games.
//
// Renderer, config key [N64] Renderer, default "hardware": mupen64plus_
// next's default RDP plugin, GLideN64, needs a real OpenGL context --
// this mod bridges one in via a hidden, never-shown window purely to give
// WGL an HDC/pixel format to attach to (see the "N64 hardware rendering"
// section above EnsureWaveOut for the full picture -- EnvironmentCb's
// RETRO_ENVIRONMENT_SET_HW_RENDER handler, EnsureN64GlContext,
// EnsureN64Fbo). The core renders into our own offscreen FBO using GL
// calls of its own; VideoRefreshCb reads it back via glReadPixels
// (RETRO_HW_FRAME_BUFFER_VALID sentinel) and uploads it into the same
// D3D9 texture the software cores' raw pixel buffers already go through,
// so the rest of the pipeline (texture management, draw call, D3D9 state
// save/restore) is shared and unaware which renderer produced the frame.
// "software" instead forces the core's "mupen64plus-rdp-plugin" option to
// "angrylion" (a fully CPU-side software RDP implementation Windows
// builds always compile in) before it ever gets a chance to request a
// hardware render context at all -- a real speed/accuracy tradeoff
// (Angrylion is a slow, single-threaded interpreter) kept as a manual
// escape hatch since the hardware path bridges a genuinely new kind of
// context (OpenGL, alongside Mad2's own D3D9 device, under Proton/Wine)
// that's unproven across the range of GPUs/drivers this mod might run on
// -- flip to "software" if N64Sm64Challenge misbehaves (black screen,
// crash, visual corruption) under "hardware".
//
// N64's retro_run() is called SYNCHRONOUSLY from HookEndScene, same as
// NES's -- Mad2's own render thread genuinely stalls for the duration of
// each N64 frame, which is slow enough (Angrylion) to be perceptible. A
// background-thread attempt (retro_run() driven from a dedicated worker
// thread, decoupling Mad2's frame rate from N64's) was tried and reverted
// after three separate live boots crashed Mad2.exe reliably, every time,
// right after the first cross-thread retro_run() call -- confirmed via
// mad2.log that the crash landed immediately after "N64Sm64Challenge
// applied" every single time, not randomly, and confirmed the crash
// disappeared the instant retro_run() moved back onto the render thread.
// Root cause not confirmed (would need a proper Windows-side crash dump,
// not just process-exit evidence), but the leading theory is that
// mupen64plus_next's own internal "EmuThread" command-dispatch
// architecture (visible in its own log lines -- "[EmuThread]
// M64CMD_ROM_OPEN", "M64CMD_EXECUTE" -- straight out of mupen64plus-core's
// classic main_run() design) isn't safe to drive via retro_run() from an
// arbitrary calling thread, unlike most libretro cores. Moving Mad2's own
// perceived stutter elsewhere (e.g. running retro_run() less than once per
// Mad2 frame when N64 is behind, still synchronously on the render
// thread, rather than on a second thread at all) is a follow-up worth
// trying before attempting cross-thread execution again.
//
// Registered with mad2effects (mad2effects_api.h) exactly like SM64Challenge:
// no auto-revert duration (both duration fields 0, but a Clear callback is
// set) -- stays active until explicitly cleared. Since there's no child
// process to watch exit (everything lives in this DLL's own address space),
// there's no watcher-thread self-clear path either: Clear just flips
// g_Active off and un-blocks Mad2's input, but deliberately does NOT call
// retro_unload_game/retro_deinit -- each core and its loaded ROM stay
// resident so a re-trigger of the SAME game resumes exactly where the
// player left off, rather than rebooting every time (SM64/Jak1/MarioLegacy
// can't do this -- their state lives in a separate process that gets
// killed on Clear -- but an in-process core has no such constraint).
//
// Input is read directly from mad2xinput's RAW state (same lesson
// SM64Challenge/JakChallenge already learned the hard way, see
// sm64mod.cpp's ReadEffectiveOverriddenState comment) with a fixed button
// mapping per system (N64's mapping was verified against
// mupen64plus_next's own default RetroPad-to-N64-pad translation, custom/
// mupen64plus-core/plugin/emulate_game_controller_via_libretro.c in
// libretro/mupen64plus-libretro-nx -- NOT guessed), and audio goes out via
// a small best-effort winmm waveOut ring buffer, one instance per engine.
// No save-states, no rewind, no per-core configuration UI. N64Sm64Challenge
// has none of the NES games' RAM-level tricks (MuteMusic, RandomLevelStart,
// AutoStopOnLevelComplete, FDS disk-swap auto-answer) -- those are all
// SMB1/Lost-Levels-specific RAM pokes verified against those games'
// disassemblies and don't generalize to an arbitrary N64 title.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <xinput.h>
#include <mmsystem.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2xinput/include/mad2xinput_api.h"
#include "../../mad2effects/include/mad2effects_api.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2textrenderer/include/mad2textrenderer_api.h"
#include "../../mad2settings/include/mad2settings_api.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

#define RETRO_IMPORT_SYMBOLS  // see libretro.h -- these functions live in a
                               // DLL we LoadLibraryA/GetProcAddress ourselves,
                               // never link against, so declare them
                               // dllimport (never referenced directly either
                               // way -- only via our own typedefs below --
                               // this just keeps the header's own RETRO_API
                               // declarations from trying to dllexport
                               // "retro_init" etc. out of THIS DLL).
#include "../include/libretro.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2nesmod]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Which of the two libretro cores/engines (see Engine below) a game
// belongs to. Two NES-family games share one FCEUmm core instance; the N64
// game gets its own separate mupen64plus_next instance -- see file header.
// ---------------------------------------------------------------------

enum class System { Nes, N64 };

// N64Sm64Challenge's RDP renderer -- see the "N64 hardware rendering"
// section below for the full picture. Hardware (GLideN64/OpenGL) is the
// default; Software (Angrylion, the only path that existed before this
// was added) is a manual escape hatch via [N64] Renderer=software if the
// GL path misbehaves.
enum class N64Renderer { Hardware, Software };

// ---------------------------------------------------------------------
// Game catalog -- one entry per mad2effects effect this mod registers.
// Compile-time fields describe each title; the trailing runtime fields are
// filled in by EnsureConfigLoaded (romPath/testTriggerVk/weight are all
// individually configurable per game, under configSection -- "Nes" for the
// two NES-family games (sharing that section since mad2config's format has
// no per-game sub-sections -- see the *ConfigKey fields, which just carry a
// distinct key name per game within it) and "N64" for the N64 game, kept
// separate so N64-only settings don't clutter the NES section.
// ---------------------------------------------------------------------

struct EmuGame {
    System system;
    const char* effectName;
    const char* displayName;
    const char* configSection;
    const char* romPathConfigKey;
    const char* defaultRomPath;
    bool isFds;  // Famicom Disk System image -- needs nescore\disksys.rom (see roms/README.md). NES-only.
    const char* testTriggerConfigKey;
    int defaultTestTriggerVk;
    const char* weightConfigKey;
    int defaultWeight;
    // Worlds available to ApplyRandomLevelStart's random pick, numbered
    // from World 1. Fixed at 8 for both games -- Lost Levels' World 9 is
    // PINNED OUT for now (worldCount deliberately excludes it, rather than
    // being 9): live-tested both with and without HardWorldFlag forced on,
    // and both black-screen despite the RAM-traced state machine
    // completing the exact same "load finished, screen re-enabled"
    // sequence Worlds 1-8 complete successfully. Since it fails identically
    // whether HardWorldFlag is involved or not, the problem isn't specific
    // to "Worlds A-D" as first suspected -- something about World 9 itself
    // (its level data, or some other state a legitimate 8-world
    // playthrough would have set up that a direct warp skips) is broken,
    // and RAM-state tracing alone can't see far enough to say what.
    // Worth revisiting with more direct visibility into the actual
    // rendered output than this mod's logs can give -- the machinery below
    // (ApplyFdsDiskSwapAutoAnswer, TraceFdsStateTransitions, the
    // HardWorldFlag-forcing timing fix) is left in place for whoever picks
    // this back up, not reverted.
    int worldCount;
    // Whether World 9 (index worldCount, i.e. ONE PAST the current pick
    // range while it's pinned out above) can also be played as one of Lost
    // Levels' harder "Worlds A-D" repeat-loops -- see
    // Config::includeHardWorlds and ApplyRandomLevelStart's own comment.
    // False for both games right now: SMB1 has no such concept at all, and
    // Lost Levels' is parked alongside World 9 itself per the above.
    bool supportsHardWorlds;

    char romPath[MAX_PATH] = "";
    int testTriggerVk = 0;
    int weight = 0;
};

static EmuGame g_Games[] = {
    {System::Nes, "NesChallenge", "Super Mario Bros. Challenge", "Nes", "RomPath", "nescore\\smb1.nes", false,
     "TestTriggerKey", VK_F8, "Weight", 5, 8, false},
    {System::Nes, "NesLostLevelsChallenge", "Super Mario Bros: The Lost Levels Challenge", "Nes", "Smb2RomPath",
     "nescore\\smb2j.fds", true, "Smb2TestTriggerKey", VK_F5, "Smb2Weight", 5, 8, false},
    // mupen64plus_next, forced into pure-software (Angrylion) rendering --
    // see file header. Own core, own [N64] config section; worldCount/
    // supportsHardWorlds are meaningless outside the NES RAM-poke code
    // paths, left at 0/false.
    {System::N64, "N64Sm64Challenge", "Super Mario 64 (N64, Emulated) Challenge", "N64", "RomPath",
     "n64core\\sm64.z64", false, "TestTriggerKey", 'N', "Weight", 5, 0, false},
};
static const int kGameCount = sizeof(g_Games) / sizeof(g_Games[0]);

// ---------------------------------------------------------------------
// Config -- [Nes] settings shared across the two NES-family games above,
// plus a separate [N64] block for N64Sm64Challenge (own core, own core
// path/controller settings, none of the NES RAM-poke options apply to it).
// TestClearKey/RefreshGameKey are the only keys shared across BOTH systems
// -- both operate on "whichever game is currently the active mad2effects
// entry", already system-agnostic (see PollTestKeys/ForceReloadCurrentGame).
// ---------------------------------------------------------------------

struct Config {
    char corePath[MAX_PATH] = "nescore\\fceumm_libretro.dll";
    DWORD userIndex = 0;
    bool broadcastToAllControllers = true;
    int testClearVk = VK_F7;        // stops whichever game is active (pauses -- core/ROM stay resident)
    int muteMusicToggleVk = VK_F6;  // live-toggles MuteMusic
    int refreshGameVk = 'R';        // force-reloads the currently ACTIVE game (fresh RandomLevelStart re-roll)
    bool muteMusic = false;
    bool autoStopOnLevelComplete = true;
    bool randomLevelStart = true;
    bool includeHardWorlds = false;
    int debugForceWorld = 0;   // 1-9, 0 = disabled (use RandomLevelStart's own RNG)
    int debugForceLevel = 0;   // 1-4, 0 = disabled (random)
    bool debugForceHardWorld = false;

    // [N64] -- N64Sm64Challenge only.
    char n64CorePath[MAX_PATH] = "n64core\\mupen64plus_next_libretro.dll";
    DWORD n64UserIndex = 0;
    bool n64BroadcastToAllControllers = true;
    N64Renderer n64Renderer = N64Renderer::Hardware;
} g_Config;

static bool g_ConfigLoaded = false;

static void EnsureConfigLoaded() {
    if (g_ConfigLoaded) return;
    g_ConfigLoaded = true;

    const auto& api = Mad2Config_Resolve();
    if (!api.GetInt) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in defaults\n");
        return;
    }

    char buf[MAX_PATH];
    api.GetString("Nes", "CorePath", g_Config.corePath,
                  "Path (relative to the game exe dir) to the libretro core DLL to load.\n"
                  "See `just fetch-libretro-fceumm` -- deployed to nescore\\fceumm_libretro.dll by default.",
                  buf, sizeof(buf));
    strncpy(g_Config.corePath, buf, sizeof(g_Config.corePath) - 1);
    g_Config.corePath[sizeof(g_Config.corePath) - 1] = 0;

    g_Config.userIndex = static_cast<DWORD>(api.GetInt(
        "Nes", "UserIndex", static_cast<int>(g_Config.userIndex),
        "Which controller (XInput user index, 0-3) drives the NES core when\n"
        "BroadcastToAllControllers is false."));
    g_Config.broadcastToAllControllers = api.GetBool(
        "Nes", "BroadcastToAllControllers", g_Config.broadcastToAllControllers ? TRUE : FALSE,
        "Block Mad2's input on all 4 possible XInput slots (not just UserIndex) while\n"
        "a NES effect is active -- same Steam-Input-multi-slot rationale as\n"
        "[InputEffects] BroadcastToAllControllers.") != FALSE;
    g_Config.testClearVk = api.GetVirtualKey(
        "Nes", "TestClearKey", g_Config.testClearVk,
        "Keyboard key that stops whichever NES effect is currently active (pausing --\n"
        "the core/ROM stay loaded, a later re-trigger resumes). Virtual-key code name\n"
        "with the VK_ prefix stripped (e.g. F7), or a 0xNN hex code. Set to an unused\n"
        "code (e.g. 0) to disable.\n"
        "See https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes");
    g_Config.muteMusicToggleVk = api.GetVirtualKey(
        "Nes", "MuteMusicToggleKey", g_Config.muteMusicToggleVk,
        "Keyboard key that live-toggles MuteMusic below. Same key-name format as TestClearKey.");
    g_Config.refreshGameVk = api.GetVirtualKey(
        "Nes", "RefreshGameKey", g_Config.refreshGameVk,
        "Keyboard key that force-reloads whichever game is currently loaded (unload +\n"
        "load fresh, same as a full Mad2 restart would give it) and activates it --\n"
        "unlike TestClearKey/TestTriggerKey's pause/resume, this always re-rolls\n"
        "RandomLevelStart/IncludeHardWorlds rather than resuming where you left off.\n"
        "For quickly re-testing those without restarting Mad2. Same key-name format as\n"
        "TestClearKey -- defaults to the letter R (not an F-key) specifically to avoid\n"
        "colliding with any of this repo's existing F1-F12 hotkeys (every single one is\n"
        "already spoken for by some other mod, several of them dev-only diagnostics\n"
        "that fire unconditionally on any keypress regardless of what you're doing --\n"
        "confirmed live: an earlier F4 default double-fired mad2metadumper's full\n"
        "reflection dump alongside this key every press).");
    g_Config.muteMusic = api.GetBool(
        "Nes", "MuteMusic", g_Config.muteMusic ? TRUE : FALSE,
        "Silence the game's music engine while sound effects (jump, coin, stomp, etc.)\n"
        "keep playing normally -- forces the game's own \"Silence\" music track into its\n"
        "event music queue every frame rather than touching the sound-effect queues at\n"
        "all. See MuteMusicToggleKey to flip this live without editing config.cfg.") != FALSE;
    g_Config.autoStopOnLevelComplete = api.GetBool(
        "Nes", "AutoStopOnLevelComplete", g_Config.autoStopOnLevelComplete ? TRUE : FALSE,
        "Automatically stop the active NES effect (same as pressing TestClearKey) the\n"
        "moment the current level is beaten, instead of continuing on into the next one.") !=
        FALSE;
    g_Config.randomLevelStart = api.GetBool(
        "Nes", "RandomLevelStart", g_Config.randomLevelStart ? TRUE : FALSE,
        "Start at a random world+level each time a game is freshly (re)loaded into the\n"
        "core, instead of always World 1-1. Verified against both games' own\n"
        "disassemblies -- see ApplyRandomLevelStart's comment in src/nesmod.cpp for\n"
        "exactly what's relied on. Worlds 1-8 for both games -- Lost Levels' World 9\n"
        "(and IncludeHardWorlds' \"Worlds A-D\" below) is currently pinned out of the pool\n"
        "entirely; see EmuGame::worldCount's own comment for why.") != FALSE;
    g_Config.includeHardWorlds = api.GetBool(
        "Nes", "IncludeHardWorlds", g_Config.includeHardWorlds ? TRUE : FALSE,
        "PARKED/KNOWN BROKEN, currently inert either way -- see EmuGame::worldCount's\n"
        "comment in src/nesmod.cpp. Was meant to let RandomLevelStart, when it picked\n"
        "Lost Levels' World 9, also flip a coin to play it as one of the harder\n"
        "\"Worlds A-D\" repeat-loops instead (HardWorldFlag forced on). Live-tested both\n"
        "with and without HardWorldFlag involved: both black-screen identically despite\n"
        "the RAM-traced state machine completing normally, so the problem turned out to\n"
        "be World 9 itself, not this flag -- World 9 is unreachable via the normal pool\n"
        "right now regardless of this setting.") != FALSE;
    g_Config.debugForceWorld = api.GetInt(
        "Nes", "DebugForceWorld", g_Config.debugForceWorld,
        "DEBUG/TESTING. 1-8: skip RandomLevelStart's own RNG and always start at this\n"
        "exact world instead (clamped to whatever the loaded game's own world count\n"
        "is -- see EmuGame::worldCount, currently 8 for both games while Lost Levels'\n"
        "World 9 is parked). 0 disables this override (default RNG behavior). Combine\n"
        "with RefreshGameKey to quickly re-test one specific spot.");
    g_Config.debugForceLevel = api.GetInt(
        "Nes", "DebugForceLevel", g_Config.debugForceLevel,
        "DEBUG/TESTING. 1-4: companion to DebugForceWorld above, forces the level\n"
        "within that world too. 0 disables (random 1-4). Ignored if DebugForceWorld is 0.");
    g_Config.debugForceHardWorld = api.GetBool(
        "Nes", "DebugForceHardWorld", g_Config.debugForceHardWorld ? TRUE : FALSE,
        "DEBUG/TESTING, currently inert -- see IncludeHardWorlds above. Was meant to\n"
        "force HardWorldFlag on whenever DebugForceWorld targets Lost Levels' last\n"
        "world, for deterministic hard-world testing without waiting on RNG.") != FALSE;

    api.GetString("N64", "CorePath", g_Config.n64CorePath,
                  "Path (relative to the game exe dir) to the N64 libretro core DLL to load.\n"
                  "See `just fetch-libretro-mupen64plus` -- deployed to\n"
                  "n64core\\mupen64plus_next_libretro.dll by default.",
                  buf, sizeof(buf));
    strncpy(g_Config.n64CorePath, buf, sizeof(g_Config.n64CorePath) - 1);
    g_Config.n64CorePath[sizeof(g_Config.n64CorePath) - 1] = 0;

    g_Config.n64UserIndex = static_cast<DWORD>(api.GetInt(
        "N64", "UserIndex", static_cast<int>(g_Config.n64UserIndex),
        "Which controller (XInput user index, 0-3) drives the N64 core when\n"
        "BroadcastToAllControllers is false."));
    g_Config.n64BroadcastToAllControllers = api.GetBool(
        "N64", "BroadcastToAllControllers", g_Config.n64BroadcastToAllControllers ? TRUE : FALSE,
        "Block Mad2's input on all 4 possible XInput slots (not just UserIndex) while\n"
        "N64Sm64Challenge is active -- same rationale as [Nes] BroadcastToAllControllers.") != FALSE;
    api.GetString("N64", "Renderer", "hardware",
                  "\"hardware\" (GLideN64, OpenGL -- default) or \"software\" (Angrylion, pure CPU).\n"
                  "Hardware is dramatically faster but bridges an OpenGL context into this\n"
                  "already-D3D9-hooked process, unproven across every possible GPU/driver combo --\n"
                  "switch to \"software\" if N64Sm64Challenge misbehaves (black screen, crash, visual\n"
                  "corruption) and it doesn't with this set to \"software\".",
                  buf, sizeof(buf));
    g_Config.n64Renderer = (_stricmp(buf, "software") == 0) ? N64Renderer::Software : N64Renderer::Hardware;

    for (int i = 0; i < kGameCount; ++i) {
        EmuGame& game = g_Games[i];
        char comment[256];

        snprintf(comment, sizeof(comment),
                 "Path (relative to the game exe dir) to the ROM loaded for \"%s\".\nSee roms/README.md.",
                 game.displayName);
        api.GetString(game.configSection, game.romPathConfigKey, game.defaultRomPath, comment, buf, sizeof(buf));
        snprintf(game.romPath, sizeof(game.romPath), "%s", buf);

        snprintf(comment, sizeof(comment),
                 "Keyboard key that immediately starts/resumes \"%s\" on demand, bypassing\n"
                 "mad2chaosmod's weighted RNG. Same key-name format as TestClearKey.",
                 game.displayName);
        game.testTriggerVk =
            api.GetVirtualKey(game.configSection, game.testTriggerConfigKey, game.defaultTestTriggerVk, comment);

        snprintf(comment, sizeof(comment), "\"%s\": chance-pool weight.", game.displayName);
        game.weight = api.GetInt(game.configSection, game.weightConfigKey, game.defaultWeight, comment);
    }

    Log("Config loaded (corePath=%s userIndex=%lu n64CorePath=%s n64UserIndex=%lu testClearVk=0x%X muteMusic=%d "
        "autoStopOnLevelComplete=%d randomLevelStart=%d)\n",
        g_Config.corePath, g_Config.userIndex, g_Config.n64CorePath, g_Config.n64UserIndex, g_Config.testClearVk,
        g_Config.muteMusic, g_Config.autoStopOnLevelComplete, g_Config.randomLevelStart);
    for (int i = 0; i < kGameCount; ++i) {
        Log("  Game[%d] %s: system=%d romPath=%s testTriggerVk=0x%X weight=%d isFds=%d\n", i, g_Games[i].effectName,
            static_cast<int>(g_Games[i].system), g_Games[i].romPath, g_Games[i].testTriggerVk, g_Games[i].weight,
            g_Games[i].isFds);
    }
}

// ---------------------------------------------------------------------
// libretro core loading -- LoadLibraryA + GetProcAddress by name (plain
// cdecl C exports, confirmed via objdump against the vendored core: no
// "@N" stdcall decoration, so no -Wl,--kill-at concern here -- that's only
// needed for mods that themselves export a shared API, see
// mad2hookutil_api.h).
// ---------------------------------------------------------------------

typedef unsigned(RETRO_CALLCONV* retro_api_version_t)(void);
typedef void(RETRO_CALLCONV* retro_init_t)(void);
typedef void(RETRO_CALLCONV* retro_deinit_t)(void);
typedef void(RETRO_CALLCONV* retro_get_system_info_t)(struct retro_system_info*);
typedef void(RETRO_CALLCONV* retro_get_system_av_info_t)(struct retro_system_av_info*);
typedef void(RETRO_CALLCONV* retro_set_environment_t)(retro_environment_t);
typedef void(RETRO_CALLCONV* retro_set_video_refresh_t)(retro_video_refresh_t);
typedef void(RETRO_CALLCONV* retro_set_audio_sample_t)(retro_audio_sample_t);
typedef void(RETRO_CALLCONV* retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t);
typedef void(RETRO_CALLCONV* retro_set_input_poll_t)(retro_input_poll_t);
typedef void(RETRO_CALLCONV* retro_set_input_state_t)(retro_input_state_t);
typedef void(RETRO_CALLCONV* retro_set_controller_port_device_t)(unsigned, unsigned);
typedef void(RETRO_CALLCONV* retro_reset_t)(void);
typedef void(RETRO_CALLCONV* retro_run_t)(void);
typedef bool(RETRO_CALLCONV* retro_load_game_t)(const struct retro_game_info*);
typedef void(RETRO_CALLCONV* retro_unload_game_t)(void);
typedef void*(RETRO_CALLCONV* retro_get_memory_data_t)(unsigned);
typedef size_t(RETRO_CALLCONV* retro_get_memory_size_t)(unsigned);

struct CoreApi {
    retro_api_version_t api_version;
    retro_init_t init;
    retro_deinit_t deinit;
    retro_get_system_info_t get_system_info;
    retro_get_system_av_info_t get_system_av_info;
    retro_set_environment_t set_environment;
    retro_set_video_refresh_t set_video_refresh;
    retro_set_audio_sample_t set_audio_sample;
    retro_set_audio_sample_batch_t set_audio_sample_batch;
    retro_set_input_poll_t set_input_poll;
    retro_set_input_state_t set_input_state;
    retro_set_controller_port_device_t set_controller_port_device;
    retro_reset_t reset;
    retro_run_t run;
    retro_load_game_t load_game;
    retro_unload_game_t unload_game;
    retro_get_memory_data_t get_memory_data;
    retro_get_memory_size_t get_memory_size;
};

static const int kWaveBufferCount = 6;
static const int kWaveBufferCapacitySamples = 8192;  // interleaved stereo int16 samples (4096 frames)

// One Engine per SYSTEM (not per game) -- see file header. Each owns its own
// core module/API table, its own loaded-game state (so a NES game and the
// N64 game can each sit paused independently), and its own waveOut audio
// device (opened at whatever sample rate that engine's loaded game reports;
// the two engines are never audible simultaneously anyway, since only one
// g_ActiveGameIndex exists at a time, but keeping them separate avoids
// having to reopen/reconfigure waveOut on every switch).
struct Engine {
    explicit Engine(System k) : kind(k) {}
    System kind;
    HMODULE module = nullptr;
    CoreApi api{};
    retro_system_info systemInfo{};
    retro_system_av_info avInfo{};
    retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;  // libretro's documented default
    bool coreLoaded = false;
    bool gameLoaded = false;
    int loadedGameIndex = -1;  // which g_Games[] entry is currently resident in this engine's core, -1 = none
    // Kept alive for the process lifetime once loaded (tiny -- these ROMs
    // are at most tens of MB) rather than freed right after retro_load_game
    // returns; simplest correct choice since retro_load_game's own contract
    // doesn't guarantee it's done copying by the time the call returns.
    std::vector<uint8_t> romBytes;

    HWAVEOUT waveOut = nullptr;
    WAVEHDR waveHdrs[kWaveBufferCount]{};
    std::vector<int16_t> waveBufData[kWaveBufferCount];
    int waveNextBuf = 0;
    double waveOpenSampleRate = 0.0;
    std::vector<int16_t> pendingAudio;  // accumulated by audio_sample*_cb, flushed after retro_run()
};
static Engine g_Engines[2] = {Engine(System::Nes), Engine(System::N64)};
static Engine& EngineFor(System s) { return g_Engines[static_cast<int>(s)]; }

// Which Engine the libretro call currently unwinding on this thread belongs
// to -- set before every retro_init/retro_load_game/retro_run call (see
// EnsureCoreLoaded/EnsureGameLoaded/RunAndDrawFrame below) so the shared
// libretro callback functions (EnvironmentCb, VideoRefreshCb,
// AudioSample*Cb, InputStateCb) know which engine's state to read/mutate.
// Safe as a single global rather than a per-call parameter because libretro
// calls are always synchronous and single-threaded from this mod's own
// perspective -- retro_run for one engine never overlaps a callback from
// the other.
static Engine* g_CallbackEngine = nullptr;

// ---------------------------------------------------------------------
// D3D9: the device + backbuffer size captured at CreateDevice time (see
// InstallHooks below), and the texture whichever engine is currently
// ACTIVE writes into via its video_refresh_cb every retro_run() call. One
// shared texture is enough since only one engine ever draws in a given
// frame (see g_Active/g_ActiveGameIndex).
// ---------------------------------------------------------------------

static IDirect3DDevice9* g_Device = nullptr;
static UINT g_ScreenW = 0, g_ScreenH = 0;
static IDirect3DTexture9* g_EmuTexture = nullptr;
static int g_TexW = 0, g_TexH = 0;
static D3DFORMAT g_TexFmt = D3DFMT_UNKNOWN;

static D3DFORMAT PixelFormatToD3D(retro_pixel_format fmt) {
    switch (fmt) {
        case RETRO_PIXEL_FORMAT_0RGB1555: return D3DFMT_X1R5G5B5;
        case RETRO_PIXEL_FORMAT_XRGB8888: return D3DFMT_X8R8G8B8;
        case RETRO_PIXEL_FORMAT_RGB565: return D3DFMT_R5G6B5;
        default: return D3DFMT_UNKNOWN;  // 2101010/HDR10 -- not supported, see file header
    }
}

static bool EnsureEmuTexture(int width, int height, D3DFORMAT fmt) {
    if (!g_Device || fmt == D3DFMT_UNKNOWN) return false;
    if (g_EmuTexture && g_TexW == width && g_TexH == height && g_TexFmt == fmt) return true;

    if (g_EmuTexture) {
        g_EmuTexture->Release();
        g_EmuTexture = nullptr;
    }
    HRESULT hr = g_Device->CreateTexture(width, height, 1, 0, fmt, D3DPOOL_MANAGED, &g_EmuTexture, nullptr);
    if (FAILED(hr)) {
        Log("CreateTexture(%dx%d, fmt=%d) failed: hr=0x%08lX\n", width, height, static_cast<int>(fmt),
            static_cast<unsigned long>(hr));
        g_TexW = g_TexH = 0;
        g_TexFmt = D3DFMT_UNKNOWN;
        return false;
    }
    g_TexW = width;
    g_TexH = height;
    g_TexFmt = fmt;
    Log("Emulator texture (re)created: %dx%d fmt=%d\n", width, height, static_cast<int>(fmt));
    return true;
}

// ---------------------------------------------------------------------
// N64 hardware rendering (GLideN64, OpenGL) -- see file header for the
// tradeoff/history writeup. mupen64plus_next's GL bridging goes through
// glsm (libretro-common/glsm/glsm.c, a shared helper many GL-based
// libretro cores use), whose own glsm_state_ctx_init (fetched and read
// directly, not guessed) requests a PLAIN RETRO_HW_CONTEXT_OPENGL context
// -- not a versioned core-profile one -- on any build where its own
// "CORE" macro isn't defined, which is the case for this exact 32-bit
// Windows buildbot build (confirmed against the core's Makefile: no bare
// "CORE" define anywhere in it). That means a single old-school
// wglCreateContext(hdc) call is enough -- no WGL_ARB_create_context
// version/profile negotiation, no dummy-context-to-fetch-extensions
// bootstrap. GL functions beyond 1.1 (MinGW's own <GL/gl.h> only declares
// up to 1.1) are resolved by hand via wglGetProcAddress, falling back to
// GetProcAddress(opengl32.dll, ...) for the true 1.1 entry points
// wglGetProcAddress isn't guaranteed to return -- the standard Windows
// OpenGL loader pattern.
//
// The context is bound to a hidden, never-shown window purely so WGL has
// an HDC/pixel format to attach to -- it's never presented; every actual
// frame is read back via glReadPixels from our own offscreen FBO (sized
// from the core's reported max_width/max_height, created once at load
// time -- N64 games changing resolution mid-session isn't handled, a
// deliberate simplification) and uploaded into g_EmuTexture exactly like
// the software cores' pixel buffers, so the rest of the pipeline (texture
// management, DrawEmuQuadPointFiltered, D3D9 state save/restore) is
// unchanged and shared between all three games regardless of renderer.
// ---------------------------------------------------------------------
#include <GL/gl.h>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

typedef void(APIENTRY* PFNGLGENFRAMEBUFFERS_)(GLsizei, GLuint*);
typedef void(APIENTRY* PFNGLDELETEFRAMEBUFFERS_)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFNGLBINDFRAMEBUFFER_)(GLenum, GLuint);
typedef void(APIENTRY* PFNGLFRAMEBUFFERTEXTURE2D_)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void(APIENTRY* PFNGLGENRENDERBUFFERS_)(GLsizei, GLuint*);
typedef void(APIENTRY* PFNGLDELETERENDERBUFFERS_)(GLsizei, const GLuint*);
typedef void(APIENTRY* PFNGLBINDRENDERBUFFER_)(GLenum, GLuint);
typedef void(APIENTRY* PFNGLRENDERBUFFERSTORAGE_)(GLenum, GLenum, GLsizei, GLsizei);
typedef void(APIENTRY* PFNGLFRAMEBUFFERRENDERBUFFER_)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum(APIENTRY* PFNGLCHECKFRAMEBUFFERSTATUS_)(GLenum);

struct N64GlState {
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC hglrc = nullptr;
    bool contextReady = false;

    PFNGLGENFRAMEBUFFERS_ glGenFramebuffers = nullptr;
    PFNGLDELETEFRAMEBUFFERS_ glDeleteFramebuffers = nullptr;
    PFNGLBINDFRAMEBUFFER_ glBindFramebuffer = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2D_ glFramebufferTexture2D = nullptr;
    PFNGLGENRENDERBUFFERS_ glGenRenderbuffers = nullptr;
    PFNGLDELETERENDERBUFFERS_ glDeleteRenderbuffers = nullptr;
    PFNGLBINDRENDERBUFFER_ glBindRenderbuffer = nullptr;
    PFNGLRENDERBUFFERSTORAGE_ glRenderbufferStorage = nullptr;
    PFNGLFRAMEBUFFERRENDERBUFFER_ glFramebufferRenderbuffer = nullptr;
    PFNGLCHECKFRAMEBUFFERSTATUS_ glCheckFramebufferStatus = nullptr;

    GLuint fbo = 0, colorTex = 0, depthRb = 0;
    int fboW = 0, fboH = 0;

    retro_hw_context_reset_t contextReset = nullptr;
    retro_hw_context_reset_t contextDestroy = nullptr;
};
static N64GlState g_N64Gl;
static bool g_N64HwRenderNegotiated = false;  // SET_HW_RENDER accepted this session -- see EnvironmentCb

static LRESULT CALLBACK N64GlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static bool EnsureN64GlWindow() {
    if (g_N64Gl.hwnd) return true;
    HINSTANCE hInst = GetModuleHandleA(nullptr);
    static ATOM windowClass = 0;
    if (!windowClass) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = N64GlWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = "Mad2N64GlHiddenWindow";
        windowClass = RegisterClassA(&wc);
        if (!windowClass) {
            Log("N64 HW render: RegisterClassA failed, err=%lu\n", GetLastError());
            return false;
        }
    }
    g_N64Gl.hwnd =
        CreateWindowExA(0, "Mad2N64GlHiddenWindow", "Mad2N64Gl", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, hInst, nullptr);
    if (!g_N64Gl.hwnd) {
        Log("N64 HW render: CreateWindowExA failed, err=%lu\n", GetLastError());
        return false;
    }
    return true;
}

// Resolves a GL function beyond 1.1 by name -- wglGetProcAddress first
// (returns NULL or one of a handful of documented sentinel "not found"
// values -- 1/2/3/-1 cast to a pointer -- on failure, MSDN's own
// documented quirk), falling back to GetProcAddress(opengl32.dll, ...)
// for true 1.1 entry points wglGetProcAddress may not resolve.
static void* ResolveGlProc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    intptr_t pv = reinterpret_cast<intptr_t>(p);
    if (pv == 0 || pv == 1 || pv == 2 || pv == 3 || pv == -1) {
        static HMODULE hOpenGl32 = GetModuleHandleA("opengl32.dll");
        p = hOpenGl32 ? reinterpret_cast<void*>(GetProcAddress(hOpenGl32, name)) : nullptr;
    }
    return p;
}

static bool EnsureN64GlContext() {
    if (g_N64Gl.contextReady) return true;
    if (!EnsureN64GlWindow()) return false;

    g_N64Gl.hdc = GetDC(g_N64Gl.hwnd);
    if (!g_N64Gl.hdc) {
        Log("N64 HW render: GetDC failed\n");
        return false;
    }

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(g_N64Gl.hdc, &pfd);
    if (!pf || !SetPixelFormat(g_N64Gl.hdc, pf, &pfd)) {
        Log("N64 HW render: Choose/SetPixelFormat failed, err=%lu\n", GetLastError());
        return false;
    }

    g_N64Gl.hglrc = wglCreateContext(g_N64Gl.hdc);
    if (!g_N64Gl.hglrc) {
        Log("N64 HW render: wglCreateContext failed, err=%lu\n", GetLastError());
        return false;
    }
    if (!wglMakeCurrent(g_N64Gl.hdc, g_N64Gl.hglrc)) {
        Log("N64 HW render: wglMakeCurrent failed, err=%lu\n", GetLastError());
        return false;
    }

#define LOADGL(field, name)                                                              \
    g_N64Gl.field = reinterpret_cast<decltype(g_N64Gl.field)>(ResolveGlProc(name));       \
    if (!g_N64Gl.field) {                                                                 \
        Log("N64 HW render: missing GL entry point \"%s\"\n", name);                      \
        return false;                                                                     \
    }
    LOADGL(glGenFramebuffers, "glGenFramebuffers")
    LOADGL(glDeleteFramebuffers, "glDeleteFramebuffers")
    LOADGL(glBindFramebuffer, "glBindFramebuffer")
    LOADGL(glFramebufferTexture2D, "glFramebufferTexture2D")
    LOADGL(glGenRenderbuffers, "glGenRenderbuffers")
    LOADGL(glDeleteRenderbuffers, "glDeleteRenderbuffers")
    LOADGL(glBindRenderbuffer, "glBindRenderbuffer")
    LOADGL(glRenderbufferStorage, "glRenderbufferStorage")
    LOADGL(glFramebufferRenderbuffer, "glFramebufferRenderbuffer")
    LOADGL(glCheckFramebufferStatus, "glCheckFramebufferStatus")
#undef LOADGL

    Log("N64 HW render: OpenGL context created (renderer=\"%s\" version=\"%s\")\n",
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)), reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    g_N64Gl.contextReady = true;
    return true;
}

static bool EnsureN64Fbo(int width, int height) {
    if (!g_N64Gl.contextReady || width <= 0 || height <= 0) return false;
    if (g_N64Gl.fbo && g_N64Gl.fboW == width && g_N64Gl.fboH == height) return true;

    if (g_N64Gl.fbo) {
        g_N64Gl.glDeleteFramebuffers(1, &g_N64Gl.fbo);
        glDeleteTextures(1, &g_N64Gl.colorTex);
        g_N64Gl.glDeleteRenderbuffers(1, &g_N64Gl.depthRb);
        g_N64Gl.fbo = g_N64Gl.colorTex = g_N64Gl.depthRb = 0;
    }

    glGenTextures(1, &g_N64Gl.colorTex);
    glBindTexture(GL_TEXTURE_2D, g_N64Gl.colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    g_N64Gl.glGenRenderbuffers(1, &g_N64Gl.depthRb);
    g_N64Gl.glBindRenderbuffer(GL_RENDERBUFFER, g_N64Gl.depthRb);
    g_N64Gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    g_N64Gl.glGenFramebuffers(1, &g_N64Gl.fbo);
    g_N64Gl.glBindFramebuffer(GL_FRAMEBUFFER, g_N64Gl.fbo);
    g_N64Gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_N64Gl.colorTex, 0);
    g_N64Gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_N64Gl.depthRb);

    GLenum status = g_N64Gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Log("N64 HW render: FBO incomplete (status=0x%04X)\n", status);
        return false;
    }

    // Leave texture unit 0 / GL_RENDERBUFFER binding clean before handing
    // control to the core's own rendering (context_reset() fires right
    // after this returns) -- our own setup above is the only GL state this
    // mod ever touches, but leaving OUR color texture bound on unit 0
    // going into the core's first-ever GL calls is an unforced, easily
    // avoided state leak, worth ruling out now that live testing showed
    // some (but not all) N64 textures rendering as solid black -- a
    // pattern that matches N64 palette/color-indexed (CI4/CI8) textures,
    // which GLideN64 renders via a second texture unit for the palette
    // lookup, specifically failing while direct RGBA textures work fine.
    // Not confirmed as the actual cause (no GL error was ever logged
    // either way), but free and safe to rule out regardless. The FBO
    // itself (GL_FRAMEBUFFER) is deliberately left bound -- that IS the
    // "current framebuffer" this function just built and advertises via
    // N64GetCurrentFramebuffer, not leftover state.
    glBindTexture(GL_TEXTURE_2D, 0);
    g_N64Gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);

    g_N64Gl.fboW = width;
    g_N64Gl.fboH = height;
    Log("N64 HW render: FBO (re)created %dx%d (fbo=%u tex=%u)\n", width, height, g_N64Gl.fbo, g_N64Gl.colorTex);
    return true;
}

// Set on hw_render.get_current_framebuffer/get_proc_address by EnvironmentCb's
// SET_HW_RENDER handler -- called by the core itself (via glsm) whenever it
// needs to know which FBO to render into, or resolve a GL entry point of
// its own.
static uintptr_t RETRO_CALLCONV N64GetCurrentFramebuffer() { return static_cast<uintptr_t>(g_N64Gl.fbo); }

static retro_proc_address_t RETRO_CALLCONV N64GetProcAddress(const char* sym) {
    return reinterpret_cast<retro_proc_address_t>(ResolveGlProc(sym));
}

// ---------------------------------------------------------------------
// waveOut audio -- a small pool of ring-buffer headers per engine; if none
// are free (WHDR_DONE) when a frame's samples are ready, that frame's audio
// is silently dropped rather than blocking the render thread. Best-effort.
// ---------------------------------------------------------------------

static void CloseWaveOut(Engine& engine) {
    if (!engine.waveOut) return;
    waveOutReset(engine.waveOut);
    for (int i = 0; i < kWaveBufferCount; ++i) {
        if (engine.waveHdrs[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(engine.waveOut, &engine.waveHdrs[i], sizeof(WAVEHDR));
        engine.waveHdrs[i] = WAVEHDR{};
    }
    waveOutClose(engine.waveOut);
    engine.waveOut = nullptr;
    engine.waveOpenSampleRate = 0.0;
}

static void EnsureWaveOut(Engine& engine, double sampleRate) {
    if (engine.waveOut && engine.waveOpenSampleRate == sampleRate) return;
    CloseWaveOut(engine);
    if (sampleRate <= 0.0) return;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&engine.waveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        Log("waveOutOpen failed (mr=%u) -- audio disabled this session for engine %d\n", mr,
            static_cast<int>(engine.kind));
        engine.waveOut = nullptr;
        return;
    }
    for (int i = 0; i < kWaveBufferCount; ++i) {
        engine.waveBufData[i].assign(kWaveBufferCapacitySamples, 0);
        engine.waveHdrs[i] = WAVEHDR{};
    }
    engine.waveNextBuf = 0;
    engine.waveOpenSampleRate = sampleRate;
    Log("waveOut opened at %.1f Hz for engine %d\n", sampleRate, static_cast<int>(engine.kind));
}

static void FlushPendingAudio(Engine& engine) {
    if (!engine.waveOut || engine.pendingAudio.empty()) {
        engine.pendingAudio.clear();
        return;
    }

    WAVEHDR& hdr = engine.waveHdrs[engine.waveNextBuf];
    if ((hdr.dwFlags & WHDR_PREPARED) && !(hdr.dwFlags & WHDR_DONE)) {
        // Still playing -- every buffer in the pool is currently busy.
        // Drop this frame's audio rather than block waiting for one to free up.
        engine.pendingAudio.clear();
        return;
    }
    if (hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(engine.waveOut, &hdr, sizeof(WAVEHDR));

    size_t count = engine.pendingAudio.size();
    if (count > static_cast<size_t>(kWaveBufferCapacitySamples)) count = kWaveBufferCapacitySamples;
    std::vector<int16_t>& buf = engine.waveBufData[engine.waveNextBuf];
    memcpy(buf.data(), engine.pendingAudio.data(), count * sizeof(int16_t));

    hdr = WAVEHDR{};
    hdr.lpData = reinterpret_cast<LPSTR>(buf.data());
    hdr.dwBufferLength = static_cast<DWORD>(count * sizeof(int16_t));
    if (waveOutPrepareHeader(engine.waveOut, &hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
        waveOutWrite(engine.waveOut, &hdr, sizeof(WAVEHDR));
    }
    engine.waveNextBuf = (engine.waveNextBuf + 1) % kWaveBufferCount;
    engine.pendingAudio.clear();
}

// ---------------------------------------------------------------------
// libretro callbacks.
// ---------------------------------------------------------------------

// Reused across frames to avoid reallocating every readback -- see the
// RETRO_HW_FRAME_BUFFER_VALID branch below.
static std::vector<uint8_t> g_N64GlReadbackBuf;

static void RETRO_CALLCONV VideoRefreshCb(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (!g_CallbackEngine) return;

    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        // Hardware-rendered (GLideN64) frame -- the core just finished
        // drawing into our own FBO (g_N64Gl.fbo, returned by
        // N64GetCurrentFramebuffer) using GL calls of its own; read it
        // back into a CPU buffer and upload it exactly like the software
        // path below uses. GL readback is always bottom-to-top row order
        // (a fundamental GL framebuffer convention -- glReadPixels(0,0,...)
        // starts at the bottom-left texel -- not something the core's own
        // "bottom_left_origin" request in SET_HW_RENDER changes on our
        // end), while the D3D9 upload below assumes top-to-bottom, same as
        // every software core's video_refresh_cb -- flip rows while
        // copying out. GL_BGRA/GL_UNSIGNED_BYTE matches D3DFMT_X8R8G8B8's
        // in-memory byte order directly, avoiding a separate channel-
        // swizzle pass on top of the flip.
        if (!g_N64Gl.contextReady || !g_N64Gl.fbo) return;
        size_t rowBytes = static_cast<size_t>(width) * 4;
        g_N64GlReadbackBuf.resize(rowBytes * height);
        g_N64Gl.glBindFramebuffer(GL_FRAMEBUFFER, g_N64Gl.fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_BGRA, GL_UNSIGNED_BYTE,
                     g_N64GlReadbackBuf.data());

        if (!EnsureEmuTexture(static_cast<int>(width), static_cast<int>(height), D3DFMT_X8R8G8B8)) return;
        D3DLOCKED_RECT locked;
        if (FAILED(g_EmuTexture->LockRect(0, &locked, nullptr, 0))) return;
        uint8_t* dst = reinterpret_cast<uint8_t*>(locked.pBits);
        for (unsigned y = 0; y < height; ++y) {
            const uint8_t* srcRow = g_N64GlReadbackBuf.data() + static_cast<size_t>(height - 1 - y) * rowBytes;
            memcpy(dst + static_cast<size_t>(y) * locked.Pitch, srcRow, rowBytes);
        }
        g_EmuTexture->UnlockRect(0);
        return;
    }

    if (!data) return;  // "duplicate last frame" (RETRO_ENVIRONMENT_GET_CAN_DUPE) -- texture already holds it
    retro_pixel_format pixelFormat = g_CallbackEngine->pixelFormat;
    D3DFORMAT fmt = PixelFormatToD3D(pixelFormat);
    if (!EnsureEmuTexture(static_cast<int>(width), static_cast<int>(height), fmt)) return;

    static const size_t kBytesPerPixel[] = {2, 4, 2};  // indexed by retro_pixel_format 0/1/2
    size_t bpp = (pixelFormat >= 0 && pixelFormat <= 2) ? kBytesPerPixel[pixelFormat] : 4;

    D3DLOCKED_RECT locked;
    if (FAILED(g_EmuTexture->LockRect(0, &locked, nullptr, 0))) return;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
    uint8_t* dst = reinterpret_cast<uint8_t*>(locked.pBits);
    size_t rowBytes = static_cast<size_t>(width) * bpp;
    for (unsigned y = 0; y < height; ++y) {
        memcpy(dst + static_cast<size_t>(y) * locked.Pitch, src + static_cast<size_t>(y) * pitch, rowBytes);
    }
    g_EmuTexture->UnlockRect(0);
}

static void AppendAudioSamples(const int16_t* data, size_t frames) {
    if (!g_CallbackEngine) return;
    size_t n = frames * 2;
    g_CallbackEngine->pendingAudio.insert(g_CallbackEngine->pendingAudio.end(), data, data + n);
}

static size_t RETRO_CALLCONV AudioSampleBatchCb(const int16_t* data, size_t frames) {
    AppendAudioSamples(data, frames);
    return frames;
}

static void RETRO_CALLCONV AudioSampleCb(int16_t left, int16_t right) {
    int16_t pair[2] = {left, right};
    AppendAudioSamples(pair, 1);
}

static void RETRO_CALLCONV InputPollCb(void) {}

// Synthetic, one-frame-pulsed JOYPAD_R press -- NOT read from the real
// controller. FCEUmm's own retro_run polls RETRO_DEVICE_ID_JOYPAD_R
// (through this exact callback) to detect the player manually toggling
// the virtual FDS disk's eject/insert state (see
// ApplyFdsDiskSwapAutoAnswer's own comment for why Lost Levels needs this
// answered automatically rather than left to the player). Set for exactly
// one InputStateCb-observing frame at a time; never touched by
// InputStateCb itself so it can be a plain read here.
static bool g_FdsRPulseThisFrame = false;

// XInput -> NES pad translation. No single "official" mapping exists across
// emulators; this one keeps letters aligned (Xbox A -> NES A / jump, Xbox B
// -> NES B / run) and additionally binds X as an alternate run button (a
// common emulator-frontend convention, since NES B is otherwise the only
// route to "run" and X is right there under the thumb). Same mapping for
// both games -- Lost Levels uses an identical NES-standard-pad layout.
static int16_t NesInputState(unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_R) return g_FdsRPulseThisFrame ? 1 : 0;

    const auto& api = Mad2XInput_Resolve();
    if (!api.GetState) return 0;
    XINPUT_STATE state{};
    if (!api.GetState(g_Config.userIndex, &state)) return 0;
    const XINPUT_GAMEPAD& gp = state.Gamepad;

    bool left = (gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0 || gp.sThumbLX < -16384;
    bool right = (gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0 || gp.sThumbLX > 16384;
    bool up = (gp.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0 || gp.sThumbLY > 16384;
    bool down = (gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0 || gp.sThumbLY < -16384;

    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_A: return (gp.wButtons & XINPUT_GAMEPAD_A) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_B:
            return (gp.wButtons & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X)) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_START: return (gp.wButtons & XINPUT_GAMEPAD_START) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return (gp.wButtons & XINPUT_GAMEPAD_BACK) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_LEFT: return left ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT: return right ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_UP: return up ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_DOWN: return down ? 1 : 0;
        default: return 0;
    }
}

// XInput -> N64 pad translation. UNLIKE the NES mapping above, this one is
// NOT a free-form "keep letters aligned" choice -- it's deliberately
// matched to mupen64plus_next's own default RetroPad interpretation
// (inputGetKeys_default in custom/mupen64plus-core/plugin/
// emulate_game_controller_via_libretro.c, libretro/mupen64plus-libretro-nx
// upstream, fetched and read directly rather than guessed), so that
// whatever the core does internally with a given RETRO_DEVICE_ID_JOYPAD_*
// id lines up with the N64 button the mapping below intends:
//   N64 A = RETRO JOYPAD_B, N64 B = RETRO JOYPAD_Y, N64 Z = RETRO JOYPAD_L2,
//   N64 L/R shoulders = RETRO JOYPAD_L/R, C-buttons = RIGHT analog stick,
//   control stick = LEFT analog stick (both read via RETRO_DEVICE_ANALOG,
//   not JOYPAD_UP/DOWN/LEFT/RIGHT, which the core reserves for the N64
//   pad's actual D-Pad).
// RETRO JOYPAD_B/Y/L2/L/R are then mapped from Xbox buttons by POSITION
// (the standard libretro/RetroArch RetroPad-to-Xbox convention: RetroPad
// mirrors SNES face-button layout, B=bottom/A=right/Y=left/X=top, so
// RetroPad B = Xbox A, RetroPad Y = Xbox X) rather than by name -- Xbox A
// (bottom) -> N64 A (jump) reads naturally, and Xbox X (left) -> N64 B
// (punch/attack) matches common third-party N64-pad-on-Xbox-controller
// mapping guides. Z (crouch/ground-pound in SM64) goes on the analog Left
// Trigger, matching the same real-world convention; L/R shoulders go on
// Xbox LB/RB. The analog Y-axis sign was derived (not guessed) from the
// same source file's inputGetKeys_reuse, which negates its raw analogY
// input before treating it as the N64's Y_AXIS -- i.e. the core expects
// "positive Y = pushed down", the opposite of XInput's sThumbLY/RY
// convention ("positive = pushed up"), so both sticks negate Y before
// handing values to the core. X-axis sign was left as XInput's own natural
// "positive = right" convention. This whole mapping is a first pass, not
// independently confirmed in-game against a real N64 pad's feel -- flag
// for correction if C-stick/analog-stick direction ever reads backwards
// during actual play.
static int16_t ClampToS16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

static int16_t N64InputState(unsigned device, unsigned index, unsigned id) {
    const auto& api = Mad2XInput_Resolve();
    if (!api.GetState) return 0;
    XINPUT_STATE state{};
    if (!api.GetState(g_Config.n64UserIndex, &state)) return 0;
    const XINPUT_GAMEPAD& gp = state.Gamepad;

    if (device == RETRO_DEVICE_JOYPAD) {
        switch (id) {
            case RETRO_DEVICE_ID_JOYPAD_B: return (gp.wButtons & XINPUT_GAMEPAD_A) ? 1 : 0;              // N64 A
            case RETRO_DEVICE_ID_JOYPAD_Y: return (gp.wButtons & XINPUT_GAMEPAD_X) ? 1 : 0;              // N64 B
            case RETRO_DEVICE_ID_JOYPAD_START: return (gp.wButtons & XINPUT_GAMEPAD_START) ? 1 : 0;
            case RETRO_DEVICE_ID_JOYPAD_L: return (gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0;
            case RETRO_DEVICE_ID_JOYPAD_R: return (gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0;
            case RETRO_DEVICE_ID_JOYPAD_L2: return gp.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 1 : 0;  // Z
            case RETRO_DEVICE_ID_JOYPAD_UP: return (gp.wButtons & XINPUT_GAMEPAD_DPAD_UP) ? 1 : 0;
            case RETRO_DEVICE_ID_JOYPAD_DOWN: return (gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0;
            case RETRO_DEVICE_ID_JOYPAD_LEFT: return (gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0;
            case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0;
            default: return 0;
        }
    }
    if (device == RETRO_DEVICE_ANALOG) {
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return gp.sThumbLX;
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return ClampToS16(-static_cast<int32_t>(gp.sThumbLY));
        } else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return gp.sThumbRX;
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return ClampToS16(-static_cast<int32_t>(gp.sThumbRY));
        }
    }
    return 0;
}

static int16_t RETRO_CALLCONV InputStateCb(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0 || !g_CallbackEngine) return 0;
    return g_CallbackEngine->kind == System::Nes ? NesInputState(device, index, id)
                                                  : N64InputState(device, index, id);
}

static void RETRO_CALLCONV CoreLogCb(enum retro_log_level level, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log("[core:%d] %s", static_cast<int>(level), buf);
}

static retro_log_callback g_LogCallback{CoreLogCb};

// GET_SYSTEM_DIRECTORY/GET_SAVE_DIRECTORY point at "nescore" for the FCEUmm
// engine (see justfile's _deploy-nes -- also where FCEUmm looks for
// "disksys.rom", the FDS BIOS NesLostLevelsChallenge needs, see
// roms/README.md) and "n64core" for the N64 engine (see _deploy-n64) --
// branched on g_CallbackEngine since both engines share this one callback.
static bool RETRO_CALLCONV EnvironmentCb(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            if (!g_CallbackEngine) return false;
            auto fmt = *reinterpret_cast<const retro_pixel_format*>(data);
            if (fmt < 0 || fmt > RETRO_PIXEL_FORMAT_RGB565) return false;  // XRGB2101010/HDR10 unsupported
            g_CallbackEngine->pixelFormat = fmt;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *reinterpret_cast<bool*>(data) = true;
            return true;
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
            return true;  // acknowledged, contents ignored -- fixed mapping, see InputStateCb
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            return true;  // acknowledged -- VideoRefreshCb re-sizes the texture off the actual frame anyway
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            return true;  // acknowledged, no core-options UI this milestone -- core uses its own defaults
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            // GLideN64 (via glsm) negotiates its GL context here -- see the
            // "N64 hardware rendering" section above EnsureWaveOut for the
            // full picture. Only meaningful when [N64] Renderer=hardware
            // (the default); software mode (Angrylion) never triggers this
            // at all, since we force the RDP plugin away from GLideN64 in
            // the GET_VARIABLE case below before the core ever gets far
            // enough to ask.
            if (!g_CallbackEngine || g_CallbackEngine->kind != System::N64) return false;
            auto* hw = reinterpret_cast<retro_hw_render_callback*>(data);
            if (!hw || (hw->context_type != RETRO_HW_CONTEXT_OPENGL && hw->context_type != RETRO_HW_CONTEXT_NONE)) {
                Log("N64 HW render: core requested unsupported context_type=%d -- denying\n",
                    hw ? static_cast<int>(hw->context_type) : -1);
                return false;
            }
            if (!EnsureN64GlContext()) {
                Log("N64 HW render: GL context setup failed -- denying HW render (try [N64] "
                    "Renderer=software in config.cfg)\n");
                return false;
            }
            g_N64Gl.contextReset = hw->context_reset;
            g_N64Gl.contextDestroy = hw->context_destroy;
            hw->get_current_framebuffer = N64GetCurrentFramebuffer;
            hw->get_proc_address = N64GetProcAddress;
            g_N64HwRenderNegotiated = true;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            // Force mupen64plus_next's RDP/CPU/RSP plugins plus two
            // performance/stability-critical options that turned out NOT to
            // be safe to leave unanswered, unlike every NES-core option
            // below -- discovered live, not anticipated up front:
            //
            // 1) cpucore: an unanswered GET_VARIABLE here left the core
            //    running "R4300 emulator: Pure Interpreter" (by far the
            //    slowest of its three CPU cores) rather than falling back to
            //    its own documented "recommended" default
            //    (dynamic_recompiler, confirmed compiled into this exact
            //    32-bit Windows build via WITH_DYNAREC=x86 in the core's own
            //    Makefile) -- that metadata default is only ever applied by
            //    a frontend that presents a real core-options UI and writes
            //    a value back; a minimal frontend like this one that just
            //    answers (or doesn't) individual GET_VARIABLE calls leaves
            //    the core's own internal variable at whatever it zero-
            //    initializes to, which is its slowest listed option (array
            //    index 0), not the documented default. Stacked with
            //    Angrylion's own deliberate slowness and retro_run()
            //    running synchronously on Mad2's render thread every
            //    EndScene call, this alone made the whole game appear to
            //    hard-lock (an extremely long single retro_run() call
            //    blocking the thread that also pumps Mad2's own
            //    rendering/input -- not yet an actual crash at this point).
            //
            // 2) rsp-plugin: forcing "hle" here turned out to be a trap.
            //    Per the core's own libretro.c, requesting HLE while
            //    Angrylion is the active RDP plugin makes the core silently
            //    substitute "Parallel RSP" instead (logged as "Selected HLE
            //    RSP with Angrylion, falling back to Parallel RSP!") --
            //    confirmed by fetching libretro.c directly, not assumed.
            //    "Parallel RSP" (the mupen64plus-rsp-paraLLEl submodule) is
            //    NOT the Vulkan-based ParaLLEl-RDP project despite the
            //    similar name -- it's its own independent CPU-side JIT (GNU
            //    Lightning-based dynamic recompiler for RSP microcode,
            //    confirmed by fetching that submodule's own source tree: no
            //    Vulkan/shader files anywhere in it, just lightning/'s
            //    portable x86/arm/etc code generator). That's a SECOND
            //    independent JIT running alongside the R4300 dynarec forced
            //    above, generating and executing machine code at runtime
            //    inside this already-hooked Wine process -- and a live test
            //    with dynarec fixed but this RSP substitution still in play
            //    crashed Mad2.exe outright (confirmed via a zombie process,
            //    not merely slow). Forcing "cxd4" instead sidesteps the
            //    HLE-with-Angrylion substitution logic entirely (the core
            //    connects it directly, no fallback path) and picks the
            //    traditional, non-JIT pairing Angrylion has shipped with for
            //    years -- a plain interpreter, no runtime code generation,
            //    at the cost of being slower than the JIT-based Parallel RSP
            //    would have been if it worked.
            //
            // R4300 dynarec itself needs no GL/Vulkan context (a self-
            // contained CPU-side JIT, orthogonal to the render-context
            // constraint that motivated denying HW_RENDER below), so
            // forcing it doesn't reopen that risk -- only the RSP JIT
            // substitution above turned out to be the actual problem.
            //
            // 3) astick-sensitivity: a THIRD live-discovered trap, same
            //    shape as (1) -- the core's own left-analog-stick reader
            //    (inputGetKeys_reuse in emulate_game_controller_via_
            //    libretro.c) scales every stick movement by
            //    `astick_sensitivity / 100.0` before handing it to the N64.
            //    That variable is a plain `int astick_sensitivity;` at file
            //    scope in libretro.c with NO initializer -- C zero-
            //    initializes it, so left unanswered it's 0, not the
            //    documented default of 100 (percent) -- multiplying every
            //    stick reading by exactly zero. Confirmed directly from
            //    source, not guessed: this is exactly why the control stick
            //    read as permanently centered even though InputStateCb was
            //    returning real XInput values. astick-deadzone has the same
            //    zero-init/zero-unless-answered shape but a deadzone of 0
            //    is harmless (more permissive than the documented default
            //    of 15%, not less) -- forced anyway for correctness/parity
            //    rather than relying on that being safe by accident.
            //
            // Every other key still falls through to "not implemented" --
            // confirmed safe for the NES core's own options, not
            // re-verified for every other mupen64plus_next option that
            // isn't performance/stability/input-critical.
            //
            // rdp-plugin/rsp-plugin differ by [N64] Renderer: "gliden64"
            // needs "hle" (its own natural, fast, default pairing -- no
            // Angrylion-specific substitution logic engages for it, unlike
            // (2) above); "angrylion" still needs "cxd4" for the same
            // reason as always. Forcing rdp-plugin explicitly either way
            // (rather than leaving hardware mode's "gliden64" unanswered
            // and trusting the documented default) avoids relying on the
            // same "unanswered means the documented default" assumption
            // that already turned out wrong twice above.
            //
            // 4) GLideN64-specific options, live-discovered again: a first
            //    hardware-render test rendered real geometry but with every
            //    surface a flat solid color instead of its actual texture --
            //    same "unanswered GET_VARIABLE leaves the core's C variable
            //    at its zero-init, not its documented default" bug class as
            //    (1) and (3), just in GLideN64's own option set instead of
            //    the core proper. Rather than bisect by hand, every GLideN64
            //    option's documented default (core_options.h) was
            //    cross-checked against its actual libretro.c file-scope
            //    initializer by script -- caught four boolean mismatches
            //    this way (EnableTextureCache, EnableLODEmulation,
            //    EnableShadersStorage, txFilterIgnoreBG, all documented
            //    "True" but zero-initialized "False") -- but that first pass
            //    alone didn't fix it: live testing still showed SOME N64
            //    textures rendering solid black, others fine. The script's
            //    regex had a blind spot for options whose documented default
            //    itself is platform-conditional (#if defined(VC)/#elif
            //    HAVE_LIBNX/#else in core_options.h, not a single quoted
            //    string) -- MaxTxCacheSize is exactly one of those, found by
            //    hand on the follow-up pass: its own description literally
            //    reads "Set Max texture cache size (in elements). Reduce it
            //    if you experience black textures leading to a crash" --
            //    documented default 8000 (our platform's #else branch,
            //    neither VC nor HAVE_LIBNX), zero-initialized to 0 elements
            //    -- a texture cache that can hold nothing, which fails
            //    textures individually as cache pressure evicts them rather
            //    than failing all texturing outright, matching the observed
            //    "some load, some don't" pattern far better than any of the
            //    four boolean options above did on their own.
            struct ForcedVar {
                const char* key;
                const char* value;
            };
            static const ForcedVar kHardwareVars[] = {
                {"mupen64plus-rdp-plugin", "gliden64"},
                {"mupen64plus-cpucore", "dynamic_recompiler"},
                {"mupen64plus-rsp-plugin", "hle"},
                {"mupen64plus-astick-deadzone", "15"},
                {"mupen64plus-astick-sensitivity", "100"},
                {"mupen64plus-EnableTextureCache", "True"},
                {"mupen64plus-EnableLODEmulation", "True"},
                {"mupen64plus-EnableShadersStorage", "True"},
                {"mupen64plus-txFilterIgnoreBG", "True"},
                {"mupen64plus-MaxTxCacheSize", "8000"},
            };
            static const ForcedVar kSoftwareVars[] = {
                {"mupen64plus-rdp-plugin", "angrylion"},
                {"mupen64plus-cpucore", "dynamic_recompiler"},
                {"mupen64plus-rsp-plugin", "cxd4"},
                {"mupen64plus-astick-deadzone", "15"},
                {"mupen64plus-astick-sensitivity", "100"},
            };
            auto* v = reinterpret_cast<retro_variable*>(data);
            if (v && v->key && g_CallbackEngine && g_CallbackEngine->kind == System::N64) {
                bool hw = g_Config.n64Renderer == N64Renderer::Hardware;
                const ForcedVar* table = hw ? kHardwareVars : kSoftwareVars;
                size_t count = hw ? (sizeof(kHardwareVars) / sizeof(kHardwareVars[0]))
                                   : (sizeof(kSoftwareVars) / sizeof(kSoftwareVars[0]));
                for (size_t i = 0; i < count; ++i) {
                    if (strcmp(v->key, table[i].key) == 0) {
                        v->value = table[i].value;
                        return true;
                    }
                }
            }
            return false;  // "not implemented" -- documented signal for "use your hardcoded default"
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            *reinterpret_cast<retro_log_callback*>(data) = g_LogCallback;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *reinterpret_cast<const char**>(data) =
                (g_CallbackEngine && g_CallbackEngine->kind == System::N64) ? "n64core" : "nescore";
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------
// Core loading -- once per process, per ENGINE (see Engine above). Which
// GAME is loaded into a given engine's core can change at runtime (see
// EnsureGameLoaded below).
// ---------------------------------------------------------------------

static const char* CorePathFor(System system) {
    return system == System::Nes ? g_Config.corePath : g_Config.n64CorePath;
}

static const char* FetchHintFor(System system) {
    return system == System::Nes ? "`just fetch-libretro-fceumm`" : "`just fetch-libretro-mupen64plus`";
}

static bool EnsureCoreLoaded(Engine& engine) {
    if (engine.coreLoaded) return true;
    g_CallbackEngine = &engine;

    const char* corePath = CorePathFor(engine.kind);
    engine.module = LoadLibraryA(corePath);
    if (!engine.module) {
        Log("LoadLibraryA(\"%s\") failed, err=%lu -- run %s and deploy again\n", corePath, GetLastError(),
            FetchHintFor(engine.kind));
        return false;
    }

#define RESOLVE(field, name)                                                                                \
    engine.api.field = reinterpret_cast<decltype(engine.api.field)>(                                        \
        reinterpret_cast<void*>(GetProcAddress(engine.module, name)));                                      \
    if (!engine.api.field) {                                                                                 \
        Log("Core missing export \"%s\"\n", name);                                                          \
        return false;                                                                                       \
    }
    RESOLVE(api_version, "retro_api_version")
    RESOLVE(init, "retro_init")
    RESOLVE(deinit, "retro_deinit")
    RESOLVE(get_system_info, "retro_get_system_info")
    RESOLVE(get_system_av_info, "retro_get_system_av_info")
    RESOLVE(set_environment, "retro_set_environment")
    RESOLVE(set_video_refresh, "retro_set_video_refresh")
    RESOLVE(set_audio_sample, "retro_set_audio_sample")
    RESOLVE(set_audio_sample_batch, "retro_set_audio_sample_batch")
    RESOLVE(set_input_poll, "retro_set_input_poll")
    RESOLVE(set_input_state, "retro_set_input_state")
    RESOLVE(set_controller_port_device, "retro_set_controller_port_device")
    RESOLVE(reset, "retro_reset")
    RESOLVE(run, "retro_run")
    RESOLVE(load_game, "retro_load_game")
    RESOLVE(unload_game, "retro_unload_game")
    RESOLVE(get_memory_data, "retro_get_memory_data")
    RESOLVE(get_memory_size, "retro_get_memory_size")
#undef RESOLVE

    Log("Core loaded: %s (libretro API version=%u)\n", corePath, engine.api.api_version());

    engine.api.set_environment(EnvironmentCb);
    engine.api.set_video_refresh(VideoRefreshCb);
    engine.api.set_audio_sample(AudioSampleCb);
    engine.api.set_audio_sample_batch(AudioSampleBatchCb);
    engine.api.set_input_poll(InputPollCb);
    engine.api.set_input_state(InputStateCb);
    engine.api.init();

    memset(&engine.systemInfo, 0, sizeof(engine.systemInfo));
    engine.api.get_system_info(&engine.systemInfo);
    Log("Core system info: %s %s (need_fullpath=%d)\n", engine.systemInfo.library_name,
        engine.systemInfo.library_version, engine.systemInfo.need_fullpath);

    engine.coreLoaded = true;
    return true;
}

// Forward-declared -- resets the level-tracking/random-start state that's
// only meaningful for whichever NES game is currently resident (see
// below). No N64 equivalent -- N64Sm64Challenge has no RAM-poke state.
static void ResetPerGameRamTrackingState();

// Loads g_Games[gameIndex] into its engine's core, unloading whatever's
// currently resident in THAT engine first if it's a DIFFERENT game (only
// one game can occupy a given core's own internal state at a time -- but
// the OTHER engine, if it has its own game loaded, is untouched). A no-op
// if that same game is already loaded in its engine -- this is what lets
// Clear/re-trigger of the SAME game pause/resume instead of rebooting (see
// file header).
static bool EnsureGameLoaded(int gameIndex) {
    EmuGame& game = g_Games[gameIndex];
    Engine& engine = EngineFor(game.system);
    if (!EnsureCoreLoaded(engine)) return false;
    if (engine.gameLoaded && engine.loadedGameIndex == gameIndex) return true;
    g_CallbackEngine = &engine;

    if (engine.gameLoaded) {
        Log("Unloading %s to switch to %s\n", g_Games[engine.loadedGameIndex].effectName, game.effectName);
        engine.api.unload_game();
        engine.gameLoaded = false;
        engine.loadedGameIndex = -1;
    }

    retro_game_info info{};
    info.path = game.romPath;

    if (engine.systemInfo.need_fullpath) {
        info.data = nullptr;
        info.size = 0;
    } else {
        FILE* f = fopen(game.romPath, "rb");
        if (!f) {
            Log("fopen(\"%s\") failed -- see roms/README.md, then deploy again\n", game.romPath);
            return false;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        engine.romBytes.resize(size > 0 ? static_cast<size_t>(size) : 0);
        size_t read = engine.romBytes.empty() ? 0 : fread(engine.romBytes.data(), 1, engine.romBytes.size(), f);
        fclose(f);
        if (read != engine.romBytes.size()) {
            Log("fread(\"%s\") short read (%zu/%zu bytes)\n", game.romPath, read, engine.romBytes.size());
            return false;
        }
        info.data = engine.romBytes.data();
        info.size = engine.romBytes.size();
    }

    if (game.system == System::N64) g_N64HwRenderNegotiated = false;  // set by EnvironmentCb's SET_HW_RENDER, freshly, per load

    if (!engine.api.load_game(&info)) {
        Log("retro_load_game(\"%s\") failed%s\n", game.romPath,
            game.isFds ? " -- FDS games also need nescore\\disksys.rom, see roms/README.md" : "");
        return false;
    }

    memset(&engine.avInfo, 0, sizeof(engine.avInfo));
    engine.api.get_system_av_info(&engine.avInfo);
    Log("ROM loaded (%s): %s (%ux%u @ %.3f fps, %.1f Hz audio)\n", game.effectName, game.romPath,
        engine.avInfo.geometry.base_width, engine.avInfo.geometry.base_height, engine.avInfo.timing.fps,
        engine.avInfo.timing.sample_rate);

    if (game.system == System::N64 && g_N64HwRenderNegotiated) {
        // Only now (geometry known) can the FBO the core will render into
        // actually be created -- see EnsureN64Fbo/context_reset's own
        // comments. GLideN64's own emu_step_initialize() is deferred until
        // context_reset() fires for the GL/Vulkan RDP plugins (confirmed
        // from the core's own retro_load_game -- non-GL plugins call it
        // directly instead), so skipping this call would leave the core
        // never actually finishing its own init.
        unsigned w = engine.avInfo.geometry.max_width ? engine.avInfo.geometry.max_width : engine.avInfo.geometry.base_width;
        unsigned h = engine.avInfo.geometry.max_height ? engine.avInfo.geometry.max_height : engine.avInfo.geometry.base_height;
        if (!EnsureN64Fbo(static_cast<int>(w), static_cast<int>(h))) {
            Log("N64 HW render: FBO setup failed after successful GL context negotiation -- aborting load "
                "(try [N64] Renderer=software in config.cfg)\n");
            return false;
        }
        if (g_N64Gl.contextReset) g_N64Gl.contextReset();
    }

    EnsureWaveOut(engine, engine.avInfo.timing.sample_rate);

    engine.loadedGameIndex = gameIndex;
    engine.gameLoaded = true;
    if (game.system == System::Nes) ResetPerGameRamTrackingState();
    return true;
}

// ---------------------------------------------------------------------
// Input block -- identical technique to mad2companionmod's SM64Challenge
// (see sm64mod.cpp's ApplyInputBlock/RemoveInputBlock): fully zeroes what
// Mad2 itself sees while the NES core owns the controller.
// ---------------------------------------------------------------------

struct HandleSet {
    Mad2XInputOverrideHandle h[XUSER_MAX_COUNT] = {};
};
static HandleSet g_BlockHandles;

static void ApplyInputBlock(DWORD userIndex, bool broadcastToAllControllers) {
    const auto& api = Mad2XInput_Resolve();
    if (!api.AddOverride) {
        Log("[Dependency] mad2xinput.dll not available -- cannot block Mad2 input for the active effect\n");
        return;
    }
    Mad2XInputOverride ov{};
    ov.forceBlockButtons = 0xFFFF;
    ov.enableForceLX = ov.enableForceLY = ov.enableForceRX = ov.enableForceRY = TRUE;
    ov.forceLX = ov.forceLY = ov.forceRX = ov.forceRY = 0;

    auto addOne = [&](DWORD idx) {
        if (g_BlockHandles.h[idx] && api.RemoveOverride) api.RemoveOverride(g_BlockHandles.h[idx]);
        g_BlockHandles.h[idx] = api.AddOverride(idx, &ov);
    };
    if (broadcastToAllControllers) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) addOne(i);
    } else {
        addOne(userIndex);
    }
}

static void RemoveInputBlock() {
    const auto& api = Mad2XInput_Resolve();
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (g_BlockHandles.h[i] && api.RemoveOverride) api.RemoveOverride(g_BlockHandles.h[i]);
        g_BlockHandles.h[i] = 0;
    }
}

// ---------------------------------------------------------------------
// mad2effects Apply/Clear -- see file header for the "pause, don't reload"
// design and why there's no watcher thread here (no child process to
// watch). Shared by every game in g_Games[], distinguished by the
// Mad2EffectDesc::apply/clearUserdata index passed through by
// EnsureEffectsRegistered.
// ---------------------------------------------------------------------

static volatile bool g_Active = false;
static int g_ActiveGameIndex = -1;  // which g_Games[] entry g_Active refers to, -1 = none

static void WINAPI ApplyEmuGame(void* userdata) {
    int gameIndex = static_cast<int>(reinterpret_cast<intptr_t>(userdata));

    if (g_Active && g_ActiveGameIndex != gameIndex) {
        // Switching games while one is running -- explicitly clear the old
        // one's effect (not just overwrite our own state) so mad2effects'
        // own isActive bookkeeping (and mad2effectshud) stays honest: only
        // one of these can really be active at a time, regardless of
        // whether the switch crosses engines (NES <-> N64) or stays within
        // one.
        const auto& effects = Mad2Effects_Resolve();
        if (effects.ClearByName) effects.ClearByName(g_Games[g_ActiveGameIndex].effectName);
    }

    if (!EnsureGameLoaded(gameIndex)) {
        Log("%s: core/ROM not ready -- effect not applied\n", g_Games[gameIndex].effectName);
        return;
    }
    const EmuGame& game = g_Games[gameIndex];
    if (game.system == System::Nes) {
        ApplyInputBlock(g_Config.userIndex, g_Config.broadcastToAllControllers);
    } else {
        ApplyInputBlock(g_Config.n64UserIndex, g_Config.n64BroadcastToAllControllers);
    }
    g_Active = true;
    g_ActiveGameIndex = gameIndex;
    Log("%s applied\n", g_Games[gameIndex].effectName);
}

static void WINAPI ClearEmuGame(void* userdata) {
    int gameIndex = static_cast<int>(reinterpret_cast<intptr_t>(userdata));
    if (!g_Active || g_ActiveGameIndex != gameIndex) return;  // not the one currently running -- no-op
    g_Active = false;
    RemoveInputBlock();
    Log("%s cleared (paused -- core/ROM stay loaded)\n", g_Games[gameIndex].effectName);
}

static bool g_EffectsRegistered = false;

static void EnsureEffectsRegistered() {
    if (g_EffectsRegistered) return;
    const auto& api = Mad2Effects_Resolve();
    if (!api.Register) return;  // retry next frame

    for (int i = 0; i < kGameCount; ++i) {
        Mad2EffectDesc desc{};
        desc.name = g_Games[i].effectName;
        desc.displayName = g_Games[i].displayName;
        desc.defaultWeight = g_Games[i].weight;
        desc.apply = ApplyEmuGame;
        desc.applyUserdata = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        desc.clear = ClearEmuGame;
        desc.clearUserdata = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        desc.defaultMinDurationMs = 0;
        desc.defaultMaxDurationMs = 0;  // no auto-revert -- see file header
        BOOL ok = api.Register(&desc);
        Log("Mad2Effects_Register(\"%s\") -> %d\n", g_Games[i].effectName, ok);
    }
    g_EffectsRegistered = true;
}

// Force-reloads whichever game is currently the ACTIVE mad2effects entry
// (see Config::refreshGameVk) -- unlike a plain Clear+Trigger of the same
// effect, which EnsureGameLoaded treats as "already loaded, just resume"
// and leaves alone, this unloads it first so the subsequent Trigger's own
// EnsureGameLoaded call does a genuine fresh retro_load_game -- re-rolling
// RandomLevelStart/IncludeHardWorlds exactly like a full Mad2 restart
// would, without actually restarting Mad2. Routed through
// Mad2Effects_Trigger (not just calling ApplyEmuGame directly) so
// mad2effects' own isActive bookkeeping stays correct either way. Scoped to
// the active engine only -- a dormant paused game in the OTHER engine is
// left untouched.
static void ForceReloadCurrentGame() {
    if (g_ActiveGameIndex < 0) {
        Log("RefreshGameKey pressed but nothing is currently active\n");
        return;
    }
    int gameIndex = g_ActiveGameIndex;
    EmuGame& game = g_Games[gameIndex];
    Engine& engine = EngineFor(game.system);
    Log("RefreshGameKey pressed -- forcing fresh reload of %s\n", game.effectName);
    if (engine.gameLoaded) {
        g_CallbackEngine = &engine;
        engine.api.unload_game();
        engine.gameLoaded = false;
        engine.loadedGameIndex = -1;
    }
    const auto& effects = Mad2Effects_Resolve();
    if (effects.Trigger) effects.Trigger(game.effectName, 0);
}

// ---------------------------------------------------------------------
// SMB1 / SMB2J (Lost Levels) shared RAM addresses -- verified directly
// against BOTH games' own disassemblies: SMB1's canonical "SMBDIS.ASM" by
// doppelganger, and Lost Levels' "SMB2J DISASSEMBLY" (threecreepio/
// smb2j-disassembly on GitHub, a ca65 port of doppelganger's original
// smb2j disassembly). Every address below was independently confirmed
// identical in both sources -- fetched and grepped for the exact label
// definitions rather than trusted from "these games are similar" alone.
// All addresses are within the NES's 2KB work RAM ($0000-$07FF), exposed
// by retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) -- true for FDS titles
// too (the FDS's extra RAM is a separate expansion the disk-loaded PRG/CHR
// data lives in; the base 2KB internal work RAM these addresses live in is
// the same 6502 hardware region on both a cartridge and an FDS Famicom).
// ---------------------------------------------------------------------

static const uint16_t kAddr_OperMode = 0x0770;      // 0=title, 1=game, 2=victory(ending only), 3=game over
static const uint8_t kOperMode_Title = 0;
static const uint8_t kOperMode_Game = 1;
static const uint16_t kAddr_WorldNumber = 0x075f;   // 0-indexed world (dash number)
static const uint16_t kAddr_LevelNumber = 0x075c;   // 0-indexed level within world (dash number)
static const uint16_t kAddr_AreaNumber = 0x0760;    // internal index FindAreaPointer actually uses to load an area
static const uint16_t kAddr_HardWorldFlag = 0x07fb; // Lost Levels only -- see ApplyRandomLevelStart's own comment

// Random-level-start injection. Both disassemblies confirm AreaNumber and
// LevelNumber always move in lockstep for the ordinary world 1-8 / level
// 1-4 progression -- both reset to 0 together at world start (title
// screen "Press Start", warp zone entry, and post-castle world-advance:
// SMBDIS.ASM's PlayerEndWorld / smb2main.asm's equivalent all do
// "sta AreaNumber / sta LevelNumber" as a pair), and both increment
// together by exactly 1 on every flagpole-clear (PlayerEndLevel's
// "inc LevelNumber" immediately followed by "inc AreaNumber" in NextArea).
// The ONLY other place AreaNumber gets a value that ISN'T LevelNumber is
// underground/pipe-detour sub-areas, which use a completely different,
// per-level-embedded mechanism (a "row $0e" object-data entry overwriting
// AreaPointer directly -- see CheckFrenzyBuffer/ParseRow0e) that never
// touches AreaNumber or LevelNumber at all. So AreaNumber == LevelNumber
// holds for every reachable (world, level) start point, making a direct
// "world W level L" warp as simple as setting WorldNumber=W-1,
// LevelNumber=AreaNumber=L-1 -- exactly what this does, applied
// continuously while OperMode is still the title screen (harmless/
// idempotent; nothing else writes these three there unless the game's own
// separate "hold Select" world-select cheat feature is active, which this
// mod's automated launch never engages) so it lands correctly regardless
// of exactly which frame "Start" ends up pressed on.
//
// Level count is a fixed 4 (World 1-4 dash numbers) for every world in
// both games, including Lost Levels' World 9 -- World9Areas itself has
// exactly 4 entries in AreaAddrOffsets, same shape as every World 1-8
// entry. Per-game world COUNT (8, or 9 for Lost Levels) lives on
// EmuGame::worldCount instead -- see its own comment for why Lost Levels'
// further "Worlds A-D" content stops there rather than going higher.
static const int kLevelCount = 4;
static bool g_RandomLevelPicked = false;
static uint8_t g_RandomWorldNumber = 0;
static uint8_t g_RandomLevelNumber = 0;

// "Worlds A-D" (Lost Levels only): a harder repeat of World 9, entered by
// forcing HardWorldFlag ($07fb) rather than a different WorldNumber --
// confirmed from GameMenuRoutine/InitializeArea in smb2main.asm:
// HardWorldFlag alone is what makes InitializeArea force SecondaryHardMode
// on regardless of WorldNumber, and what makes LoadHardWorlds pull in a
// SEPARATE FDS-resident file ("SM2DATA4", FileListNumber 3) instead of
// reusing the same World9Areas data World 9 itself already has loaded --
// this is why HardWorldFlag isn't just cosmetic and needs the disk-swap
// handling below, unlike a plain World 9 run.
//
// Unlike WorldNumber/LevelNumber/AreaNumber (forced from the moment the
// title screen appears -- harmless, since nothing reads them until Start
// is processed), HardWorldFlag is only forced from OperMode_Task ($0772)
// reaching 5 onward (AttractModeSubs' own dispatch table: 0=
// AttractModeDiskRoutines, 1=InitializeGame, 2=ScreenRoutines, 3=
// PrimaryGameSetup, 4=GameMenuRoutine, 5=HardWorldsCheckpoint) --
// deliberately NOT held during tasks 0-4, i.e. not during the whole
// stretch where the title screen is just sitting there waiting for Start.
// GameMenuRoutine (task 4) is what actually processes the Start press --
// its own code unconditionally clears HardWorldFlag to 0 the instant
// Start is detected, before conditionally re-setting it (only if
// GamesBeatenCount>=8 and A+Start was held, a real-play condition this
// mod's automated launch never satisfies) -- and its own StartGame
// continuation is what advances OperMode_Task to 5. So task>=5 is the
// earliest point HardWorldFlag can be forced without fighting that clear,
// and it's also otherwise unreachable during ordinary title-screen
// dwelling in real play -- forcing it earlier would put the game in a
// state (hard-world flag set while still choosing whether to even start)
// real players and the original developers never exercise, a plausible
// contributor to World A/B/C/D black-screening on Start observed live
// (root cause not fully confirmed -- FDSBIOS_LOADFILES's own internals
// aren't in either disassembly, since it's part of the Nintendo-provided
// disksys.rom BIOS, not the game ROM).
static const uint16_t kAddr_OperModeTask = 0x0772;
static const uint8_t kOperModeTask_HardWorldsCheckpoint = 5;
static bool g_HardWorldPicked = false;

static void ApplyRandomLevelStart(uint8_t* ram) {
    int loadedGameIndex = EngineFor(System::Nes).loadedGameIndex;
    if (!ram || !g_Config.randomLevelStart || loadedGameIndex < 0) return;
    const EmuGame& game = g_Games[loadedGameIndex];
    if (!g_RandomLevelPicked) {
        if (g_Config.debugForceWorld > 0) {
            int w = g_Config.debugForceWorld;
            if (w > game.worldCount) w = game.worldCount;
            g_RandomWorldNumber = static_cast<uint8_t>(w - 1);
            int lvl = g_Config.debugForceLevel > 0 ? g_Config.debugForceLevel : (rand() % kLevelCount) + 1;
            if (lvl > kLevelCount) lvl = kLevelCount;
            g_RandomLevelNumber = static_cast<uint8_t>(lvl - 1);
            g_HardWorldPicked = g_Config.debugForceHardWorld && game.supportsHardWorlds &&
                                 g_RandomWorldNumber == game.worldCount - 1;
        } else {
            g_RandomWorldNumber = static_cast<uint8_t>(rand() % game.worldCount);
            g_RandomLevelNumber = static_cast<uint8_t>(rand() % kLevelCount);
            g_HardWorldPicked = g_Config.includeHardWorlds && game.supportsHardWorlds &&
                                 g_RandomWorldNumber == game.worldCount - 1 && (rand() % 2) == 0;
        }
        g_RandomLevelPicked = true;
        Log("RandomLevelStart (%s): picked World %d-%d%s%s\n", game.effectName, g_RandomWorldNumber + 1,
            g_RandomLevelNumber + 1, g_HardWorldPicked ? " (hard world A-D)" : "",
            g_Config.debugForceWorld > 0 ? " [DebugForceWorld]" : "");
    }
    if (ram[kAddr_OperMode] == kOperMode_Title) {
        ram[kAddr_WorldNumber] = g_RandomWorldNumber;
        ram[kAddr_LevelNumber] = g_RandomLevelNumber;
        ram[kAddr_AreaNumber] = g_RandomLevelNumber;
        if (g_HardWorldPicked && ram[kAddr_OperModeTask] >= kOperModeTask_HardWorldsCheckpoint) {
            ram[kAddr_HardWorldFlag] = 1;
        }
    }
}

// ---------------------------------------------------------------------
// FDS disk-swap auto-answer -- needed for Lost Levels' World 5-8 AND
// World 9/A-D transitions alike (any of them can trigger a
// LoadWorlds5Thru8/LoadHardWorlds/LoadEnding file reload; discovered while
// wiring up Worlds A-D, but applies just as much to plain World 5-8 play).
//
// FCEUmm's own FDS emulation (src/fds.c/libretro.c) treats the virtual
// disk as permanently inserted unless something explicitly calls
// FCEU_FDSInsert to toggle it -- which happens ONLY in response to the
// player pressing the JOYPAD_R button while GameInfo->type == GIT_FDS (see
// libretro.c's retro_run: "if (curR && !prevR) FCEU_FDSInsert(-1);"). The
// actual file read (LoadFiles/FDSBIOS_LOADFILES) happens BEFORE the
// game's own WaitForEject/WaitForReinsert wait loop and succeeds
// regardless (the disk is already "inserted" the whole session, so the
// data is right there) -- but WaitForEject specifically polls
// FDS_DRIVE_STATUS waiting for the disk to go NOT-inserted, which never
// happens on its own since nothing here is pressing R for real. Left
// unanswered, that loop holds DiskIOTask at 2 forever: the game hangs on
// a black "please swap the disk" screen even though the data it needed is
// already loaded.
//
// Fixed by watching DiskIOTask ($07fc) ourselves and synthesizing exactly
// one JOYPAD_R press (via g_FdsRPulseThisFrame, read by InputStateCb) each
// time it freshly enters WaitForEject (2) or WaitForReinsert (3) --
// confirmed the same two state indices across all three of this game's
// DiskIOTask dispatchers (AttractModeDiskRoutines/GameModeDiskRoutines/
// VictoryModeDiskRoutines/HardWorldsCheckpoint all share the identical
// "DiskScreen, <load step>, WaitForEject, WaitForReinsert, ResetDiskVars"
// 5-step shape, just with a different <load step> routine each).
static const uint16_t kAddr_DiskIOTask = 0x07fc;
static const uint8_t kDiskIOTask_WaitForEject = 2;
static const uint8_t kDiskIOTask_WaitForReinsert = 3;
static bool g_DiskIOTaskTrackingValid = false;
static uint8_t g_LastDiskIOTask = 0;

// Fine-grained tracing for debugging Worlds A-D -- logs every single time
// OperMode/OperMode_Task/DiskIOTask/HardWorldFlag changes value, not just
// DiskIOTask entering WaitForEject/WaitForReinsert. Added after a live test
// showed a black screen on Start with NO "auto-answering" log line at all,
// meaning DiskIOTask never even reached state 2 -- this traces the whole
// state machine (including whether HardWorldsCheckpoint/LoadHardWorlds is
// reached at all, and whether it's LoadFiles/FDSBIOS_LOADFILES itself --
// invisible to source-level tracing, since it's inside the Nintendo BIOS,
// not the game ROM -- that's stalling) so the next test pins down exactly
// where it's stuck instead of guessing further.
static bool g_FdsTraceValid = false;
static uint8_t g_LastOperModeTrace = 0, g_LastOperModeTaskTrace = 0, g_LastHardWorldFlagTrace = 0;

static void TraceFdsStateTransitions(const uint8_t* ram) {
    if (!ram) return;
    uint8_t operMode = ram[kAddr_OperMode];
    uint8_t operModeTask = ram[kAddr_OperModeTask];
    uint8_t diskIOTask = ram[kAddr_DiskIOTask];
    uint8_t hardWorldFlag = ram[kAddr_HardWorldFlag];

    if (g_FdsTraceValid && (operMode != g_LastOperModeTrace || operModeTask != g_LastOperModeTaskTrace ||
                             diskIOTask != g_LastDiskIOTask || hardWorldFlag != g_LastHardWorldFlagTrace)) {
        Log("FDS trace: OperMode %d->%d OperMode_Task %d->%d DiskIOTask %d->%d HardWorldFlag %d->%d\n",
            g_LastOperModeTrace, operMode, g_LastOperModeTaskTrace, operModeTask, g_LastDiskIOTask, diskIOTask,
            g_LastHardWorldFlagTrace, hardWorldFlag);
    }
    g_LastOperModeTrace = operMode;
    g_LastOperModeTaskTrace = operModeTask;
    g_LastHardWorldFlagTrace = hardWorldFlag;
    g_FdsTraceValid = true;
}

static void ApplyFdsDiskSwapAutoAnswer(uint8_t* ram) {
    g_FdsRPulseThisFrame = false;
    int loadedGameIndex = EngineFor(System::Nes).loadedGameIndex;
    if (!ram || loadedGameIndex < 0 || !g_Games[loadedGameIndex].isFds) return;

    TraceFdsStateTransitions(ram);

    uint8_t task = ram[kAddr_DiskIOTask];
    if (g_DiskIOTaskTrackingValid && task != g_LastDiskIOTask &&
        (task == kDiskIOTask_WaitForEject || task == kDiskIOTask_WaitForReinsert)) {
        g_FdsRPulseThisFrame = true;
        Log("FDS disk-swap: auto-answering DiskIOTask=%d\n", task);
    }
    g_LastDiskIOTask = task;
    g_DiskIOTaskTrackingValid = true;
}

// Music engine state (Square1/Square2/NoiseSoundQueue, used for one-off
// sound effects, are deliberately NOT touched here -- see MuteMusic's own
// comment in EnsureConfigLoaded above). AreaMusicQueue/EventMusicQueue are
// "start this track" requests that the engine itself already clears to 0
// every single frame once consumed (RunSoundSubroutines in both
// disassemblies); the actual "what's currently playing" state
// MusicHandler keeps re-rendering every frame lives in
// AreaMusicBuffer/EventMusicBuffer instead. AreaMusicBuffer_Alt is the
// engine's own "what to restore once the current event track ends"
// scratch slot (see LoadEventMusic).
static const uint16_t kAddr_AreaMusicQueue = 0x00fb;
static const uint16_t kAddr_EventMusicQueue = 0x00fc;
static const uint16_t kAddr_AreaMusicBuffer = 0x00f4;
static const uint16_t kAddr_AreaMusicBuffer_Alt = 0x07c5;
static const uint16_t kAddr_EventMusicBuffer = 0x07b1;

// "Silence" (%10000000, confirmed at the same value in both disassemblies)
// is not an ad-hoc value -- it's a real, fully-fledged music track ID the
// game itself stores into EventMusicQueue at several points (flagpole
// grab, pipe warps, area transitions -- see e.g. SMBDIS.ASM's "lda
// #Silence; sta EventMusicQueue ;silence music and leave") whose header
// (SilenceHdr/SilenceData) renders actual zero-volume APU writes through
// the ordinary per-frame note pipeline. This matters because simply
// forcing the queue/buffer bytes to a bare 0 makes MusicHandler's own
// "nothing to do" early exit trigger -- which SKIPS writing to the APU
// entirely, so whatever note was already sounding keeps ringing forever
// instead of actually cutting off. Using the game's own Silence track
// instead reliably produces one real "now be quiet" APU write, exactly
// like every other music transition in this game already relies on.
//
// Silence is a genuine looping TRACK, though, not a one-shot flag: once
// EventMusicBuffer holds it, MusicHandler's ContinueMusic path keeps
// re-rendering SilenceData every single subsequent frame for as long as
// the buffer stays nonzero -- and per SilenceHdr's own header bytes, that
// ongoing rendering writes to the SQUARE 1 channel too. Square 1 is also
// what SMB1/Lost Levels' jump sound effect uses (Square1SfxHandler runs
// BEFORE MusicHandler each frame in RunSoundSubroutines, so MusicHandler's
// write lands last and wins) -- holding Silence continuously was confirmed
// live to silence the jump sound right along with the music, while
// Square2/noise-channel effects (coin, stomp, etc.) were unaffected. So
// Silence is only ever used here for exactly ONE frame per "something is
// trying to play" event, immediately followed by flattening every one of
// these four bytes to a bare 0 -- which DOES engage MusicHandler's
// do-nothing early exit (no more APU writes of any kind from the music
// engine, so no more Square 1 contention with jump) precisely because,
// having just come through one genuine Silence frame, there is no held
// note left to cut off. See ApplyMuteMusic below.
static const uint8_t kMusic_Silence = 0x80;

static uint8_t* GetNesRam() {
    Engine& engine = EngineFor(System::Nes);
    if (!engine.api.get_memory_data) return nullptr;
    return reinterpret_cast<uint8_t*>(engine.api.get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

// Holds whichever area-music track was actually playing (or about to
// resume) the moment MuteMusic was last turned on, so turning it back off
// can explicitly re-request that same track via the ordinary
// AreaMusicQueue "start this track" path (LoadAreaMusic) -- the same
// fully-supported entry point the game itself always uses to (re)start
// area music (e.g. after a star-power/hurry-up jingle ends), rather than
// poking AreaMusicBuffer directly and hoping its internal note-position
// counters happened to survive being muted untouched. This does mean
// un-muting restarts the track from the beginning rather than resuming
// mid-note -- a deliberate, safer tradeoff over relying on unverified
// assumptions about that internal state.
static bool g_WasMuteMusic = false;
static uint8_t g_SavedAreaMusicBuffer = 0;

static void ApplyMuteMusic(uint8_t* ram) {
    if (!ram) return;
    bool wantMute = g_Config.muteMusic;

    if (wantMute && !g_WasMuteMusic) {
        uint8_t current = ram[kAddr_AreaMusicBuffer];
        g_SavedAreaMusicBuffer = current ? current : ram[kAddr_AreaMusicBuffer_Alt];
    }

    if (wantMute) {
        bool somethingPlaying = (ram[kAddr_EventMusicBuffer] != 0 && ram[kAddr_EventMusicBuffer] != kMusic_Silence) ||
                                 ram[kAddr_AreaMusicBuffer] != 0 ||
                                 (ram[kAddr_EventMusicQueue] != 0 && ram[kAddr_EventMusicQueue] != kMusic_Silence) ||
                                 ram[kAddr_AreaMusicQueue] != 0;
        if (somethingPlaying) {
            // One real cutoff pass -- see kMusic_Silence's comment for why
            // this can't just be held here every frame.
            ram[kAddr_EventMusicQueue] = kMusic_Silence;
        } else {
            ram[kAddr_EventMusicQueue] = 0;
            ram[kAddr_AreaMusicQueue] = 0;
            ram[kAddr_EventMusicBuffer] = 0;
            ram[kAddr_AreaMusicBuffer] = 0;
        }
    } else if (g_WasMuteMusic) {
        ram[kAddr_EventMusicQueue] = 0;
        ram[kAddr_EventMusicBuffer] = 0;
        if (g_SavedAreaMusicBuffer) ram[kAddr_AreaMusicQueue] = g_SavedAreaMusicBuffer;
    }

    g_WasMuteMusic = wantMute;
}

// Detects "the player just finished a level and the game moved on to the
// next one": OperMode stays in GameMode (1) continuously (so this never
// fires on death -> game-over-mode, nor on the true end-of-game Victory
// Mode screen, which per both disassemblies' OperModeExecutionTree is a
// completely different OperMode value reached only after the final
// castle) while (WorldNumber, LevelNumber) changes value. This is
// deliberately a RAM-state-transition check rather than trying to catch
// one specific internal task ID (e.g. the flagpole-slide routine) --
// simpler, and correct regardless of which exact path advanced the level
// (flagpole, warp pipe/zone, etc).
static bool g_LevelTrackingValid = false;
static uint8_t g_LastOperMode = 0, g_LastWorld = 0, g_LastLevel = 0;

static bool DetectLevelComplete(const uint8_t* ram) {
    if (!ram) return false;
    uint8_t operMode = ram[kAddr_OperMode];
    uint8_t world = ram[kAddr_WorldNumber];
    uint8_t level = ram[kAddr_LevelNumber];

    bool complete = g_LevelTrackingValid && g_LastOperMode == kOperMode_Game && operMode == kOperMode_Game &&
                    (world != g_LastWorld || level != g_LastLevel);

    g_LastOperMode = operMode;
    g_LastWorld = world;
    g_LastLevel = level;
    g_LevelTrackingValid = true;
    return complete;
}

static void ResetPerGameRamTrackingState() {
    g_LevelTrackingValid = false;
    g_RandomLevelPicked = false;
    g_HardWorldPicked = false;
    g_WasMuteMusic = false;
    g_DiskIOTaskTrackingValid = false;
    g_FdsRPulseThisFrame = false;
}

// ---------------------------------------------------------------------
// Full-screen quad draw -- deliberately NOT mad2textrenderer's own
// DrawTexturedQuad, which hardcodes D3DTEXF_LINEAR (right for GDI text/HUD
// icons, wrong here: it bilinear-blurs the NES's native 256x240 pixels
// across a much larger screen-space quad). NES output wants nearest-
// neighbor upscaling to stay crisp, so this duplicates that function's
// state-setting body with POINT filtering instead -- SaveState/RestoreState
// are still reused as-is below (pure save/restore, no filter opinion).
// ---------------------------------------------------------------------

namespace {
struct ScreenVertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
}  // namespace

static void DrawEmuQuadPointFiltered(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float w, float h) {
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetTexture(0, tex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);

    ScreenVertex verts[4] = {
        {0.0f, 0.0f, 0, 1, 0xFFFFFFFF, 0.0f, 0.0f},
        {w, 0.0f, 0, 1, 0xFFFFFFFF, 1.0f, 0.0f},
        {0.0f, h, 0, 1, 0xFFFFFFFF, 0.0f, 1.0f},
        {w, h, 0, 1, 0xFFFFFFFF, 1.0f, 1.0f},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ScreenVertex));
}

// ---------------------------------------------------------------------
// Per-frame: run the ACTIVE engine's core one frame + draw it full-screen,
// opaque, replacing whatever Mad2 itself rendered this frame. The NES RAM-
// poke features (MuteMusic/RandomLevelStart/FDS auto-answer/
// AutoStopOnLevelComplete) only make sense for the NES engine -- N64Sm64
// Challenge has none of those, just run+draw.
// ---------------------------------------------------------------------

static void RunAndDrawFrame(IDirect3DDevice9* dev) {
    if (g_ActiveGameIndex < 0) return;
    EmuGame& game = g_Games[g_ActiveGameIndex];
    Engine& engine = EngineFor(game.system);
    g_CallbackEngine = &engine;

    if (game.system == System::Nes) {
        uint8_t* ram = GetNesRam();
        ApplyMuteMusic(ram);
        ApplyRandomLevelStart(ram);
        ApplyFdsDiskSwapAutoAnswer(ram);

        engine.api.run();
        FlushPendingAudio(engine);

        if (g_Config.autoStopOnLevelComplete && DetectLevelComplete(ram)) {
            Log("Level complete detected (world=%d level=%d) -- stopping %s\n", g_LastWorld + 1, g_LastLevel + 1,
                game.effectName);
            const auto& effects = Mad2Effects_Resolve();
            if (effects.ClearByName) effects.ClearByName(game.effectName);
        }
    } else {
        // N64: retro_run() is called synchronously here, on the render
        // thread, exactly like NES -- see file header's "background-thread
        // attempt" note for why. Mad2's own render thread does stall for
        // the duration of N64's retro_run() call as a result -- a real,
        // known tradeoff, not yet solved.
        engine.api.run();
        FlushPendingAudio(engine);
    }

    if (!g_EmuTexture) return;
    UINT w = g_ScreenW ? g_ScreenW : static_cast<UINT>(g_TexW);
    UINT h = g_ScreenH ? g_ScreenH : static_cast<UINT>(g_TexH);

    Mad2TextRendererSavedState saved{};
    const auto& tr = Mad2TextRenderer_Resolve();
    if (!tr.SaveState || !tr.RestoreState) return;
    tr.SaveState(dev, &saved);
    DrawEmuQuadPointFiltered(dev, g_EmuTexture, static_cast<float>(w), static_cast<float>(h));
    tr.RestoreState(dev, &saved);
}

// ---------------------------------------------------------------------
// Test hotkeys -- per-game TestTriggerKey (see g_Games), shared
// TestClearKey/MuteMusicToggleKey (see Config).
// ---------------------------------------------------------------------

static void PollTestKeys() {
    static bool wasTriggerPressed[kGameCount] = {};
    for (int i = 0; i < kGameCount; ++i) {
        int vk = g_Games[i].testTriggerVk;
        bool isPressed = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (isPressed && !wasTriggerPressed[i]) {
            Log("TestTriggerKey pressed -- forcing %s\n", g_Games[i].effectName);
            const auto& effects = Mad2Effects_Resolve();
            if (effects.Trigger) effects.Trigger(g_Games[i].effectName, 0);
        }
        wasTriggerPressed[i] = isPressed;
    }

    static bool wasClearPressed = false;
    bool isClearPressed = g_Config.testClearVk > 0 && (GetAsyncKeyState(g_Config.testClearVk) & 0x8000) != 0;
    if (isClearPressed && !wasClearPressed && g_ActiveGameIndex >= 0) {
        Log("TestClearKey pressed -- clearing %s\n", g_Games[g_ActiveGameIndex].effectName);
        const auto& effects = Mad2Effects_Resolve();
        if (effects.ClearByName) effects.ClearByName(g_Games[g_ActiveGameIndex].effectName);
    }
    wasClearPressed = isClearPressed;

    static bool wasMuteTogglePressed = false;
    bool isMuteTogglePressed =
        g_Config.muteMusicToggleVk > 0 && (GetAsyncKeyState(g_Config.muteMusicToggleVk) & 0x8000) != 0;
    if (isMuteTogglePressed && !wasMuteTogglePressed) {
        g_Config.muteMusic = !g_Config.muteMusic;
        Log("MuteMusicToggleKey pressed -- MuteMusic now %d\n", g_Config.muteMusic);
    }
    wasMuteTogglePressed = isMuteTogglePressed;

    static bool wasRefreshPressed = false;
    bool isRefreshPressed = g_Config.refreshGameVk > 0 && (GetAsyncKeyState(g_Config.refreshGameVk) & 0x8000) != 0;
    if (isRefreshPressed && !wasRefreshPressed) ForceReloadCurrentGame();
    wasRefreshPressed = isRefreshPressed;
}

// ---------------------------------------------------------------------
// mad2settings.dll registration -- just "Refresh Active Game" now (a live
// action wrapping ForceReloadCurrentGame, the same function RefreshGameKey
// already calls -- Raw Config has no way to express a one-shot action, so
// this one stays registered). Every other Nes/N64 setting this used to
// register (MuteMusic, AutoStopOnLevelComplete, RandomLevelStart,
// DebugForceWorld/Level/HardWorld) is redundant now that the debug menu's
// generic "Raw Config" tab shows/edits every config.cfg key directly --
// including testClearVk/muteMusicToggleVk/refreshGameVk/each game's own
// testTriggerVk, which already show up there automatically (with a proper
// rebind widget) via mad2config.dll's vkey type hint, with no registration
// needed here at all. Raw Config only ever writes straight to config.cfg
// though, so this mod still has to notice those edits itself --
// ReloadSettingsLive re-reads the removed fields every ~1s (see its call
// site in HookEndScene).
// ---------------------------------------------------------------------

static void WINAPI RefreshGameAction(void*) { ForceReloadCurrentGame(); }

// userdata carries the g_Games[] index (cast through uintptr_t) rather than
// needing kGameCount separate trampoline functions -- same pattern
// mad2cheatapply used to use for its own per-cheat actions.
static void WINAPI LaunchGameAction(void* userdata) {
    size_t idx = static_cast<size_t>(reinterpret_cast<uintptr_t>(userdata));
    if (idx >= static_cast<size_t>(kGameCount)) return;
    const auto& effects = Mad2Effects_Resolve();
    if (effects.Trigger) effects.Trigger(g_Games[idx].effectName, 0);
}

// Same as the TestClearKey hotkey (see PollTestKeys) -- clears whichever
// game is currently active, if any.
static void WINAPI TestClearActiveGameAction(void*) {
    if (g_ActiveGameIndex < 0) {
        Log("Test Clear Active Game pressed but nothing is currently active\n");
        return;
    }
    const auto& effects = Mad2Effects_Resolve();
    if (effects.ClearByName) effects.ClearByName(g_Games[g_ActiveGameIndex].effectName);
}

static bool g_SettingsRegistered = false;

static void EnsureSettingsRegistered() {
    if (g_SettingsRegistered) return;
    const auto& settings = Mad2Settings_Resolve();
    if (!settings.Register) return;
    g_SettingsRegistered = true;

    for (int i = 0; i < kGameCount; ++i) {
        Mad2SettingDesc launch{};
        launch.category = "NES/N64";
        launch.label = g_Games[i].displayName;
        launch.type = MAD2SETTING_ACTION;
        launch.onChanged = LaunchGameAction;
        launch.userdata = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
        settings.Register(&launch);
    }

    Mad2SettingDesc testClear{};
    testClear.category = "NES/N64";
    testClear.label = "Test Clear Active Game";
    testClear.type = MAD2SETTING_ACTION;
    testClear.onChanged = TestClearActiveGameAction;
    settings.Register(&testClear);

    Mad2SettingDesc refresh{};
    refresh.category = "NES/N64";
    refresh.label = "Refresh Active Game";
    refresh.type = MAD2SETTING_ACTION;
    refresh.onChanged = RefreshGameAction;
    settings.Register(&refresh);

    Log("Registered settings with mad2settings.dll\n");
}

static uint64_t g_LastNesSettingsReloadMs = 0;

static void ReloadSettingsLive() {
    uint64_t now = GetTickCount64();
    if (now - g_LastNesSettingsReloadMs < 1000) return;
    g_LastNesSettingsReloadMs = now;

    const auto& api = Mad2Config_Resolve();
    if (!api.GetBool) return;
    g_Config.muteMusic = api.GetBool("Nes", "MuteMusic", g_Config.muteMusic ? TRUE : FALSE, "") != FALSE;
    g_Config.autoStopOnLevelComplete =
        api.GetBool("Nes", "AutoStopOnLevelComplete", g_Config.autoStopOnLevelComplete ? TRUE : FALSE, "") != FALSE;
    g_Config.randomLevelStart =
        api.GetBool("Nes", "RandomLevelStart", g_Config.randomLevelStart ? TRUE : FALSE, "") != FALSE;
    g_Config.debugForceHardWorld =
        api.GetBool("Nes", "DebugForceHardWorld", g_Config.debugForceHardWorld ? TRUE : FALSE, "") != FALSE;
    g_Config.debugForceWorld = api.GetInt("Nes", "DebugForceWorld", g_Config.debugForceWorld, "");
    g_Config.debugForceLevel = api.GetInt("Nes", "DebugForceLevel", g_Config.debugForceLevel, "");
}

// ---------------------------------------------------------------------
// Direct3DCreate9 IAT hook -> CreateDevice vtable patch -> EndScene vtable
// patch (same technique/vtable slots as mad2igttimer -- see that file for
// the full explanation of why this composes with mad2shadowfix regardless
// of mad2/mods/ load order).
// ---------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    EnsureConfigLoaded();
    ReloadSettingsLive();
    EnsureEffectsRegistered();
    EnsureSettingsRegistered();
    PollTestKeys();

    if (g_Active) RunAndDrawFrame(This);

    return RealEndScene(This);
}

typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t RealCreateDevice = nullptr;

static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType,
                                                    HWND hFocusWindow, DWORD BehaviorFlags,
                                                    D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                    IDirect3DDevice9** ppReturnedDeviceInterface) {
    HRESULT hr = RealCreateDevice(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                   ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        g_Device = *ppReturnedDeviceInterface;
        if (pPresentationParameters) {
            g_ScreenW = pPresentationParameters->BackBufferWidth;
            g_ScreenH = pPresentationParameters->BackBufferHeight;
        }
        if (!RealEndScene) {
            void* prevEndScene = nullptr;
            Mad2HookUtil_PatchVTableSlot(g_Device, 42, reinterpret_cast<void*>(HookEndScene), &prevEndScene);
            RealEndScene = reinterpret_cast<EndScene_t>(prevEndScene);
            Log("Hooked IDirect3DDevice9::EndScene (vtable[42], device=%p, screen=%ux%u)\n", (void*)g_Device,
                g_ScreenW, g_ScreenH);
        }
    }
    return hr;
}

typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
static Direct3DCreate9_t RealDirect3DCreate9 = nullptr;

static IDirect3D9* WINAPI HookDirect3DCreate9(UINT SDKVersion) {
    if (!RealDirect3DCreate9) {
        Log("ERROR: RealDirect3DCreate9 is null\n");
        return nullptr;
    }
    IDirect3D9* pReal = RealDirect3DCreate9(SDKVersion);
    if (!pReal) return pReal;

    if (!RealCreateDevice) {
        void* prevCreateDevice = nullptr;
        Mad2HookUtil_PatchVTableSlot(pReal, 16, reinterpret_cast<void*>(HookCreateDevice), &prevCreateDevice);
        RealCreateDevice = reinterpret_cast<CreateDevice_t>(prevCreateDevice);
        Log("Hooked IDirect3D9::CreateDevice (vtable[16], d3d9=%p)\n", (void*)pReal);
    }
    return pReal;
}

static void InstallHooks() {
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) {
        Log("ERROR: d3d9.dll not loaded yet; cannot hook Direct3DCreate9\n");
        return;
    }
    RealDirect3DCreate9 =
        reinterpret_cast<Direct3DCreate9_t>(reinterpret_cast<void*>(GetProcAddress(hD3D9, "Direct3DCreate9")));

    void* prev = nullptr;
    int n = Mad2HookUtil_PatchIatAllModules("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(HookDirect3DCreate9), &prev);
    if (prev) RealDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(prev);
    Log("Patched Direct3DCreate9 in %d module(s) (chained real=%p)\n", n, (void*)RealDirect3DCreate9);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            srand(static_cast<unsigned>(time(nullptr)) ^ GetCurrentProcessId());  // seeds ApplyRandomLevelStart's pick
            InstallHooks();
            break;
        case DLL_PROCESS_DETACH:
            if (g_Active && g_ActiveGameIndex >= 0) {
                ClearEmuGame(reinterpret_cast<void*>(static_cast<intptr_t>(g_ActiveGameIndex)));
            }
            // Give the core a chance to free its own GL resources (its
            // last-ever GL calls) while our context is still current and
            // before unload_game/deinit touch it -- see
            // retro_hw_render_callback::context_destroy's own contract.
            if (g_N64Gl.contextReady && g_N64Gl.contextDestroy) g_N64Gl.contextDestroy();
            for (Engine& engine : g_Engines) {
                CloseWaveOut(engine);
                if (engine.gameLoaded && engine.api.unload_game) engine.api.unload_game();
                if (engine.coreLoaded && engine.api.deinit) engine.api.deinit();
                if (engine.module) {
                    FreeLibrary(engine.module);
                    engine.module = nullptr;
                }
            }
            if (g_N64Gl.contextReady) {
                if (g_N64Gl.fbo) g_N64Gl.glDeleteFramebuffers(1, &g_N64Gl.fbo);
                if (g_N64Gl.colorTex) glDeleteTextures(1, &g_N64Gl.colorTex);
                if (g_N64Gl.depthRb) g_N64Gl.glDeleteRenderbuffers(1, &g_N64Gl.depthRb);
                wglMakeCurrent(nullptr, nullptr);
                wglDeleteContext(g_N64Gl.hglrc);
            }
            if (g_N64Gl.hdc) ReleaseDC(g_N64Gl.hwnd, g_N64Gl.hdc);
            if (g_N64Gl.hwnd) DestroyWindow(g_N64Gl.hwnd);
            if (g_EmuTexture) {
                g_EmuTexture->Release();
                g_EmuTexture = nullptr;
            }
            break;
        default:
            break;
    }
    return TRUE;
}
