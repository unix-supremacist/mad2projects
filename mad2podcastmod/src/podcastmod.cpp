// Podcast chaos effect for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Registers a single
// mad2effects (mad2/mods/mad2effects.dll -- see
// ../../mad2effects/include/mad2effects_api.h) effect, "PodcastBreak", that
// asks mad2music to play a random spoken-word clip, ducking mad2music's own
// continuously-looping background music underneath it. Unlike
// mad2companionmod, this mod never launches or kills mad2music as part of
// the effect itself -- mad2music is expected to already be running for the
// whole session, so ApplyPodcastBreak/ClearPodcastBreak just send a
// one-shot trigger and wait for a reply. See include/mad2podcast_protocol.h
// for the full wire-format rationale.
//
// Two platform branches for GETTING mad2music running in the first place
// (distinct from the trigger/reply protocol above, which is identical on
// both):
//   - Under Wine: mad2music is a native Linux process, forked/supervised by
//     mad2relauncher for the whole play session -- see
//     ../../mad2relauncher/music.go. This mod never launches it.
//   - On real Windows: no relauncher, no automatic supervision. See
//     StartMad2MusicIfNeeded_NativeWindows below -- checks a named mutex
//     mad2music.exe itself holds for its whole lifetime (see
//     ../../mad2music/src/main.cpp) and CreateProcess's it once, letting it
//     run unsupervised for the rest of the session (no crash-relaunch --
//     nothing else on native Windows provides that; same "best effort, not
//     the full Wine-side experience" scope as mad2companionmod's Windows
//     branches).
//
// This mod has no per-frame D3D9 hook, same "init-only background thread"
// shape as mad2inputeffectsmod/mad2sm64mod: DllMain spawns a thread that
// retries until mad2config/mad2effects are resolvable, then registers the
// effect and exits -- see InitThreadFunc below.
//
// PodcastBreak is registered with no auto-revert duration (both duration
// fields 0, but a Clear callback is set), same contract as mad2sm64mod's
// SM64Challenge: it "stays active until explicitly cleared" per
// mad2effects_api.h, and this mod's own watcher thread notices when
// mad2music reports the clip finished (a FINISHED control packet) and
// self-clears via Mad2Effects_ClearByName -- see OnPodcastFinished. g_Active
// is the single source of truth arbitrating that self-clear path against an
// externally-triggered Clear (mad2chaosmod re-rolling, ClearAll, a
// re-trigger while already active), same compare-exchange pattern as
// mad2sm64mod's OnSm64ProcessGone/ClearSM64Challenge.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2effects/include/mad2effects_api.h"
#include "../include/mad2podcast_protocol.h"

#include "../../mad2sharedlog/mad2sharedlog.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2podcastmod]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [Podcast] section).
// ---------------------------------------------------------------------

static const char kDefaultWindowsMusicPath[] = "mad2music\\mad2music.exe";
static const char kDefaultWindowsAudioDir[] = "mad2music\\audio";

struct Config {
    int weight = 5;
    int controlPort = MAD2PODCAST_DEFAULT_CONTROL_PORT;
    int testTriggerVk = VK_F9;  // manual on-demand trigger, independent of mad2chaosmod's weighted RNG
    char windowsMusicPath[MAX_PATH];
    char windowsAudioDir[MAX_PATH];
} g_Config;

static void LoadConfig() {
    strncpy(g_Config.windowsMusicPath, kDefaultWindowsMusicPath, sizeof(g_Config.windowsMusicPath) - 1);
    g_Config.windowsMusicPath[sizeof(g_Config.windowsMusicPath) - 1] = 0;
    strncpy(g_Config.windowsAudioDir, kDefaultWindowsAudioDir, sizeof(g_Config.windowsAudioDir) - 1);
    g_Config.windowsAudioDir[sizeof(g_Config.windowsAudioDir) - 1] = 0;

    const auto& api = Mad2Config_Resolve();
    if (!api.GetInt) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in defaults\n");
        return;
    }

    g_Config.weight = api.GetInt("Podcast", "Weight", g_Config.weight, "PodcastBreak: chance-pool weight.");
    g_Config.controlPort = api.GetInt(
        "Podcast", "ControlPort", g_Config.controlPort,
        "Loopback UDP port mad2podcastmod sends podcast triggers to. Must match\n"
        "mad2music's -control-port / [Podcast] ControlPort (config.cfg is read by both\n"
        "sides independently -- see mad2music/src/config.cpp for why).");
    g_Config.testTriggerVk = api.GetVirtualKey(
        "Podcast", "TestTriggerKey", g_Config.testTriggerVk,
        "Keyboard key that immediately triggers PodcastBreak on demand, bypassing\n"
        "mad2chaosmod's weighted RNG entirely -- useful for testing since the default\n"
        "Weight above only gives it a small share of the chance pool. Virtual-key code\n"
        "name with the VK_ prefix stripped (e.g. F9), or a 0xNN hex code. Set to an\n"
        "unused code (e.g. 0) to disable.\n"
        "See https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes");

    char pathBuf[MAX_PATH];
    api.GetString("Podcast", "WindowsMusicPath", kDefaultWindowsMusicPath,
                   "Path (relative to the game exe dir) to a Windows-built mad2music.exe -- only\n"
                   "used when NOT running under Wine (see `just build-music-windows`). No deploy\n"
                   "target copies it here automatically yet -- place mad2music.exe + SDL3.dll at\n"
                   "this path yourself, same manual-step status as SM64/Jak1's WindowsXxxPath keys.",
                   pathBuf, sizeof(pathBuf));
    strncpy(g_Config.windowsMusicPath, pathBuf, sizeof(g_Config.windowsMusicPath) - 1);
    g_Config.windowsMusicPath[sizeof(g_Config.windowsMusicPath) - 1] = 0;

    api.GetString("Podcast", "WindowsAudioDir", kDefaultWindowsAudioDir,
                   "Path (relative to the game exe dir) to mad2music's audio pool (its -audio-dir\n"
                   "argument) -- only used when NOT running under Wine.",
                   pathBuf, sizeof(pathBuf));
    strncpy(g_Config.windowsAudioDir, pathBuf, sizeof(g_Config.windowsAudioDir) - 1);
    g_Config.windowsAudioDir[sizeof(g_Config.windowsAudioDir) - 1] = 0;

    Log("Config loaded (controlPort=%d, weight=%d, testTriggerVk=0x%X, windowsMusicPath=%s, windowsAudioDir=%s)\n",
        g_Config.controlPort, g_Config.weight, g_Config.testTriggerVk, g_Config.windowsMusicPath,
        g_Config.windowsAudioDir);
}

// ---------------------------------------------------------------------
// Platform detection -- same self-contained check as mad2companionmod's
// sm64mod.cpp/jak1mod.cpp (duplicated per this repo's own precedent of
// every small platform shim keeping its own copy rather than a shared
// library -- see CLAUDE.md's IAT-hooking section for the same reasoning
// applied elsewhere).
// ---------------------------------------------------------------------

static bool IsRunningUnderWine() {
    static int cached = -1;
    if (cached < 0) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        cached = (ntdll && GetProcAddress(ntdll, "wine_get_version")) ? 1 : 0;
    }
    return cached == 1;
}

// ---------------------------------------------------------------------
// Native-Windows-only: start mad2music.exe once for the whole session if
// it isn't already running, since there's no mad2relauncher here to do it.
// Uses a named mutex mad2music.exe itself holds for its entire process
// lifetime (see mad2music/src/main.cpp's Mad2Music_SingleInstanceMutex) --
// OpenMutexA succeeding means an instance is already alive; failing means
// none is, so this launches one. Fire-and-forget: once started, nobody
// supervises or relaunches it if it crashes (matching mad2companionmod's
// own "best effort, not full parity with the Wine+mad2relauncher
// experience" scope for its native-Windows branches).
// ---------------------------------------------------------------------

static const char kMad2MusicMutexName[] = "Mad2Music_SingleInstanceMutex";

static void StartMad2MusicIfNeeded_NativeWindows() {
    HANDLE existing = OpenMutexA(SYNCHRONIZE, FALSE, kMad2MusicMutexName);
    if (existing) {
        CloseHandle(existing);
        Log("[Music] mad2music already running (mutex held) -- not launching another instance\n");
        return;
    }

    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string gameDir(exePath);
    size_t slash = gameDir.find_last_of("\\/");
    if (slash != std::string::npos) gameDir.resize(slash);

    std::string musicExe = gameDir + "\\" + g_Config.windowsMusicPath;
    char cmdLine[2 * MAX_PATH + 32];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\" -audio-dir \"%s\"", musicExe.c_str(), g_Config.windowsAudioDir);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // lpCurrentDirectory = the game exe dir (not mad2music's own
    // subdirectory) -- mad2music.exe reads ./config.cfg and writes
    // ./mad2music.log relative to its cwd, same "lands alongside every
    // other mod's" convention as the Wine/mad2relauncher branch (see
    // mad2music/src/main.cpp's file header). -audio-dir is resolved
    // relative to that same cwd, hence windowsAudioDir being relative too.
    BOOL ok = CreateProcessA(musicExe.c_str(), cmdLine, nullptr, nullptr, FALSE, 0, nullptr, gameDir.c_str(), &si, &pi);
    if (!ok) {
        Log("[Music] CreateProcess(\"%s\") failed, err=%lu -- background music/podcasts disabled this session\n",
            musicExe.c_str(), GetLastError());
        return;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    Log("[Music] Launched mad2music: %s (pid=%lu)\n", musicExe.c_str(), pi.dwProcessId);
}

// ---------------------------------------------------------------------
// Shared state.
// ---------------------------------------------------------------------

static std::atomic<bool> g_Active{false};

static bool g_WinsockReady = false;
static SOCKET g_ControlSocket = INVALID_SOCKET;
static sockaddr_in g_MusicAddr{};

static HANDLE g_WatcherThread = nullptr;

// ---------------------------------------------------------------------
// Sockets.
// ---------------------------------------------------------------------

static bool EnsureWinsock() {
    if (!g_WinsockReady) {
        WSADATA wsaData;
        g_WinsockReady = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
        if (!g_WinsockReady) Log("WSAStartup failed\n");
    }
    return g_WinsockReady;
}

static bool SetupSocket() {
    if (!EnsureWinsock()) return false;
    if (g_ControlSocket != INVALID_SOCKET) return true;

    g_ControlSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_ControlSocket == INVALID_SOCKET) return false;

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bindAddr.sin_port = 0;  // ephemeral -- mad2music replies to whatever source port our sends come from
    bind(g_ControlSocket, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr));

    // Short timeout so the watcher thread's recvfrom loop notices g_Active
    // flipping false (an external Clear()) promptly instead of blocking
    // indefinitely on a FINISHED that may never arrive (e.g. mad2music
    // isn't running at all).
    DWORD timeoutMs = 200;
    setsockopt(g_ControlSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    ZeroMemory(&g_MusicAddr, sizeof(g_MusicAddr));
    g_MusicAddr.sin_family = AF_INET;
    g_MusicAddr.sin_port = htons(static_cast<u_short>(g_Config.controlPort));
    g_MusicAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    return true;
}

static void SendTrigger() {
    if (g_ControlSocket == INVALID_SOCKET) return;
    Mad2PodcastControlPacket pkt{MAD2PODCAST_CONTROL_MAGIC, MAD2PODCAST_CONTROL_TRIGGER};
    sendto(g_ControlSocket, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
           reinterpret_cast<sockaddr*>(&g_MusicAddr), sizeof(g_MusicAddr));
}

// ---------------------------------------------------------------------
// Teardown coordination -- see file header for the two-trigger arbitration
// this compare-exchange implements (external Clear vs. self-clear on
// FINISHED), same shape as mad2sm64mod's OnSm64ProcessGone.
// ---------------------------------------------------------------------

static void OnPodcastFinished() {
    bool expected = true;
    if (!g_Active.compare_exchange_strong(expected, false)) return;

    const auto& effects = Mad2Effects_Resolve();
    if (effects.ClearByName) effects.ClearByName("PodcastBreak");
    Log("Podcast finished -- effect cleared\n");
}

static DWORD WINAPI WatcherThreadFunc(LPVOID) {
    char buf[64];
    for (;;) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n = recvfrom(g_ControlSocket, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == static_cast<int>(sizeof(Mad2PodcastControlPacket))) {
            auto* pkt = reinterpret_cast<Mad2PodcastControlPacket*>(buf);
            if (pkt->magic == MAD2PODCAST_CONTROL_MAGIC && pkt->type == MAD2PODCAST_CONTROL_FINISHED) break;
        } else if (n == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT) {
            Log("[Podcast] control recv error %d\n", WSAGetLastError());
        }
        if (!g_Active.load()) return 0;  // ClearPodcastBreak already owns teardown, nothing left to report
    }
    OnPodcastFinished();
    return 0;
}

// ---------------------------------------------------------------------
// mad2effects Apply/Clear.
// ---------------------------------------------------------------------

static void WINAPI ApplyPodcastBreak(void*) {
    bool expected = false;
    if (!g_Active.compare_exchange_strong(expected, true)) {
        Log("PodcastBreak already active -- ignoring re-trigger\n");
        return;
    }

    if (!SetupSocket()) {
        Log("Failed to set up UDP socket -- aborting PodcastBreak\n");
        g_Active.store(false);
        return;
    }

    SendTrigger();
    g_WatcherThread = CreateThread(nullptr, 0, WatcherThreadFunc, nullptr, 0, nullptr);
    Log("PodcastBreak applied (trigger sent to mad2music on port %d)\n", g_Config.controlPort);
}

static void WINAPI ClearPodcastBreak(void*) {
    bool expected = true;
    if (!g_Active.compare_exchange_strong(expected, false)) return;  // already inactive (self-clear path already ran) -- no-op

    // No STOP message in v1 -- see the protocol header's file comment for
    // why (podcast clips are short, not worth the extra state). mad2music
    // just keeps playing the clip out; this side stops waiting for it.
    if (g_WatcherThread) {
        WaitForSingleObject(g_WatcherThread, 2000);
        CloseHandle(g_WatcherThread);
        g_WatcherThread = nullptr;
    }
    Log("PodcastBreak cleared\n");
}

// ---------------------------------------------------------------------
// Manual test-trigger keybind -- same shape as mad2sm64mod's TestTriggerKey.
// ---------------------------------------------------------------------

static DWORD WINAPI TestTriggerThreadFunc(LPVOID) {
    bool wasPressed = false;
    for (;;) {
        Sleep(33);
        if (g_Config.testTriggerVk <= 0) continue;
        bool isPressed = (GetAsyncKeyState(g_Config.testTriggerVk) & 0x8000) != 0;
        if (isPressed && !wasPressed) {
            Log("TestTriggerKey pressed -- forcing PodcastBreak\n");
            const auto& effects = Mad2Effects_Resolve();
            if (effects.Trigger) effects.Trigger("PodcastBreak", 0);
        }
        wasPressed = isPressed;
    }
}

// ---------------------------------------------------------------------
// Init-only background thread: retries until every dependency is
// resolvable, then registers the effect and exits.
// ---------------------------------------------------------------------

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
            Log("[Dependency] still waiting on: %s%s (retrying indefinitely)\n", configOk ? "" : "mad2config ",
                effectsOk ? "" : "mad2effects ");
        }
        Sleep(kRetryDelayMs);
    }

    LoadConfig();

    // Native-Windows-only: get mad2music running for the session since
    // there's no mad2relauncher here to do it (see file header). Under
    // Wine, mad2relauncher already owns this (music.go) -- don't double-
    // launch a second instance from inside the Wine process too.
    if (!IsRunningUnderWine()) StartMad2MusicIfNeeded_NativeWindows();

    Mad2EffectDesc desc{};
    desc.name = "PodcastBreak";
    desc.displayName = "Podcast Break";
    desc.defaultWeight = g_Config.weight;
    desc.apply = ApplyPodcastBreak;
    desc.clear = ClearPodcastBreak;
    desc.defaultMinDurationMs = 0;
    desc.defaultMaxDurationMs = 0;  // no auto-revert timer -- self-clears when mad2music reports FINISHED
    BOOL ok = Mad2Effects_Resolve().Register(&desc);
    Log("Mad2Effects_Register(\"PodcastBreak\") -> %d\n", ok);

    HANDLE testTriggerThread = CreateThread(nullptr, 0, TestTriggerThreadFunc, nullptr, 0, nullptr);
    if (testTriggerThread) CloseHandle(testTriggerThread);

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            HANDLE h = CreateThread(nullptr, 0, InitThreadFunc, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
            break;
        }
        case DLL_PROCESS_DETACH:
            if (g_Active.load()) ClearPodcastBreak(nullptr);
            if (g_WinsockReady) {
                if (g_ControlSocket != INVALID_SOCKET) closesocket(g_ControlSocket);
                WSACleanup();
            }
            break;
        default:
            break;
    }
    return TRUE;
}
