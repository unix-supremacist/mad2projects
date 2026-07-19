// Jak & Daxter chaos effect for Madagascar 2 -- the "JakChallenge" sibling
// of sm64mod.cpp's SM64Challenge, living in the same DLL (mad2companionmod.dll,
// formerly mad2sm64mod.dll -- see CMakeLists.txt) since both are small
// "launch a separate game process as a chaos effect" mods sharing the same
// mad2xinput-input-block / mad2effects-registration shape. See
// jak1_init.h's declaration and sm64mod.cpp's DllMain for how the two
// effects' registration is wired together from one init thread.
//
// Same two-platform-branch reasoning as SM64Challenge would apply, except
// there is only one branch here: the vendored extern/jak-project (an
// open-goal/jak-project checkout, built via `just build-jak1-linux`) has no
// Windows build path of its own, so JakChallenge is Wine-only: under Wine
// it sends a LAUNCH control packet to mad2relauncher (../mad2relauncher,
// see jak1.go), which forks the source-built `gk` and handles window-focus
// juggling, exactly like SM64Challenge's Wine branch. On native Windows
// this effect logs once and no-ops -- see ApplyJakChallenge.
//
// Controller passthrough: mad2relauncher cannot feed input to gk over a
// socket (gk has no such extern -- see include/mad2jak1_protocol.h's file
// header for the full explanation), so mad2relauncher instead atomically
// overwrites a small file that a hand-written patch in
// extern/jak-project/goal_src/jak1/engine/ps2/pad.gc's service-cpads polls
// once per frame. To keep that GOAL patch a handful of `set!` lines (GOAL
// has no business doing XInput-bitmask-to-PS2-pad-bitmask translation),
// THIS file does the translation, in C++, before ever sending a packet --
// see XInputToPs2Pad below. mad2relauncher and the GOAL patch on the other
// end just relay/copy bytes.
//
// Auto-exit: unlike SM64Challenge (which relies purely on watching the
// child process's own exit), JakChallenge's "challenge" framing is real --
// a hand patch in extern/jak-project/goal_src/jak1/engine/game/game-info.gc
// (adjust-item, fuel-cell case) already calls (kernel-shutdown) the moment a
// power cell is collected while *kernel-boot-level* is set (which
// mad2relauncher always sets, via -level <random checkpoint>). So "collect
// one power cell, game exits itself" is already the natural behavior of a
// -level boot -- this mod doesn't need to detect the power-cell pickup
// itself, only watch for the process exiting, exactly like SM64Challenge's
// OnSm64ProcessGone/WatcherThreadFunc_Wine.
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
#include "../include/mad2jak1_protocol.h"
#include "jak1_init.h"

// ---------------------------------------------------------------------
// Logging -- shares mad2companionmod.log rather than opening its own file,
// since this is still one DLL (one log file per *DLL*, not per source file
// -- see CLAUDE.md's "Logging conventions"). Forwards to sm64mod.cpp's
// mutex-guarded Companion_LogV (see jak1_init.h) instead of keeping an
// independent FILE* -- two unsynchronized handles onto the same filename
// (one "w"-truncate, one "a"-append) used to be a real interleaving/
// truncation hazard here (improvement-plan.md item 14).
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Companion_LogV("[Jak1] ", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config -- shares mad2config.dll's [Companion] section with sm64mod.cpp's
// SM64Challenge settings (SM64_* keys) rather than each effect owning its
// own separate section -- see that file's LoadConfig comment for the full
// reasoning. Keys prefixed "Jak1_" to stay distinguishable within the
// shared section.
// ---------------------------------------------------------------------

// static: this struct/instance name collides with sm64mod.cpp's own
// (identically-named, for the same mirroring reasons) -- both need internal
// linkage since they're now two translation units in one DLL.
static struct Config {
    DWORD userIndex = 0;
    bool broadcastToAllControllers = true;
    int weight = 5;
    int inputPort = MAD2JAK1_DEFAULT_INPUT_PORT;
    int controlPort = MAD2JAK1_DEFAULT_CONTROL_PORT;
    int testTriggerVk = VK_F11;  // manual on-demand trigger, independent of mad2chaosmod's weighted RNG
} g_Config;

static void LoadConfig() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetInt) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in defaults\n");
        return;
    }

    g_Config.userIndex = static_cast<DWORD>(api.GetInt(
        "Companion", "Jak1_UserIndex", static_cast<int>(g_Config.userIndex),
        "Which controller (XInput user index, 0-3) JakChallenge sources its streamed\n"
        "post-override input from when Jak1_BroadcastToAllControllers is false."));
    g_Config.broadcastToAllControllers =
        api.GetBool("Companion", "Jak1_BroadcastToAllControllers", g_Config.broadcastToAllControllers ? TRUE : FALSE,
                     "Block Mad2's input on all 4 possible XInput slots (not just Jak1_UserIndex)\n"
                     "while JakChallenge is active -- same Steam-Input-multi-slot rationale as\n"
                     "[InputEffects] BroadcastToAllControllers.") != FALSE;
    g_Config.weight = api.GetInt("Companion", "Jak1_Weight", g_Config.weight, "JakChallenge: chance-pool weight.");
    g_Config.inputPort = api.GetInt(
        "Companion", "Jak1_InputPort", g_Config.inputPort,
        "Loopback UDP port mad2companionmod streams post-override controller state to\n"
        "mad2relauncher on (already translated to PS2 pad-buttons wire format -- see\n"
        "include/mad2jak1_protocol.h). Must match mad2relauncher's -jak1-input-port flag.");
    g_Config.controlPort = api.GetInt(
        "Companion", "Jak1_ControlPort", g_Config.controlPort,
        "Loopback UDP port mad2companionmod uses to ask mad2relauncher to launch/stop\n"
        "Jak1. Must match mad2relauncher's -jak1-control-port flag.");
    g_Config.testTriggerVk = api.GetVirtualKey(
        "Companion", "Jak1_TestTriggerKey", g_Config.testTriggerVk,
        "Keyboard key that immediately triggers JakChallenge on demand, bypassing\n"
        "mad2chaosmod's weighted RNG entirely -- see SM64_TestTriggerKey for the same\n"
        "idea. Set to an unused code (e.g. 0) to disable.\n"
        "See https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes");

    Log("Config loaded (userIndex=%lu, inputPort=%d, controlPort=%d, testTriggerVk=0x%X)\n", g_Config.userIndex,
        g_Config.inputPort, g_Config.controlPort, g_Config.testTriggerVk);
}

// ---------------------------------------------------------------------
// Platform detection -- duplicated from sm64mod.cpp rather than shared
// (both are tiny, self-contained checks; see CLAUDE.md's precedent for
// every IAT-hooking mod keeping its own copy rather than a shared library).
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
static SOCKET g_InputSocket = INVALID_SOCKET;    // mod -> mad2relauncher, input streaming
static SOCKET g_ControlSocket = INVALID_SOCKET;  // mod <-> mad2relauncher
static sockaddr_in g_InputTargetAddr{};
static sockaddr_in g_RelauncherAddr{};

static HANDLE g_StreamThread = nullptr;
static HANDLE g_StreamStopEvent = nullptr;
static HANDLE g_WatcherThread = nullptr;

struct HandleSet {
    Mad2XInputOverrideHandle h[XUSER_MAX_COUNT] = {};
};
static HandleSet g_BlockHandles;

// ---------------------------------------------------------------------
// Input block -- identical shape to SM64Challenge's ApplyInputBlock.
// ---------------------------------------------------------------------

static void ApplyInputBlock() {
    const auto& api = Mad2XInput_Resolve();
    if (!api.AddOverride) {
        Log("[Dependency] mad2xinput.dll not available -- cannot block Mad2 input for JakChallenge\n");
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
// Sockets -- same shape as sm64mod.cpp's SetupSockets/SendControl.
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
        bindAddr.sin_port = 0;  // ephemeral -- mad2relauncher replies to whatever source port our sends come from
        bind(g_ControlSocket, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr));

        // Same rationale as sm64mod.cpp's SetupSockets: short timeout so the
        // watcher thread's recvfrom loop notices g_Active flipping false
        // (an external Clear()) promptly instead of blocking indefinitely.
        DWORD timeoutMs = 200;
        setsockopt(g_ControlSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        ZeroMemory(&g_RelauncherAddr, sizeof(g_RelauncherAddr));
        g_RelauncherAddr.sin_family = AF_INET;
        g_RelauncherAddr.sin_port = htons(static_cast<u_short>(g_Config.controlPort));
        g_RelauncherAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    }
    return g_InputSocket != INVALID_SOCKET && g_ControlSocket != INVALID_SOCKET;
}

static void SendControl(Mad2Jak1ControlType type) {
    if (g_ControlSocket == INVALID_SOCKET) return;
    Mad2Jak1ControlPacket pkt{MAD2JAK1_CONTROL_MAGIC, static_cast<uint32_t>(type)};
    sendto(g_ControlSocket, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
           reinterpret_cast<sockaddr*>(&g_RelauncherAddr), sizeof(g_RelauncherAddr));
}

// ---------------------------------------------------------------------
// XInput -> PS2 pad translation. See mad2jak1_protocol.h's file header for
// why this lives here (sender side) rather than on the GOAL/mad2relauncher
// side of the wire.
// ---------------------------------------------------------------------

// engine/ps2/pad.gc's stick bytes: 0-255, 128 = centered. X: 0 = left,
// 255 = right (no inversion vs. XInput). Y: 0 = up/forward, 255 = down/back
// -- INVERTED vs. XInput's signed range (positive = up), matching
// service-cpads' stick0 direction math (see pad.gc: f28-0 uses
// (127 - lefty), i.e. smaller lefty = more "forward").
static uint8_t Ps2StickByte(SHORT value, bool invertForY) {
    int32_t v = invertForY ? (32767 - static_cast<int32_t>(value)) : (static_cast<int32_t>(value) + 32768);
    if (v < 0) v = 0;
    if (v > 0xFFFF) v = 0xFFFF;
    return static_cast<uint8_t>(v >> 8);
}

static void XInputToPs2Pad(const XINPUT_STATE& state, Mad2Jak1InputPacket& out) {
    const auto& gp = state.Gamepad;
    uint16_t b = 0;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_UP) b |= MAD2JAK1_PAD_UP;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) b |= MAD2JAK1_PAD_DOWN;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) b |= MAD2JAK1_PAD_LEFT;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) b |= MAD2JAK1_PAD_RIGHT;
    if (gp.wButtons & XINPUT_GAMEPAD_START) b |= MAD2JAK1_PAD_START;
    if (gp.wButtons & XINPUT_GAMEPAD_BACK) b |= MAD2JAK1_PAD_SELECT;
    if (gp.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) b |= MAD2JAK1_PAD_L3;
    if (gp.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) b |= MAD2JAK1_PAD_R3;
    if (gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= MAD2JAK1_PAD_L1;
    if (gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= MAD2JAK1_PAD_R1;
    // Standard Xbox-face-button -> PS2-face-button mapping (A=cross, B=circle,
    // X=square, Y=triangle) -- same convention mad2inputdisplay's HUD layout
    // and every other Xbox-labeled overlay in this repo already assumes.
    if (gp.wButtons & XINPUT_GAMEPAD_A) b |= MAD2JAK1_PAD_X;
    if (gp.wButtons & XINPUT_GAMEPAD_B) b |= MAD2JAK1_PAD_CIRCLE;
    if (gp.wButtons & XINPUT_GAMEPAD_X) b |= MAD2JAK1_PAD_SQUARE;
    if (gp.wButtons & XINPUT_GAMEPAD_Y) b |= MAD2JAK1_PAD_TRIANGLE;
    // Triggers: PS2 hw-cpad has genuine analog pressure (abutton[]) for
    // L2/R2, but engine/ps2/pad.gc's service-cpads only ever reads button0's
    // digital bit for them in this game's actual control scheme (jump/duck),
    // so a simple threshold (matching XINPUT_GAMEPAD_TRIGGER_THRESHOLD) is
    // enough -- no need to also wire up abutton[] pressure data.
    if (gp.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= MAD2JAK1_PAD_L2;
    if (gp.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= MAD2JAK1_PAD_R2;

    out.button0 = b;
    out.leftx = Ps2StickByte(gp.sThumbLX, false);
    out.lefty = Ps2StickByte(gp.sThumbLY, true);
    out.rightx = Ps2StickByte(gp.sThumbRX, false);
    out.righty = Ps2StickByte(gp.sThumbRY, true);
}

// ---------------------------------------------------------------------
// Input streaming -- same polling/broadcast-scan shape as sm64mod.cpp's
// ReadEffectiveOverriddenState/StreamThreadFunc, translating each sample
// before sending.
//
// Deliberately reads mad2xinput's RAW state (Mad2XInput_GetState), not the
// POST-override state (Mad2XInput_GetOverriddenState) the original design
// intended. Root-caused live: this exact bug is why "Jak1 controller input
// still not reaching the game" (and its SM64Challenge twin, changes.md item
// 13) never worked. ApplyInputBlock() below installs an AddOverride layer
// on this controller (forceBlockButtons=0xFFFF, sticks forced dead-center)
// specifically to stop Mad2's own game logic from reacting to the player's
// real input while Jak1 has focus -- but GetOverriddenState() returns state
// with *every* active layer applied, including that very block, so reading
// it back here always read our own just-applied zero, every poll,
// regardless of what the controller was actually doing. Confirmed via a
// full live end-to-end test (synthetic mad2relauncher UDP packets, real
// `gk` process, temporary service-cpads debug logging): the file-write/
// GOAL-read half of the pipeline was already correct; this was the actual
// gap upstream of it. Switching to GetState (cached pre-any-override,
// immune to our own or anyone else's layer) fixes real input flowing
// through; the tradeoff is other mods' XInput effects (JoystickReversal,
// SwapAB, ...) no longer compose into what Jak1 sees. Recovering that would
// need mad2xinput.dll itself to grow an "exclude this handle" variant of
// GetOverriddenState -- flagged for whoever owns that DLL, not done here.
// ---------------------------------------------------------------------

static bool ReadEffectiveOverriddenState(XINPUT_STATE& out) {
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

static DWORD WINAPI StreamThreadFunc(LPVOID) {
    while (WaitForSingleObject(g_StreamStopEvent, 8) != WAIT_OBJECT_0) {
        XINPUT_STATE state;
        if (!ReadEffectiveOverriddenState(state)) continue;

        Mad2Jak1InputPacket pkt{};
        pkt.magic = MAD2JAK1_INPUT_MAGIC;
        pkt.seq = g_Seq.fetch_add(1);
        XInputToPs2Pad(state, pkt);
        sendto(g_InputSocket, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
               reinterpret_cast<sockaddr*>(&g_InputTargetAddr), sizeof(g_InputTargetAddr));
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
// Teardown coordination -- same g_Active-arbitrates-a-single-teardown shape
// as sm64mod.cpp's OnSm64ProcessGone/ClearSM64Challenge; see that file's
// header comment for the full two-trigger race explanation, which applies
// here unchanged.
// ---------------------------------------------------------------------

static void OnJakProcessGone() {
    bool expected = true;
    if (!g_Active.compare_exchange_strong(expected, false)) return;

    StopStreaming();
    RemoveInputBlock();

    const auto& effects = Mad2Effects_Resolve();
    if (effects.ClearByName) effects.ClearByName("JakChallenge");
    Log("Jak1 exited on its own (quit, or the game-info.gc power-cell auto-shutdown patch fired) -- effect cleared\n");
}

static DWORD WINAPI WatcherThreadFunc(LPVOID) {
    char buf[64];
    for (;;) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n = recvfrom(g_ControlSocket, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == static_cast<int>(sizeof(Mad2Jak1ControlPacket))) {
            auto* pkt = reinterpret_cast<Mad2Jak1ControlPacket*>(buf);
            if (pkt->magic == MAD2JAK1_CONTROL_MAGIC && pkt->type == MAD2JAK1_CONTROL_EXITED) break;
        } else if (n == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT) {
            Log("[Jak1] control recv error %d\n", WSAGetLastError());
        }
        if (!g_Active.load()) return 0;  // ClearJakChallenge already owns teardown, nothing left to report
    }
    OnJakProcessGone();
    return 0;
}

// ---------------------------------------------------------------------
// mad2effects Apply/Clear.
// ---------------------------------------------------------------------

static void WINAPI ApplyJakChallenge(void*) {
    if (!IsRunningUnderWine()) {
        Log("JakChallenge requires Wine + mad2relauncher (no native jak-project source to build a "
            "standalone Windows launch path from, unlike SM64Challenge) -- ignoring trigger on native Windows\n");
        return;
    }

    bool expected = false;
    if (!g_Active.compare_exchange_strong(expected, true)) {
        Log("JakChallenge already active -- ignoring re-trigger\n");
        return;
    }

    if (!SetupSockets()) {
        Log("Failed to set up UDP sockets -- aborting JakChallenge\n");
        g_Active.store(false);
        return;
    }

    ApplyInputBlock();
    StartStreaming();
    SendControl(MAD2JAK1_CONTROL_LAUNCH);  // fire-and-forget -- mad2relauncher logs/handles launch failures itself

    g_WatcherThread = CreateThread(nullptr, 0, WatcherThreadFunc, nullptr, 0, nullptr);
    Log("JakChallenge applied (Wine -> mad2relauncher)\n");
}

static void WINAPI ClearJakChallenge(void*) {
    bool expected = true;
    if (!g_Active.compare_exchange_strong(expected, false)) return;  // already inactive -- no-op

    SendControl(MAD2JAK1_CONTROL_STOP);
    if (g_WatcherThread) {
        WaitForSingleObject(g_WatcherThread, 2000);
        CloseHandle(g_WatcherThread);
        g_WatcherThread = nullptr;
    }

    StopStreaming();
    RemoveInputBlock();
    Log("JakChallenge cleared\n");
}

// ---------------------------------------------------------------------
// Manual test-trigger keybind -- same shape as sm64mod.cpp's
// TestTriggerThreadFunc.
// ---------------------------------------------------------------------

static DWORD WINAPI TestTriggerThreadFunc(LPVOID) {
    bool wasPressed = false;
    for (;;) {
        Sleep(33);
        if (g_Config.testTriggerVk <= 0) continue;
        bool isPressed = (GetAsyncKeyState(g_Config.testTriggerVk) & 0x8000) != 0;
        if (isPressed && !wasPressed) {
            Log("TestTriggerKey pressed -- forcing JakChallenge\n");
            const auto& effects = Mad2Effects_Resolve();
            if (effects.Trigger) effects.Trigger("JakChallenge", 0);
        }
        wasPressed = isPressed;
    }
}

// ---------------------------------------------------------------------
// Called from sm64mod.cpp's InitThreadFunc once mad2config/mad2xinput/
// mad2effects are all resolvable -- see jak1_init.h.
// ---------------------------------------------------------------------

void Jak1_RegisterEffect() {
    LoadConfig();

    Mad2EffectDesc desc{};
    desc.name = "JakChallenge";
    desc.displayName = "Jak & Daxter Challenge";
    desc.defaultWeight = g_Config.weight;
    desc.apply = ApplyJakChallenge;
    desc.clear = ClearJakChallenge;
    desc.defaultMinDurationMs = 0;
    desc.defaultMaxDurationMs = 0;  // no auto-revert timer -- self-clears when Jak1 exits, see OnJakProcessGone
    BOOL ok = Mad2Effects_Resolve().Register(&desc);
    Log("Mad2Effects_Register(\"JakChallenge\") -> %d\n", ok);

    HANDLE testTriggerThread = CreateThread(nullptr, 0, TestTriggerThreadFunc, nullptr, 0, nullptr);
    if (testTriggerThread) CloseHandle(testTriggerThread);
}

void Jak1_Shutdown() {
    if (g_Active.load()) ClearJakChallenge(nullptr);
    if (g_WinsockReady) {
        if (g_InputSocket != INVALID_SOCKET) closesocket(g_InputSocket);
        if (g_ControlSocket != INVALID_SOCKET) closesocket(g_ControlSocket);
        WSACleanup();
    }
    // Log file is shared with sm64mod.cpp and closed there -- see jak1_init.h.
}
