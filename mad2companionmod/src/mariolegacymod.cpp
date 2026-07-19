// Super Mario Legacy chaos effect for Madagascar 2 -- "MarioLegacyChallenge",
// third sibling of jak1mod.cpp's JakChallenge and sm64mod.cpp's
// SM64Challenge, living in the same DLL (mad2companionmod.dll). Targets the
// vendored extern/mario-legacy (OpenGOAL-Mods/Super-Mario-Legacy release
// v0.0.28), a *complete separate* fork of jak-project that swaps Mario in
// for Jak across the whole game (own gk/goalc/extractor, own goal_src,
// libsm64 statically linked into gk -- confirmed via `strings gk`) -- it is
// NOT something layered onto our own extern/jak-project checkout, the two
// are unrelated builds from unrelated upstream trees. See jak1mod.cpp's file
// header for the fuller version of the shared reasoning below; only the
// differences are called out here.
//
// Why this file is (almost) a copy of jak1mod.cpp rather than sharing code
// with it: confirmed live this session that extern/mario-legacy's own
// goal_src/jak1/engine/ps2/pad.gc is byte-identical to stock jak1's aside
// from the exact same controller-injection patch already in
// extern/jak-project (diffed directly) -- Mario's moveset (mario.gc reads
// buttons via cpad-hold?/cpad-pressed? against the same button symbols:
// x/square/circle/l1/l2/r1/l3, i.e. the same PS2 pad-buttons bitfield, not a
// different wire contract) is built entirely on the same cpad-info struct
// service-cpads already populates, so no new translation logic was needed --
// jak1mod.cpp's own XInputToPs2Pad logic is a drop-in match, byte-for-byte
// duplicated below (its copy is `static`/file-local and operates on a
// distinct nominal packet type, so it can't be called directly from here) --
// matching this DLL's existing SM64/Jak1 precedent of duplicating small
// platform shims rather than factoring them out (see sm64mod.cpp's own
// "Platform detection" comment).
//
// Known gap, NOT fixed here (out of scope for this round, flagged clearly):
// unlike extern/jak-project, extern/mario-legacy's game-info.gc/level.gc/
// kernel-defs.gc do NOT have our title-screen-skip / *kernel-boot-level*
// patch (diffed live -- entirely absent upstream in this fork). So neither
// mad2relauncher's -level flag (Wine branch) nor the native-Windows branch
// below bother passing one -- the game boots to Mario Legacy's own title
// screen instead of jumping straight to a checkpoint either way, the same
// way JakChallenge did before progress.md's Round 6 ported that patch into
// extern/jak-project. Porting the equivalent patch into this fork's
// differently-structured game-info.gc (it already carries its own
// libsm64-specific hooks in the same functions -- see that file's diff) is
// a distinct follow-up, not attempted this round.
//
// Auto-exit: for the same reason (no *kernel-boot-level*-gated
// kernel-shutdown-on-power-cell patch in this fork), MarioLegacyChallenge
// has no equivalent of JakChallenge's "collect one power cell, game exits
// itself" behavior either -- it only self-clears when the player manually
// quits gk, exactly like SM64Challenge's own watcher (which never had that
// behavior to begin with).
//
// Native-Windows branch (added after this round's initial Wine-only
// version): unlike JakChallenge (whose upstream open-goal/jak-project needs
// a genuinely different toolchain -- MSVC or Windows-native clang, x86_64,
// no MinGW path at all -- to build for Windows, investigated and not
// attempted here), OpenGOAL-Mods/Super-Mario-Legacy's GitHub releases
// already ship prebuilt Windows binaries (gk.exe/goalc.exe/extractor.exe)
// alongside the Linux ones for the same tag (v0.0.28) -- confirmed the
// `data/` asset tree is byte-identical between the two platform archives
// (same file count, matching checksums), so extern/mario-legacy/ just
// vendors the three .exe files alongside the existing Linux binaries and
// shared data/, no separate directory needed. This means, unlike
// JakChallenge, MarioLegacyChallenge CAN launch directly via CreateProcess
// on real Windows -- same shape as SM64Challenge's native branch, except gk
// (like Jak1's) has no socket primitive, so instead of streaming UDP to a
// relauncher-owned control port, this mod itself atomically writes the
// polled mad2_input.dat file gk's pad.gc patch reads every frame -- see
// WriteInputFileAtomic below, replacing what mad2relauncher/mariolegacy.go
// does under Wine.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2effects/include/mad2effects_api.h"
#include "../../mad2xinput/include/mad2xinput_api.h"
#include "../include/mad2mariolegacy_protocol.h"
#include "jak1_init.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Companion_LogV("[MarioLegacy] ", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config -- shares mad2config.dll's [Companion] section, MarioLegacy_-
// prefixed keys, matching the SM64_/Jak1_ convention already established.
// ---------------------------------------------------------------------

static const char kDefaultWindowsMarioLegacyPath[] = "mario-legacy\\gk.exe";

static struct Config {
    DWORD userIndex = 0;
    bool broadcastToAllControllers = true;
    int weight = 5;
    int inputPort = MAD2MARIOLEGACY_DEFAULT_INPUT_PORT;
    int controlPort = MAD2MARIOLEGACY_DEFAULT_CONTROL_PORT;
    char windowsMarioLegacyPath[MAX_PATH];
    int testTriggerVk = VK_F12;  // manual on-demand trigger, independent of mad2chaosmod's weighted RNG
} g_Config;

static void LoadConfig() {
    strncpy(g_Config.windowsMarioLegacyPath, kDefaultWindowsMarioLegacyPath, sizeof(g_Config.windowsMarioLegacyPath) - 1);
    g_Config.windowsMarioLegacyPath[sizeof(g_Config.windowsMarioLegacyPath) - 1] = 0;

    const auto& api = Mad2Config_Resolve();
    if (!api.GetInt) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in defaults\n");
        return;
    }

    g_Config.userIndex = static_cast<DWORD>(api.GetInt(
        "Companion", "MarioLegacy_UserIndex", static_cast<int>(g_Config.userIndex),
        "Which controller (XInput user index, 0-3) MarioLegacyChallenge sources its\n"
        "streamed input from when MarioLegacy_BroadcastToAllControllers is false."));
    g_Config.broadcastToAllControllers = api.GetBool(
        "Companion", "MarioLegacy_BroadcastToAllControllers", g_Config.broadcastToAllControllers ? TRUE : FALSE,
        "Block Mad2's input on all 4 possible XInput slots (not just\n"
        "MarioLegacy_UserIndex) while MarioLegacyChallenge is active -- same\n"
        "Steam-Input-multi-slot rationale as [InputEffects] BroadcastToAllControllers.") != FALSE;
    g_Config.weight =
        api.GetInt("Companion", "MarioLegacy_Weight", g_Config.weight, "MarioLegacyChallenge: chance-pool weight.");
    g_Config.inputPort = api.GetInt(
        "Companion", "MarioLegacy_InputPort", g_Config.inputPort,
        "Loopback UDP port mad2companionmod streams PS2-pad-format controller state to\n"
        "mad2relauncher on for MarioLegacyChallenge. Must match mad2relauncher's\n"
        "-mariolegacy-input-port flag.");
    g_Config.controlPort = api.GetInt(
        "Companion", "MarioLegacy_ControlPort", g_Config.controlPort,
        "Loopback UDP port mad2companionmod uses to ask mad2relauncher to launch/stop\n"
        "Mario Legacy. Must match mad2relauncher's -mariolegacy-control-port flag.");
    g_Config.testTriggerVk = api.GetVirtualKey(
        "Companion", "MarioLegacy_TestTriggerKey", g_Config.testTriggerVk,
        "Keyboard key that immediately triggers MarioLegacyChallenge on demand,\n"
        "bypassing mad2chaosmod's weighted RNG entirely -- see SM64_TestTriggerKey for\n"
        "the same idea. Set to an unused code (e.g. 0) to disable.\n"
        "See https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes");

    char pathBuf[MAX_PATH];
    api.GetString("Companion", "MarioLegacy_WindowsPath", kDefaultWindowsMarioLegacyPath,
                   "Path to a Windows gk.exe from OpenGOAL-Mods/Super-Mario-Legacy's own GitHub\n"
                   "releases (only used when NOT running under Wine -- see extern/mario-legacy,\n"
                   "which vendors gk.exe/goalc.exe/extractor.exe alongside the Linux binaries).",
                   pathBuf, sizeof(pathBuf));
    strncpy(g_Config.windowsMarioLegacyPath, pathBuf, sizeof(g_Config.windowsMarioLegacyPath) - 1);
    g_Config.windowsMarioLegacyPath[sizeof(g_Config.windowsMarioLegacyPath) - 1] = 0;

    Log("Config loaded (userIndex=%lu, inputPort=%d, controlPort=%d, windowsMarioLegacyPath=%s, testTriggerVk=0x%X)\n",
        g_Config.userIndex, g_Config.inputPort, g_Config.controlPort, g_Config.windowsMarioLegacyPath,
        g_Config.testTriggerVk);
}

// ---------------------------------------------------------------------
// Platform detection -- duplicated the same way sm64mod.cpp/jak1mod.cpp do.
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
// Shared state.
// ---------------------------------------------------------------------

static std::atomic<bool> g_Active{false};
static std::atomic<uint32_t> g_Seq{0};

static bool g_WinsockReady = false;
static SOCKET g_InputSocket = INVALID_SOCKET;
static SOCKET g_ControlSocket = INVALID_SOCKET;
static sockaddr_in g_InputTargetAddr{};
static sockaddr_in g_RelauncherAddr{};

static HANDLE g_StreamThread = nullptr;
static HANDLE g_StreamStopEvent = nullptr;
static HANDLE g_WatcherThread = nullptr;
static PROCESS_INFORMATION g_MarioLegacyProcess{};  // native Windows branch only
static char g_InputFilePath[MAX_PATH] = {0};        // native Windows branch only -- see WriteInputFileAtomic

struct HandleSet {
    Mad2XInputOverrideHandle h[XUSER_MAX_COUNT] = {};
};
static HandleSet g_BlockHandles;

// ---------------------------------------------------------------------
// Input block -- identical shape to SM64Challenge/JakChallenge's own.
// ---------------------------------------------------------------------

static void ApplyInputBlock() {
    const auto& api = Mad2XInput_Resolve();
    if (!api.AddOverride) {
        Log("[Dependency] mad2xinput.dll not available -- cannot block Mad2 input for MarioLegacyChallenge\n");
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
    if (g_Config.broadcastToAllControllers) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) addOne(i);
    } else {
        addOne(g_Config.userIndex);
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

static bool SetupSockets() {
    if (!EnsureWinsock()) return false;

    if (g_InputSocket == INVALID_SOCKET) {
        g_InputSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        ZeroMemory(&g_InputTargetAddr, sizeof(g_InputTargetAddr));
        g_InputTargetAddr.sin_family = AF_INET;
        g_InputTargetAddr.sin_port = htons(static_cast<u_short>(g_Config.inputPort));
        g_InputTargetAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    }
    if (g_ControlSocket == INVALID_SOCKET) {
        g_ControlSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
        bindAddr.sin_port = 0;
        bind(g_ControlSocket, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr));

        DWORD timeoutMs = 200;
        setsockopt(g_ControlSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        ZeroMemory(&g_RelauncherAddr, sizeof(g_RelauncherAddr));
        g_RelauncherAddr.sin_family = AF_INET;
        g_RelauncherAddr.sin_port = htons(static_cast<u_short>(g_Config.controlPort));
        g_RelauncherAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    }
    return g_InputSocket != INVALID_SOCKET && g_ControlSocket != INVALID_SOCKET;
}

static void SendControl(Mad2MarioLegacyControlType type) {
    if (g_ControlSocket == INVALID_SOCKET) return;
    Mad2MarioLegacyControlPacket pkt{MAD2MARIOLEGACY_CONTROL_MAGIC, static_cast<uint32_t>(type)};
    sendto(g_ControlSocket, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
           reinterpret_cast<sockaddr*>(&g_RelauncherAddr), sizeof(g_RelauncherAddr));
}

// ---------------------------------------------------------------------
// XInput -> PS2 pad translation. Byte-for-byte the same logic as
// jak1mod.cpp's own XInputToPs2Pad/Ps2StickByte (this fork's pad.gc reads
// the identical PS2 pad-buttons bitfield -- see file header) -- duplicated
// here rather than shared across translation units (jak1mod.cpp's copy is
// `static`, i.e. file-local, and the two packet struct types are distinct
// nominal types despite identical layout), matching this DLL's existing
// SM64/Jak1 precedent of duplicating small platform shims rather than
// factoring them out.
// ---------------------------------------------------------------------

static uint8_t Ps2StickByte(SHORT value, bool invertForY) {
    int32_t v = invertForY ? (32767 - static_cast<int32_t>(value)) : (static_cast<int32_t>(value) + 32768);
    if (v < 0) v = 0;
    if (v > 0xFFFF) v = 0xFFFF;
    return static_cast<uint8_t>(v >> 8);
}

static void XInputToPs2Pad(const XINPUT_STATE& state, Mad2MarioLegacyInputPacket& out) {
    const auto& gp = state.Gamepad;
    uint16_t b = 0;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_UP) b |= MAD2MARIOLEGACY_PAD_UP;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) b |= MAD2MARIOLEGACY_PAD_DOWN;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) b |= MAD2MARIOLEGACY_PAD_LEFT;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) b |= MAD2MARIOLEGACY_PAD_RIGHT;
    if (gp.wButtons & XINPUT_GAMEPAD_START) b |= MAD2MARIOLEGACY_PAD_START;
    if (gp.wButtons & XINPUT_GAMEPAD_BACK) b |= MAD2MARIOLEGACY_PAD_SELECT;
    if (gp.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) b |= MAD2MARIOLEGACY_PAD_L3;
    if (gp.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) b |= MAD2MARIOLEGACY_PAD_R3;
    if (gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= MAD2MARIOLEGACY_PAD_L1;
    if (gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= MAD2MARIOLEGACY_PAD_R1;
    // A=cross, B=circle, X=square, Y=triangle -- same convention as
    // jak1mod.cpp's XInputToPs2Pad; mario.gc's ground-pound/crouch/long-jump
    // combos are all built from these same four face buttons plus
    // L1/L2/R1/R2 (confirmed via `grep cpad-hold?/cpad-pressed?` over
    // mario.gc), so no additional mapping is needed for Mario's extra moves.
    if (gp.wButtons & XINPUT_GAMEPAD_A) b |= MAD2MARIOLEGACY_PAD_X;
    if (gp.wButtons & XINPUT_GAMEPAD_B) b |= MAD2MARIOLEGACY_PAD_CIRCLE;
    if (gp.wButtons & XINPUT_GAMEPAD_X) b |= MAD2MARIOLEGACY_PAD_SQUARE;
    if (gp.wButtons & XINPUT_GAMEPAD_Y) b |= MAD2MARIOLEGACY_PAD_TRIANGLE;
    if (gp.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= MAD2MARIOLEGACY_PAD_L2;
    if (gp.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= MAD2MARIOLEGACY_PAD_R2;

    out.button0 = b;
    out.leftx = Ps2StickByte(gp.sThumbLX, false);
    out.lefty = Ps2StickByte(gp.sThumbLY, true);
    out.rightx = Ps2StickByte(gp.sThumbRX, false);
    out.righty = Ps2StickByte(gp.sThumbRY, true);
}

// ---------------------------------------------------------------------
// Input streaming. Reads mad2xinput's RAW state (Mad2XInput_GetState), not
// post-override -- same fix/reasoning as jak1mod.cpp/sm64mod.cpp's own
// ReadEffectiveOverriddenState (see that file's comment for the full
// GetOverriddenState-reads-its-own-block bug this avoids).
// ---------------------------------------------------------------------

static bool ReadEffectiveState(XINPUT_STATE& out) {
    const auto& api = Mad2XInput_Resolve();
    if (!api.GetState) return false;
    if (api.GetState(g_Config.userIndex, &out)) return true;
    if (!g_Config.broadcastToAllControllers) return false;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (i == g_Config.userIndex) continue;
        if (api.GetState(i, &out)) return true;
    }
    return false;
}

// Native-Windows-only: atomically overwrites g_InputFilePath with the raw
// packet bytes, same write-to-.tmp-then-rename pattern
// mad2relauncher/mariolegacy.go's writeInputFileAtomic uses on the Wine
// side (see that file) -- gk's pad.gc patch polls this exact path once per
// frame regardless of which side of the wire is writing it, so the file
// format/location must match exactly. MoveFileExA with
// MOVEFILE_REPLACE_EXISTING is the Win32 equivalent of Go's os.Rename over
// an existing destination.
static void WriteInputFileAtomic(const Mad2MarioLegacyInputPacket& pkt) {
    if (!g_InputFilePath[0]) return;
    char tmpPath[MAX_PATH + 4];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", g_InputFilePath);

    HANDLE f = CreateFileA(tmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, &pkt, sizeof(pkt), &written, nullptr);
    CloseHandle(f);
    if (written != sizeof(pkt)) return;

    MoveFileExA(tmpPath, g_InputFilePath, MOVEFILE_REPLACE_EXISTING);
}

static DWORD WINAPI StreamThreadFunc(LPVOID) {
    bool wine = IsRunningUnderWine();
    while (WaitForSingleObject(g_StreamStopEvent, 8) != WAIT_OBJECT_0) {
        XINPUT_STATE state;
        if (!ReadEffectiveState(state)) continue;

        Mad2MarioLegacyInputPacket pkt{};
        pkt.magic = MAD2MARIOLEGACY_INPUT_MAGIC;
        pkt.seq = g_Seq.fetch_add(1);
        XInputToPs2Pad(state, pkt);
        if (wine) {
            sendto(g_InputSocket, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
                   reinterpret_cast<sockaddr*>(&g_InputTargetAddr), sizeof(g_InputTargetAddr));
        } else {
            WriteInputFileAtomic(pkt);
        }
    }
    return 0;
}

static void StartStreaming() {
    g_Seq.store(0);
    g_StreamStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_StreamThread = CreateThread(nullptr, 0, StreamThreadFunc, nullptr, 0, nullptr);
}

static void StopStreaming() {
    if (g_StreamStopEvent) SetEvent(g_StreamStopEvent);
    if (g_StreamThread) {
        WaitForSingleObject(g_StreamThread, 2000);
        CloseHandle(g_StreamThread);
        g_StreamThread = nullptr;
    }
    if (g_StreamStopEvent) {
        CloseHandle(g_StreamStopEvent);
        g_StreamStopEvent = nullptr;
    }
}

// ---------------------------------------------------------------------
// Native-Windows-only: launch gk.exe directly and best-effort focus it.
// Same shape as sm64mod.cpp's LaunchWindowsSm64/FocusSm64WindowThreadFunc.
// ---------------------------------------------------------------------

static bool LaunchWindowsMarioLegacy() {
    char workDir[MAX_PATH];
    strncpy(workDir, g_Config.windowsMarioLegacyPath, sizeof(workDir) - 1);
    workDir[sizeof(workDir) - 1] = 0;
    char* lastSlash = strrchr(workDir, '\\');
    if (!lastSlash) lastSlash = strrchr(workDir, '/');
    if (lastSlash) *lastSlash = 0;
    else workDir[0] = 0;

    // g_InputFilePath = <workDir>\data\mad2_input.dat -- same "data/"
    // subdirectory quirk as the Wine branch (see mad2relauncher/
    // mariolegacy.go's inputPath comment: this fork's GOAL file-stream
    // opens resolve relative to <gk's cwd>/data/, unlike extern/jak-
    // project's top-level layout).
    snprintf(g_InputFilePath, sizeof(g_InputFilePath), "%s\\data\\mad2_input.dat", workDir[0] ? workDir : ".");
    DeleteFileA(g_InputFilePath);  // stale file from a previous session shouldn't be read before the first write

    // No -level here -- see file header's "Known gap" note, this fork has
    // no title-screen-skip patch so it would be a no-op either way, same as
    // the Wine branch (mad2relauncher/mariolegacy.go still passes one there
    // only because it shares jak1Checkpoints with JakChallenge's call
    // site -- not because it does anything for this effect specifically).
    char saveDir[MAX_PATH];
    snprintf(saveDir, sizeof(saveDir), "%s\\saves_chaos", workDir[0] ? workDir : ".");
    char cmdLine[2 * MAX_PATH + 64];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\" --game jak1 --config-path \"%s\" -- -boot -fakeiso",
             g_Config.windowsMarioLegacyPath, saveDir);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    ZeroMemory(&g_MarioLegacyProcess, sizeof(g_MarioLegacyProcess));
    BOOL ok = CreateProcessA(g_Config.windowsMarioLegacyPath, cmdLine, nullptr, nullptr, FALSE, 0, nullptr,
                              workDir[0] ? workDir : nullptr, &si, &g_MarioLegacyProcess);
    if (!ok) {
        Log("CreateProcess(\"%s\") failed, err=%lu\n", g_Config.windowsMarioLegacyPath, GetLastError());
        return false;
    }
    Log("Launched native Windows Mario Legacy: %s (pid=%lu)\n", g_Config.windowsMarioLegacyPath,
        g_MarioLegacyProcess.dwProcessId);
    return true;
}

// Fire-and-forget, same shape/timeout as sm64mod.cpp's
// FocusSm64WindowThreadFunc -- "OpenGOAL" is gk's own window title
// (confirmed via progress.md's Round 4/6 X11 investigation logging the same
// title for extern/jak-project's gk; this fork is the same jak-project-
// derived engine so its window title is unchanged).
static DWORD WINAPI FocusMarioLegacyWindowThreadFunc(LPVOID param) {
    HWND mad2Wnd = reinterpret_cast<HWND>(param);
    for (int i = 0; i < 100 && g_Active.load(); ++i) {
        HWND wnd = FindWindowA(nullptr, "OpenGOAL");
        if (wnd) {
            if (mad2Wnd) ShowWindow(mad2Wnd, SW_MINIMIZE);
            ShowWindow(wnd, SW_SHOW);
            SetForegroundWindow(wnd);
            return 0;
        }
        Sleep(100);
    }
    return 0;
}

// ---------------------------------------------------------------------
// Teardown coordination -- same shape as sm64mod.cpp/jak1mod.cpp.
// ---------------------------------------------------------------------

static void OnMarioLegacyProcessGone() {
    bool expected = true;
    if (!g_Active.compare_exchange_strong(expected, false)) return;

    StopStreaming();
    RemoveInputBlock();
    if (g_MarioLegacyProcess.hProcess) {
        CloseHandle(g_MarioLegacyProcess.hProcess);
        CloseHandle(g_MarioLegacyProcess.hThread);
        g_MarioLegacyProcess = {};
    }

    const auto& effects = Mad2Effects_Resolve();
    if (effects.ClearByName) effects.ClearByName("MarioLegacyChallenge");
    Log("Mario Legacy exited on its own -- effect cleared\n");
}

static DWORD WINAPI WatcherThreadFunc_Wine(LPVOID) {
    char buf[64];
    for (;;) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n = recvfrom(g_ControlSocket, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == static_cast<int>(sizeof(Mad2MarioLegacyControlPacket))) {
            auto* pkt = reinterpret_cast<Mad2MarioLegacyControlPacket*>(buf);
            if (pkt->magic == MAD2MARIOLEGACY_CONTROL_MAGIC && pkt->type == MAD2MARIOLEGACY_CONTROL_EXITED) break;
        } else if (n == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT) {
            Log("control recv error %d\n", WSAGetLastError());
        }
        if (!g_Active.load()) return 0;
    }
    OnMarioLegacyProcessGone();
    return 0;
}

static DWORD WINAPI WatcherThreadFunc_Native(LPVOID) {
    WaitForSingleObject(g_MarioLegacyProcess.hProcess, INFINITE);
    OnMarioLegacyProcessGone();
    return 0;
}

// ---------------------------------------------------------------------
// mad2effects Apply/Clear.
// ---------------------------------------------------------------------

static void WINAPI ApplyMarioLegacyChallenge(void*) {
    bool expected = false;
    if (!g_Active.compare_exchange_strong(expected, true)) {
        Log("MarioLegacyChallenge already active -- ignoring re-trigger\n");
        return;
    }

    bool wine = IsRunningUnderWine();
    if (wine) {
        if (!SetupSockets()) {
            Log("Failed to set up UDP sockets -- aborting MarioLegacyChallenge\n");
            g_Active.store(false);
            return;
        }
    }

    ApplyInputBlock();
    StartStreaming();

    bool launched;
    if (wine) {
        SendControl(MAD2MARIOLEGACY_CONTROL_LAUNCH);
        launched = true;  // fire-and-forget -- mad2relauncher logs/handles launch failures on its own side
    } else {
        launched = LaunchWindowsMarioLegacy();
        if (launched) {
            HANDLE t = CreateThread(nullptr, 0, FocusMarioLegacyWindowThreadFunc, GetForegroundWindow(), 0, nullptr);
            if (t) CloseHandle(t);
        }
    }

    if (!launched) {
        StopStreaming();
        RemoveInputBlock();
        g_Active.store(false);
        return;
    }

    g_WatcherThread =
        CreateThread(nullptr, 0, wine ? WatcherThreadFunc_Wine : WatcherThreadFunc_Native, nullptr, 0, nullptr);
    Log("MarioLegacyChallenge applied (%s)\n", wine ? "Wine -> mad2relauncher" : "native Windows CreateProcess");
}

static void WINAPI ClearMarioLegacyChallenge(void*) {
    bool expected = true;
    if (!g_Active.compare_exchange_strong(expected, false)) return;

    if (IsRunningUnderWine()) {
        SendControl(MAD2MARIOLEGACY_CONTROL_STOP);
    } else if (g_MarioLegacyProcess.hProcess) {
        TerminateProcess(g_MarioLegacyProcess.hProcess, 0);
    }
    if (g_WatcherThread) {
        WaitForSingleObject(g_WatcherThread, 2000);
        CloseHandle(g_WatcherThread);
        g_WatcherThread = nullptr;
    }
    if (g_MarioLegacyProcess.hProcess) {
        CloseHandle(g_MarioLegacyProcess.hProcess);
        CloseHandle(g_MarioLegacyProcess.hThread);
        g_MarioLegacyProcess = {};
    }

    StopStreaming();
    RemoveInputBlock();
    Log("MarioLegacyChallenge cleared\n");
}

// ---------------------------------------------------------------------
// Manual test-trigger keybind.
// ---------------------------------------------------------------------

static DWORD WINAPI TestTriggerThreadFunc(LPVOID) {
    bool wasPressed = false;
    for (;;) {
        Sleep(33);
        if (g_Config.testTriggerVk <= 0) continue;
        bool isPressed = (GetAsyncKeyState(g_Config.testTriggerVk) & 0x8000) != 0;
        if (isPressed && !wasPressed) {
            Log("TestTriggerKey pressed -- forcing MarioLegacyChallenge\n");
            const auto& effects = Mad2Effects_Resolve();
            if (effects.Trigger) effects.Trigger("MarioLegacyChallenge", 0);
        }
        wasPressed = isPressed;
    }
}

// ---------------------------------------------------------------------
// Called from sm64mod.cpp's InitThreadFunc, same as Jak1_RegisterEffect --
// see jak1_init.h.
// ---------------------------------------------------------------------

void MarioLegacy_RegisterEffect() {
    LoadConfig();

    Mad2EffectDesc desc{};
    desc.name = "MarioLegacyChallenge";
    desc.displayName = "Super Mario Legacy Challenge";
    desc.defaultWeight = g_Config.weight;
    desc.apply = ApplyMarioLegacyChallenge;
    desc.clear = ClearMarioLegacyChallenge;
    desc.defaultMinDurationMs = 0;
    desc.defaultMaxDurationMs = 0;  // no auto-revert timer -- self-clears when the process exits
    BOOL ok = Mad2Effects_Resolve().Register(&desc);
    Log("Mad2Effects_Register(\"MarioLegacyChallenge\") -> %d\n", ok);

    HANDLE testTriggerThread = CreateThread(nullptr, 0, TestTriggerThreadFunc, nullptr, 0, nullptr);
    if (testTriggerThread) CloseHandle(testTriggerThread);
}

void MarioLegacy_Shutdown() {
    if (g_Active.load()) ClearMarioLegacyChallenge(nullptr);
    if (g_WinsockReady) {
        if (g_InputSocket != INVALID_SOCKET) closesocket(g_InputSocket);
        if (g_ControlSocket != INVALID_SOCKET) closesocket(g_ControlSocket);
        WSACleanup();
    }
}
