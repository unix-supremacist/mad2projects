// Game-state chaos effects for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Registers three effects
// with mad2effects (mad2/mods/mad2effects.dll -- see
// ../../mad2effects/include/mad2effects_api.h), ported from ../mad2mod's
// external SDL overlay tool:
//   - ObjectShuffle: ../mad2mod's F4 key (shuffle_object_coordinates).
//     Scans the process heap for Havok physics objects (matched by vtable
//     pointer) and Fisher-Yates shuffles their positions, excluding the
//     player's own position object.
//   - MangoMutate: ../mad2mod's chaos-event "Mutate mango count". Rewrites
//     the player's mango count to a random value, gated the same way the
//     original was (mangos > 0, under 100000, and only in the story levels
//     that actually display a mango counter).
//   - PlayerTeleport: ../mad2mod's F5 key (teleport_player_randomly).
//     Teleports the player to a random position from data/coordinates.yaml
//     (deployed alongside this mod -- see root CMakeLists.txt) matching
//     the current level. ../mad2mod's single memory write was observed to
//     often not visibly move the player while they were moving -- see
//     ApplyPlayerTeleport's comment for why, and how this port forces the
//     write to stick instead.
// All three are one-shot state mutations with no Clear callback -- see
// mad2effects_api.h's "instantaneous/fire-and-forget" contract. This mod
// has no D3D9 hook of its own, so it uses the same "init-only background
// thread" pattern as mad2inputeffectsmod: DllMain spawns a thread that
// waits for mad2config/mad2effects to be resolvable, registers the three
// effects, then exits -- each effect's own Apply callback resolves the
// game-module pointers it needs (ActorInfoLib.dll/TfbHavokLibrary.dll/
// ScriptInfoLib.dll) lazily, on the thread that calls Trigger.
//
// Memory access: ../mad2mod ran as a separate Linux process and read/wrote
// Mad2.exe's memory via process_vm_readv/writev, which fails gracefully on
// an unmapped address. This mod runs *inside* Mad2.exe's own process, so
// the equivalent is a plain pointer dereference -- there is no syscall to
// fail. Every hop is checked against IsPlausiblePointer() first, mirroring
// mad2playercoords' FollowChain (see its file header for the same
// argument); this doesn't make these reads bulletproof against a garbage-
// but-plausible pointer, but it covers the real failure mode of a null/
// near-null intermediate pointer.
//
// Current level detection: ../mad2mod tracked "current level" externally
// by reading /proc/<pid>/fd to see which level archive file was open --
// not available to an in-process Windows DLL (no /proc access through the
// Win32 CRT under Wine). Instead this mirrors project_documentation.md's
// "Level Transition system": ScriptInfoLib.dll's _levelLoadRequest pointer
// (+0xA2708) resolves to a StringInfo object (validated by vtable pointer)
// whose UTF-16 string buffer holds the most recent level-load request's
// name. This is a deliberate simplification of ../mad2mod's full
// initiator/redirected-target state machine (which exists there to support
// level-redirect hijacking, a feature this mod doesn't need) -- it's just
// "the last level name the game asked to load," which is what's actually
// needed to gate MangoMutate/pick a coordinate pool for PlayerTeleport.
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2effects/include/mad2effects_api.h"
#include "../../mad2levelredirectmod/include/mad2levelredirect_api.h"

#include "../../mad2leveldetect/mad2leveldetect.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2gameeffectsmod]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [GameEffects] section).
// ---------------------------------------------------------------------

struct Config {
    std::string coordinatesFile = "coordinates.yaml";
    int objectShuffleWeight = 24;   // matches ../mad2mod's 24-ticket chaos-event weight
    int mangoMutateWeight = 6;      // matches ../mad2mod's 6-ticket weight
    int mangoMutateRandomCeiling = 10000;  // new mango count is rand() % this, matches ../mad2mod
    int playerTeleportWeight = 28;  // matches ../mad2mod's 28-ticket weight
    int playerTeleportForceAttempts = 5;    // see ApplyPlayerTeleport's file-header comment
    int playerTeleportForceIntervalMs = 16;

    // All three effects below mutate state once and are done -- but
    // mad2effects only ever marks an effect "active" (what mad2effectshud
    // reads) if it has a Clear callback, since that's what it uses to know
    // when to flip it back off. A real fire-and-forget effect (clear ==
    // nullptr) never sets isActive at all, so it never appears in the HUD,
    // even for an instant. These durations exist purely so the HUD shows
    // "Shuffled Level Objects" etc. for a few seconds after the fact --
    // there is no game state left to revert, so each Clear callback below
    // is a no-op.
    DWORD objectShuffleDisplayMs = 3000;
    DWORD mangoMutateDisplayMs = 3000;
    DWORD playerTeleportDisplayMs = 3000;
} g_Config;

static void LoadConfig() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetInt || !api.GetString) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in defaults\n");
        return;
    }

    char buf[MAX_PATH];
    api.GetString("GameEffects", "CoordinatesFile", g_Config.coordinatesFile.c_str(),
                   "PlayerTeleport: path (relative to the game exe dir) of the YAML file of logged\n"
                   "positions to teleport to, one entry per {level, x, y, z} -- see data/coordinates.yaml\n"
                   "in this mod's source for the format, ported from ../mad2mod's coordinates.yaml.",
                   buf, sizeof(buf));
    g_Config.coordinatesFile = buf;

    g_Config.objectShuffleWeight =
        api.GetInt("GameEffects", "ObjectShuffleWeight", g_Config.objectShuffleWeight,
                    "ObjectShuffle: chance-pool weight.");
    g_Config.mangoMutateWeight = api.GetInt("GameEffects", "MangoMutateWeight", g_Config.mangoMutateWeight,
                                             "MangoMutate: chance-pool weight.");
    g_Config.mangoMutateRandomCeiling =
        api.GetInt("GameEffects", "MangoMutateRandomCeiling", g_Config.mangoMutateRandomCeiling,
                    "MangoMutate: new mango count is randomized in [0, MangoMutateRandomCeiling).");
    g_Config.playerTeleportWeight = api.GetInt("GameEffects", "PlayerTeleportWeight", g_Config.playerTeleportWeight,
                                                "PlayerTeleport: chance-pool weight.");
    g_Config.playerTeleportForceAttempts =
        api.GetInt("GameEffects", "PlayerTeleportForceAttempts", g_Config.playerTeleportForceAttempts,
                    "PlayerTeleport: number of times to re-write the target position, spaced\n"
                    "PlayerTeleportForceIntervalMs apart. ../mad2mod's single-write version was observed to\n"
                    "often silently fail to teleport while the player was moving -- Havok's own physics\n"
                    "tick re-derives position from its internal rigid-body state and can clobber a single\n"
                    "poke before it's ever visible. Repeating the write across several ticks makes it\n"
                    "overwhelmingly likely one lands after the last tick that would otherwise win.");
    g_Config.playerTeleportForceIntervalMs = api.GetInt(
        "GameEffects", "PlayerTeleportForceIntervalMs", g_Config.playerTeleportForceIntervalMs,
        "PlayerTeleport: milliseconds between forced re-writes (see PlayerTeleportForceAttempts).");

    g_Config.objectShuffleDisplayMs = static_cast<DWORD>(
        api.GetInt("GameEffects", "ObjectShuffleDisplayMs", static_cast<int>(g_Config.objectShuffleDisplayMs),
                    "ObjectShuffle: how long (ms) it shows as active in mad2effectshud. Purely cosmetic --\n"
                    "the shuffle itself happens instantly, there's nothing left to revert when this expires."));
    g_Config.mangoMutateDisplayMs = static_cast<DWORD>(
        api.GetInt("GameEffects", "MangoMutateDisplayMs", static_cast<int>(g_Config.mangoMutateDisplayMs),
                    "MangoMutate: how long (ms) it shows as active in mad2effectshud. Purely cosmetic."));
    g_Config.playerTeleportDisplayMs = static_cast<DWORD>(
        api.GetInt("GameEffects", "PlayerTeleportDisplayMs", static_cast<int>(g_Config.playerTeleportDisplayMs),
                    "PlayerTeleport: how long (ms) it shows as active in mad2effectshud. Purely cosmetic."));

    Log("Config loaded (coordinatesFile=%s)\n", g_Config.coordinatesFile.c_str());
}

// ---------------------------------------------------------------------
// Pointer-chain helpers (same technique as mad2playercoords/src/playercoords.cpp).
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

// ActorInfoLib.dll base + 0x1A030, then this chain, lands on the player's
// position object (x/y/z floats at +0xD0/+0xD4/+0xD8). Same chain used by
// mad2playercoords -- see project_documentation.md section A.
static const uintptr_t kPlayerPosOffsets[] = {0x1C, 0x20, 0x148, 0x8, 0x160, 0x54};

static uintptr_t ResolvePlayerPosPtr() {
    HMODULE actorInfoLib = GetModuleHandleA("ActorInfoLib.dll");
    if (!actorInfoLib) return 0;
    return FollowChain(reinterpret_cast<uintptr_t>(actorInfoLib) + 0x0001A030, kPlayerPosOffsets,
                        sizeof(kPlayerPosOffsets) / sizeof(kPlayerPosOffsets[0]));
}

// Same ActorInfoLib.dll base, different chain -- lands on an object whose
// mango count lives at +0x114. See project_documentation.md section B.
static const uintptr_t kMangoOffsets[] = {0x1C, 0x20, 0x104, 0x9D8, 0x98, 0x44};

static uintptr_t ResolveMangoPtr() {
    HMODULE actorInfoLib = GetModuleHandleA("ActorInfoLib.dll");
    if (!actorInfoLib) return 0;
    return FollowChain(reinterpret_cast<uintptr_t>(actorInfoLib) + 0x0001A030, kMangoOffsets,
                        sizeof(kMangoOffsets) / sizeof(kMangoOffsets[0]));
}

// ---------------------------------------------------------------------
// Current level detection.
//
// Primary source: mad2levelredirectmod's Mad2LevelRedirect_GetCurrentLevel,
// which tracks the actual level archive last opened under Content/Streams/
// win/ (see mad2levelredirectmod/src/levelredirect.cpp's file header).
// That's a strictly better signal than the ScriptInfoLib heuristic below
// even with no level redirection involved -- it has no gap where "current
// level" goes stale, and it reports the real CONTENT level rather than the
// requested slot, so it stays correct if mad2levelredirectmod is also
// redirecting level loads (../mad2rando5). Falls back to the ScriptInfoLib
// heuristic only if that mod isn't installed.
// ---------------------------------------------------------------------

static std::string g_CurrentLevel = "None";

// Detection logic itself now lives in mad2leveldetect.h, shared with
// mad2cheatapply (previously a verbatim ~40-line copy in each mod -- see
// improvement-plan.md item 12). Only this mod's own g_CurrentLevel cache
// stays local.
static void UpdateCurrentLevel() { Mad2LevelDetect_UpdateCurrentLevel(g_CurrentLevel); }

// ---------------------------------------------------------------------
// Skip helper: mad2effects' Trigger() marks an effect "active" (for
// mad2effectshud's benefit -- see Config::objectShuffleDisplayMs's comment)
// BEFORE calling Apply, unconditionally, since it has no way to know
// whether Apply actually did anything. When one of these effects' gating
// checks bails out early (wrong level, unresolved pointer, out-of-range
// value), it did NOT actually mutate anything, so it must immediately
// un-mark itself active -- otherwise the HUD shows e.g. "Mutated Mango
// Count" as active for the full display window in a level that was never
// touched. Mad2Effects_ClearByName is safe to call from inside an Apply
// callback (Trigger has already released its lock by the time Apply runs
// -- see mad2effects/src/effects_dll.cpp's lock-discipline comment).
static void SkipEffect(const char* name, const char* reason) {
    Log("%s: skipped, %s\n", name, reason);
    const auto& effects = Mad2Effects_Resolve();
    if (effects.ClearByName) effects.ClearByName(name);
}

// ---------------------------------------------------------------------
// ObjectShuffle -- ../mad2mod's F4 key.
// ---------------------------------------------------------------------

struct Vec3 {
    float x, y, z;
};

// ../mad2mod walked /proc/<pid>/maps for rw-p/rwxp regions outside any
// .dll and scanned each for the Havok vtable pointer over
// process_vm_readv. In-process, VirtualQuery walks the same address space
// directly; MEM_PRIVATE is the equivalent of "not a mapped module image"
// (loaded DLLs/EXE show up as MEM_IMAGE). The 600MB per-region cap mirrors
// the original's defensive size limit.
static void WINAPI ApplyObjectShuffle(void*) {
    HMODULE havokLib = GetModuleHandleA("TfbHavokLibrary.dll");
    if (!havokLib) {
        SkipEffect("ObjectShuffle", "TfbHavokLibrary.dll not loaded");
        return;
    }
    uint32_t vtableTarget = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(havokLib) + 0x002EEA00);
    uintptr_t playerCoordPtr = ResolvePlayerPosPtr();

    std::vector<uintptr_t> found;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    const uintptr_t kMaxAddr = 0x7FFF0000;
    const SIZE_T kMaxRegionSize = 600ull * 1024 * 1024;
    while (addr < kMaxAddr) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        if (mbi.RegionSize == 0) break;  // avoid an infinite loop on a degenerate query result

        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
            mbi.RegionSize < kMaxRegionSize) {
            uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uint32_t* words = reinterpret_cast<const uint32_t*>(regionStart);
            size_t wordCount = mbi.RegionSize / 4;
            for (size_t i = 0; i < wordCount; ++i) {
                if (words[i] == vtableTarget) {
                    uintptr_t objAddr = regionStart + i * 4;
                    if (objAddr != playerCoordPtr) found.push_back(objAddr);
                }
            }
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    if (found.empty()) {
        SkipEffect("ObjectShuffle", "no coordinate objects found to shuffle");
        return;
    }

    std::vector<Vec3> positions(found.size());
    for (size_t i = 0; i < found.size(); ++i) {
        positions[i] = *reinterpret_cast<const Vec3*>(found[i] + 0xD0);
    }
    std::vector<Vec3> shuffled = positions;
    for (size_t i = shuffled.size() - 1; i > 0; --i) {
        size_t j = static_cast<size_t>(rand()) % (i + 1);
        std::swap(shuffled[i], shuffled[j]);
    }
    for (size_t i = 0; i < found.size(); ++i) {
        *reinterpret_cast<Vec3*>(found[i] + 0xD0) = shuffled[i];
    }
    Log("ObjectShuffle: shuffled %zu objects (player excluded)\n", found.size());
}

// No-op -- see Config::objectShuffleDisplayMs's comment for why this
// exists at all (purely so mad2effectshud has something to show).
static void WINAPI ClearObjectShuffle(void*) {}

// ---------------------------------------------------------------------
// MangoMutate -- ../mad2mod's chaos-event "Mutate mango count".
// ---------------------------------------------------------------------

// Same story-level gate ../mad2mod hardcoded: these are the levels that
// actually display a mango counter.
static const char* kMangoMutateLevels[] = {"BraveNewWild", "Waterhole", "Rites"};

static bool IsMangoMutateLevel(const std::string& level) {
    for (const char* l : kMangoMutateLevels) {
        if (level == l) return true;
    }
    return false;
}

static void WINAPI ApplyMangoMutate(void*) {
    UpdateCurrentLevel();
    if (!IsMangoMutateLevel(g_CurrentLevel)) {
        char reason[160];
        snprintf(reason, sizeof(reason), "current level '%s' doesn't show a mango counter", g_CurrentLevel.c_str());
        SkipEffect("MangoMutate", reason);
        return;
    }
    uintptr_t mangoPtr = ResolveMangoPtr();
    if (!mangoPtr) {
        SkipEffect("MangoMutate", "mango pointer chain unresolved");
        return;
    }
    int32_t current = *reinterpret_cast<int32_t*>(mangoPtr + 0x114);
    if (current <= 0 || current >= 100000) {
        char reason[64];
        snprintf(reason, sizeof(reason), "mango count %d out of range", current);
        SkipEffect("MangoMutate", reason);
        return;
    }
    int32_t newVal = rand() % g_Config.mangoMutateRandomCeiling;
    *reinterpret_cast<int32_t*>(mangoPtr + 0x114) = newVal;
    Log("MangoMutate: mutated mango count from %d to %d\n", current, newVal);
}

// No-op -- see Config::objectShuffleDisplayMs's comment for why this exists.
static void WINAPI ClearMangoMutate(void*) {}

// ---------------------------------------------------------------------
// PlayerTeleport -- ../mad2mod's F5 key.
// ---------------------------------------------------------------------

struct LoggedCoord {
    float x, y, z;
};

// Same line-oriented parse ../mad2mod used: coordinates.yaml is a flat
// sequence of `- level: "Name"` / `  x: ..` / `  y: ..` / `  z: ..` blocks,
// hand-scanned rather than parsed as real YAML (consistent with this
// codebase's general "no vendored parser for a small, fixed format"
// precedent -- see mad2config/mad2twitchcontrolsmod). Re-read fresh on
// every trigger rather than cached: this is a rare, one-shot action, so the
// simplicity of not needing cache invalidation wins.
static bool LoadLevelCoords(const std::string& level, std::vector<LoggedCoord>& out) {
    std::ifstream in(g_Config.coordinatesFile);
    if (!in.is_open()) return false;

    std::string needle = "\"" + level + "\"";
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("level:") == std::string::npos || line.find(needle) == std::string::npos) continue;

        std::string xLine, yLine, zLine;
        if (!std::getline(in, xLine) || !std::getline(in, yLine) || !std::getline(in, zLine)) break;
        size_t xi = xLine.find("x:"), yi = yLine.find("y:"), zi = zLine.find("z:");
        if (xi == std::string::npos || yi == std::string::npos || zi == std::string::npos) continue;

        try {
            LoggedCoord c;
            c.x = std::stof(xLine.substr(xi + 2));
            c.y = std::stof(yLine.substr(yi + 2));
            c.z = std::stof(zLine.substr(zi + 2));
            out.push_back(c);
        } catch (...) {
            // Malformed entry -- skip it.
        }
    }
    return !out.empty();
}

// A single write to the position floats often silently "didn't take" in
// ../mad2mod while the player was moving: Havok re-derives the player's
// position from its own internal rigid-body state every physics tick, and
// that derived value -- not our poke -- is what ends up back at +0xD0 on
// the very next tick. There's no clean hook point here for "after the last
// physics read of the old position, before it writes the new one" (that
// would need intercepting Havok's step function itself, a much bigger
// jump in complexity than anything else in this mod). Instead, brute-force
// it: re-write the same target position several times a few milliseconds
// apart, so at least one write is overwhelmingly likely to land after the
// last physics tick that would otherwise clobber it. Runs on its own
// short-lived thread (bounded by ForceAttempts * ForceIntervalMs, a few
// tens of ms by default) rather than blocking the Apply callback itself,
// since mad2effects_api.h requires Apply not block for long.
struct TeleportForceJob {
    LoggedCoord target;
    std::string level;
};

static DWORD WINAPI ForceTeleportThreadFunc(LPVOID param) {
    TeleportForceJob* job = reinterpret_cast<TeleportForceJob*>(param);
    int attempts = g_Config.playerTeleportForceAttempts;
    if (attempts < 1) attempts = 1;

    int written = 0;
    for (int i = 0; i < attempts; ++i) {
        uintptr_t coordPtr = ResolvePlayerPosPtr();
        if (coordPtr) {
            float* pos = reinterpret_cast<float*>(coordPtr + 0xD0);
            pos[0] = job->target.x;
            pos[1] = job->target.y;
            pos[2] = job->target.z;
            ++written;
        }
        if (i + 1 < attempts) Sleep(static_cast<DWORD>(g_Config.playerTeleportForceIntervalMs));
    }
    Log("PlayerTeleport: forced %d/%d writes of (%.3f, %.3f, %.3f) in '%s'\n", written, attempts, job->target.x,
        job->target.y, job->target.z, job->level.c_str());
    delete job;
    return 0;
}

static void WINAPI ApplyPlayerTeleport(void*) {
    UpdateCurrentLevel();
    // Case-insensitive: the real level-redirect-tracked title-screen
    // archive name is "TITLE" (all caps), confirmed live via
    // logs\mad2.log -- see mad2cheatapply's identical fix for the same bug.
    if (g_CurrentLevel == "None" || _stricmp(g_CurrentLevel.c_str(), "title") == 0) {
        char reason[96];
        snprintf(reason, sizeof(reason), "no active level (current='%s')", g_CurrentLevel.c_str());
        SkipEffect("PlayerTeleport", reason);
        return;
    }

    std::vector<LoggedCoord> coords;
    if (!LoadLevelCoords(g_CurrentLevel, coords)) {
        char reason[160];
        snprintf(reason, sizeof(reason), "no logged coordinates for level '%s' in %s", g_CurrentLevel.c_str(),
                 g_Config.coordinatesFile.c_str());
        SkipEffect("PlayerTeleport", reason);
        return;
    }

    if (!ResolvePlayerPosPtr()) {
        SkipEffect("PlayerTeleport", "player position pointer unresolved");
        return;
    }

    auto* job = new TeleportForceJob{coords[static_cast<size_t>(rand()) % coords.size()], g_CurrentLevel};
    HANDLE h = CreateThread(nullptr, 0, ForceTeleportThreadFunc, job, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        Log("PlayerTeleport: CreateThread for force-write failed: %lu\n", GetLastError());
        delete job;
    }
}

// No-op -- see Config::objectShuffleDisplayMs's comment for why this exists.
static void WINAPI ClearPlayerTeleport(void*) {}

// ---------------------------------------------------------------------
// Init-only background thread: retries until every dependency is
// resolvable, then registers every effect and exits (same pattern as
// mad2inputeffectsmod -- see its file header for why: this mod has no
// per-frame D3D9 hook to lazily resolve dependencies from instead).
// ---------------------------------------------------------------------

// clear/displayMs are only there to make the effect show up briefly in
// mad2effectshud -- see Config::objectShuffleDisplayMs's comment. All
// three effects here mutate state exactly once inside Apply; nothing is
// actually "active" between Apply and Clear.
static void RegisterEffect(const char* name, const char* displayName, int weight, Mad2Effects_ApplyFn apply,
                            Mad2Effects_ClearFn clear, DWORD displayMs) {
    Mad2EffectDesc desc{};
    desc.name = name;
    desc.displayName = displayName;
    desc.defaultWeight = weight;
    desc.apply = apply;
    desc.clear = clear;
    desc.defaultMinDurationMs = displayMs;
    desc.defaultMaxDurationMs = displayMs;
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
        bool effectsOk = Mad2Effects_Resolve().Register != nullptr;
        if (configOk && effectsOk) break;

        ++attempts;
        if (!warned && attempts >= kWarnAfterAttempts) {
            warned = true;
            Log("[Dependency] still waiting on: %s%s(retrying indefinitely)\n", configOk ? "" : "mad2config ",
                effectsOk ? "" : "mad2effects ");
        }
        Sleep(kRetryDelayMs);
    }

    LoadConfig();

    RegisterEffect("ObjectShuffle", "Shuffled Level Objects", g_Config.objectShuffleWeight, ApplyObjectShuffle,
                    ClearObjectShuffle, g_Config.objectShuffleDisplayMs);
    RegisterEffect("MangoMutate", "Mutated Mango Count", g_Config.mangoMutateWeight, ApplyMangoMutate,
                    ClearMangoMutate, g_Config.mangoMutateDisplayMs);
    RegisterEffect("PlayerTeleport", "Randomized Player Position", g_Config.playerTeleportWeight, ApplyPlayerTeleport,
                    ClearPlayerTeleport, g_Config.playerTeleportDisplayMs);

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
