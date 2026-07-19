// Chaos-mode director loop for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. No D3D9 hook, no visual of
// its own -- a single background thread that, on a randomized interval,
// picks one registered mad2effects effect at random (weighted by each
// effect's chance weight, as read from mad2effects' registry via GetInfo --
// each effect-providing mod owns its own weight config key in its own
// config.cfg section, e.g. [InputEffects] ReversalWeight; mad2chaosmod
// does not keep a second, parallel copy of that config) and triggers it.
// Conceptually ports the weighted "ticket pool" +
// timed-director-loop design from ../mad2mod/src/main.cpp (an unrelated,
// external reference tool -- design only, not shared code/APIs).
//
// This mod has no per-frame hook to lazily resolve its dependencies from
// (unlike D3D9-hooking overlay mods), so its thread retries resolving
// mad2config/mad2effects with a short sleep between attempts rather than
// assuming a single resolve at thread start will succeed -- mad2/mods/*.dll
// load order is not guaranteed, and mod_loader.cpp's LoadMods() is a
// synchronous LoadLibraryA loop, so this thread can genuinely start
// running before mad2effects.dll has even been loaded.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2effects/include/mad2effects_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2chaosmod]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [ChaosMode] section), read once at startup --
// matches this codebase's existing "config read once, not hot-reloaded"
// convention (e.g. mad2twitchchat's Channel).
// ---------------------------------------------------------------------

struct Config {
    bool enabled = false;  // opt-in -- chaos mode is off by default
    int minIntervalSeconds = 5;
    int maxIntervalSeconds = 15;
} g_Config;

static void LoadConfig() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetBool) {
        Log("[Dependency] mad2config.dll not found or missing exports -- chaos mode disabled (no config to "
            "read Enabled from; place mad2config.dll in mad2/mods/)\n");
        return;
    }

    g_Config.enabled = api.GetBool("ChaosMode", "Enabled", g_Config.enabled ? TRUE : FALSE,
                                    "Master switch for chaos mode. When true, a randomly weighted registered\n"
                                    "effect is triggered on a randomized interval -- each effect's weight is\n"
                                    "configured in its own providing mod's config section (e.g. [InputEffects]\n"
                                    "ReversalWeight), not here. Off by default.") !=
                       FALSE;
    g_Config.minIntervalSeconds =
        api.GetInt("ChaosMode", "MinIntervalSeconds", g_Config.minIntervalSeconds,
                    "Minimum/maximum seconds between chaos events; the actual interval is randomized\n"
                    "between the two on every trigger.");
    g_Config.maxIntervalSeconds = api.GetInt("ChaosMode", "MaxIntervalSeconds", g_Config.maxIntervalSeconds, "");

    Log("Config loaded: enabled=%d minInterval=%ds maxInterval=%ds\n", g_Config.enabled, g_Config.minIntervalSeconds,
        g_Config.maxIntervalSeconds);
}

// ---------------------------------------------------------------------
// Director thread.
// ---------------------------------------------------------------------

static const int kPoolCap = 100000;    // defensive cap on total ticket-pool size
static const int kMaxPerEffect = 1000;  // defensive cap on any single effect's weight

static int ComputeIntervalMs() {
    int minMs = g_Config.minIntervalSeconds * 1000;
    int maxMs = g_Config.maxIntervalSeconds * 1000;
    int diff = (maxMs > minMs) ? (maxMs - minMs) : 1000;
    return minMs + (rand() % diff);
}

// Builds a weighted ticket pool of effect indices -- direct port of the
// N-tickets-per-weight pattern from ../mad2mod/src/main.cpp's chaos event
// selection, generalized to read weights from mad2effects' registry
// (rather than hardcoded per-event-type constants). Each effect's weight
// comes straight from Mad2EffectInfo::weight -- i.e. whatever the
// registering mod itself read from its own config.cfg section at
// Register() time -- so there is exactly one place each weight is
// configured, not a second copy here.
static std::vector<int> BuildWeightedPool(const Mad2EffectsApi& effects, int count) {
    std::vector<int> pool;
    for (int i = 0; i < count && static_cast<int>(pool.size()) < kPoolCap; ++i) {
        Mad2EffectInfo info{};
        if (!effects.GetInfo(i, &info)) continue;

        int weight = info.weight;
        if (weight < 0) weight = 0;
        if (weight > kMaxPerEffect) weight = kMaxPerEffect;

        int room = kPoolCap - static_cast<int>(pool.size());
        int toAdd = weight < room ? weight : room;
        for (int t = 0; t < toAdd; ++t) pool.push_back(i);
    }
    return pool;
}

static DWORD WINAPI DirectorThreadFunc(LPVOID) {
    const int kRetryDelayMs = 200;
    const int kWarnAfterAttempts = 25;  // ~5s

    int attempts = 0;
    bool warned = false;
    for (;;) {
        bool configOk = Mad2Config_Resolve().GetBool != nullptr;
        bool effectsOk = Mad2Effects_Resolve().Trigger != nullptr;
        if (configOk && effectsOk) break;

        ++attempts;
        if (!warned && attempts >= kWarnAfterAttempts) {
            warned = true;
            Log("[Dependency] still waiting on: %s%s (retrying indefinitely)\n", configOk ? "" : "mad2config ",
                effectsOk ? "" : "mad2effects ");
        }
        Sleep(kRetryDelayMs);
    }

    LoadConfig();
    if (!g_Config.enabled) {
        Log("Chaos mode disabled (ChaosMode.Enabled=false); director thread exiting.\n");
        return 0;
    }

    const auto& effects = Mad2Effects_Resolve();

    for (;;) {
        int intervalMs = ComputeIntervalMs();
        Sleep(intervalMs);

        int count = effects.GetCount();
        if (count <= 0) {
            Log("No effects registered yet; skipping this cycle.\n");
            continue;
        }

        std::vector<int> pool = BuildWeightedPool(effects, count);
        if (pool.empty()) {
            Log("Weighted pool is empty (all effects at weight 0); skipping this cycle.\n");
            continue;
        }

        int chosen = pool[rand() % pool.size()];
        Mad2EffectInfo info{};
        if (!effects.GetInfo(chosen, &info)) continue;

        Log("Chaos event: triggering '%s' (pool size=%zu, next interval=%dms)\n", info.name, pool.size(),
            intervalMs);
        effects.Trigger(info.name, 0);
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            srand(static_cast<unsigned>(time(nullptr)));
            HANDLE h = CreateThread(nullptr, 0, DirectorThreadFunc, nullptr, 0, nullptr);
            if (h) {
                CloseHandle(h);
            } else {
                Log("CreateThread for director thread failed: %lu\n", GetLastError());
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
