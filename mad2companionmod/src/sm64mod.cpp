// SM64 chaos effect for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Registers a single
// mad2effects (mad2/mods/mad2effects.dll -- see
// ../../mad2effects/include/mad2effects_api.h) effect, "SM64Challenge",
// that launches a separate Super Mario 64 process (the vendored
// extern/sm64ex-alo port), blocks Mad2's own controller input for the
// duration via mad2xinput (mad2/mods/mad2xinput.dll -- see
// ../../mad2xinput/include/mad2xinput_api.h), and streams mad2xinput's
// POST-override state (Mad2XInput_GetOverriddenState -- exactly what Mad2
// itself receives, so an active JoystickReversal/SwapAB/etc. effect also
// applies inside SM64) to it over a loopback UDP socket, decoded by a
// custom controller backend added to extern/sm64ex-alo (see
// extern/sm64ex-alo/src/pc/controller/controller_udp.c). See
// include/mad2sm64_protocol.h for the wire format shared with that backend
// and with mad2relauncher.
//
// This mod has no per-frame D3D9 hook, same "init-only background thread"
// shape as mad2inputeffectsmod: DllMain spawns a thread that retries until
// mad2config/mad2xinput/mad2effects are all resolvable, then registers the
// effect and exits -- see InitThreadFunc below.
//
// Two platform branches, because SM64 can't run inside the same Wine
// process as Mad2 (it needs to be a native Linux binary to avoid stacking
// two D3D9 translation layers):
//   - Under Wine (detected via ntdll's wine_get_version export): this mod
//     never launches anything itself. It sends a LAUNCH control packet to
//     mad2relauncher (a separate native Linux process running alongside
//     gamescope -- see ../mad2relauncher), which forks the native Linux
//     sm64ex-alo binary and handles window-focus juggling via X11. This mod
//     only streams input and waits for an EXITED reply.
//   - On real Windows: no relauncher, no gamescope. This mod launches a
//     Windows-built sm64.exe directly via CreateProcess and does its own
//     best-effort window-focus juggling (FindWindow/SetForegroundWindow).
//
// SM64Challenge is registered with no auto-revert duration (both duration
// fields 0, but a Clear callback is set) -- per mad2effects_api.h's
// contract this means "stays active until explicitly cleared." Instead of
// relying on mad2effects' timer, this mod's own watcher thread notices when
// the SM64 process actually exits and calls Mad2Effects_ClearByName itself
// -- see OnSm64ProcessGone. g_Active is the single source of truth
// coordinating that self-clear path with an externally-triggered Clear
// (e.g. mad2chaosmod re-rolling, ClearAll, or a re-trigger while already
// active) so teardown only ever runs once -- see the comments on
// OnSm64ProcessGone/ClearSM64Challenge for exactly how that's arbitrated.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <ctime>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2effects/include/mad2effects_api.h"
#include "../../mad2xinput/include/mad2xinput_api.h"
#include "../include/mad2sm64_protocol.h"
#include "jak1_init.h"

// ---------------------------------------------------------------------
// Logging -- one shared file (mad2companionmod.log) for both source files
// in this DLL (sm64mod.cpp and jak1mod.cpp), owned here. Guarded by a
// mutex: previously each file had its own independent static FILE*/Log(),
// both targeting the same filename -- one opened "w" (truncate), the other
// "a" (append) -- a real interleaving/truncation hazard across two
// translation units' worker/watcher threads writing "the same" file through
// two unsynchronized handles (improvement-plan.md item 14). jak1mod.cpp now
// calls into Companion_LogV (declared in jak1_init.h) instead of keeping
// its own copy.
// ---------------------------------------------------------------------

static FILE* g_LogFile = nullptr;
static std::mutex g_LogMutex;

static void LogV(const char* prefix, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (!g_LogFile) {
        g_LogFile = fopen("mad2companionmod.log", "w");
        if (!g_LogFile) return;
        setvbuf(g_LogFile, nullptr, _IONBF, 0);
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_LogFile, "[%02d:%02d:%02d.%03d] %s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, prefix);
    vfprintf(g_LogFile, fmt, args);
}

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV("", fmt, args);
    va_end(args);
}

// Called from jak1mod.cpp's own Log() wrapper -- see jak1_init.h.
void Companion_LogV(const char* prefix, const char* fmt, va_list args) {
    LogV(prefix, fmt, args);
}

// ---------------------------------------------------------------------
// Config -- shares mad2config.dll's [Companion] section with jak1mod.cpp's
// JakChallenge settings (Jak1_* keys below) rather than each effect owning
// its own separate section, since both are the same "companion process"
// idea and the user asked for their settings to live together. Keys
// prefixed "SM64_" to stay distinguishable from Jak1's "Jak1_" keys within
// the shared section (mad2config's format is flat key=value per section,
// so two effects sharing one section need distinct key names).
// ---------------------------------------------------------------------

static const char kDefaultWindowsSm64Path[] = "sm64\\sm64.exe";

struct Config {
    DWORD userIndex = 0;
    bool broadcastToAllControllers = true;
    int weight = 5;
    int inputPort = MAD2SM64_DEFAULT_INPUT_PORT;
    int controlPort = MAD2SM64_DEFAULT_CONTROL_PORT;
    char windowsSm64Path[MAX_PATH];
    int testTriggerVk = VK_F10;  // manual on-demand trigger, independent of mad2chaosmod's weighted RNG
} g_Config;

static void LoadConfig() {
    strncpy(g_Config.windowsSm64Path, kDefaultWindowsSm64Path, sizeof(g_Config.windowsSm64Path) - 1);
    g_Config.windowsSm64Path[sizeof(g_Config.windowsSm64Path) - 1] = 0;

    const auto& api = Mad2Config_Resolve();
    if (!api.GetInt) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in defaults\n");
        return;
    }

    g_Config.userIndex = static_cast<DWORD>(api.GetInt(
        "Companion", "SM64_UserIndex", static_cast<int>(g_Config.userIndex),
        "Which controller (XInput user index, 0-3) SM64Challenge sources its streamed\n"
        "post-override input from when SM64_BroadcastToAllControllers is false."));
    g_Config.broadcastToAllControllers =
        api.GetBool("Companion", "SM64_BroadcastToAllControllers", g_Config.broadcastToAllControllers ? TRUE : FALSE,
                     "Block Mad2's input on all 4 possible XInput slots (not just SM64_UserIndex)\n"
                     "while SM64Challenge is active -- same Steam-Input-multi-slot rationale as\n"
                     "[InputEffects] BroadcastToAllControllers.") != FALSE;
    g_Config.weight = api.GetInt("Companion", "SM64_Weight", g_Config.weight, "SM64Challenge: chance-pool weight.");
    g_Config.inputPort = api.GetInt(
        "Companion", "SM64_InputPort", g_Config.inputPort,
        "Loopback UDP port mad2companionmod streams post-override controller state to.\n"
        "Must match the port SM64's controller_udp backend (MAD2SM64_INPUT_PORT env var)\n"
        "and, under Wine, mad2relauncher's -input-port flag are configured with.");
    g_Config.controlPort = api.GetInt(
        "Companion", "SM64_ControlPort", g_Config.controlPort,
        "Loopback UDP port mad2companionmod uses to ask mad2relauncher to launch/stop\n"
        "SM64 (Wine only -- unused on native Windows). Must match mad2relauncher's\n"
        "-control-port flag.");

    char pathBuf[MAX_PATH];
    api.GetString("Companion", "SM64_WindowsSm64Path", kDefaultWindowsSm64Path,
                   "Path to a Windows-built sm64ex-alo executable (only used when NOT running\n"
                   "under Wine -- see extern/sm64ex-alo and `just build-sm64-windows`).",
                   pathBuf, sizeof(pathBuf));
    strncpy(g_Config.windowsSm64Path, pathBuf, sizeof(g_Config.windowsSm64Path) - 1);
    g_Config.windowsSm64Path[sizeof(g_Config.windowsSm64Path) - 1] = 0;

    g_Config.testTriggerVk = api.GetVirtualKey(
        "Companion", "SM64_TestTriggerKey", g_Config.testTriggerVk,
        "Keyboard key that immediately triggers SM64Challenge on demand, bypassing\n"
        "mad2chaosmod's weighted RNG entirely -- useful for testing since the default\n"
        "Weight above only gives it a small share of the chance pool. Virtual-key code\n"
        "name with the VK_ prefix stripped (e.g. F10), or a 0xNN hex code. Set to an\n"
        "unused code (e.g. 0) to disable.\n"
        "See https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes");

    Log("Config loaded (userIndex=%lu, inputPort=%d, controlPort=%d, windowsSm64Path=%s, testTriggerVk=0x%X)\n",
        g_Config.userIndex, g_Config.inputPort, g_Config.controlPort, g_Config.windowsSm64Path,
        g_Config.testTriggerVk);
}

// ---------------------------------------------------------------------
// Platform detection.
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
static SOCKET g_InputSocket = INVALID_SOCKET;    // mod -> SM64 process, input streaming
static SOCKET g_ControlSocket = INVALID_SOCKET;  // mod <-> mad2relauncher, Wine branch only
static sockaddr_in g_InputTargetAddr{};
static sockaddr_in g_RelauncherAddr{};

static HANDLE g_StreamThread = nullptr;
static HANDLE g_StreamStopEvent = nullptr;
static HANDLE g_WatcherThread = nullptr;
static PROCESS_INFORMATION g_Sm64Process{};  // native Windows branch only

struct HandleSet {
    Mad2XInputOverrideHandle h[XUSER_MAX_COUNT] = {};
};
static HandleSet g_BlockHandles;

// ---------------------------------------------------------------------
// Input block -- fully blocks Mad2's own input while SM64Challenge is
// active, expressible with mad2xinput's existing override fields (no
// struct changes needed): every digital button forced off, every stick
// axis forced to 0.
// ---------------------------------------------------------------------

static void ApplyInputBlock() {
    const auto& api = Mad2XInput_Resolve();
    if (!api.AddOverride) {
        Log("[Dependency] mad2xinput.dll not available -- cannot block Mad2 input for SM64Challenge\n");
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
        g_InputTargetAddr.sin_addr.s_addr = inet_addr("127.0.0.1");  // hardcoded loopback -- INADDR_NONE ambiguity doesn't apply
    }
    if (g_ControlSocket == INVALID_SOCKET) {
        g_ControlSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
        bindAddr.sin_port = 0;  // ephemeral -- mad2relauncher replies to whatever source port our sends come from
        bind(g_ControlSocket, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr));

        // Short timeout so the Wine-branch watcher thread's recvfrom loop
        // notices g_Active flipping false (an external Clear()) promptly
        // instead of blocking indefinitely waiting for an EXITED that may
        // never arrive (e.g. mad2relauncher isn't running at all).
        DWORD timeoutMs = 200;
        setsockopt(g_ControlSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        ZeroMemory(&g_RelauncherAddr, sizeof(g_RelauncherAddr));
        g_RelauncherAddr.sin_family = AF_INET;
        g_RelauncherAddr.sin_port = htons(static_cast<u_short>(g_Config.controlPort));
        g_RelauncherAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    }
    return g_InputSocket != INVALID_SOCKET && g_ControlSocket != INVALID_SOCKET;
}

static void SendControl(Mad2Sm64ControlType type) {
    if (g_ControlSocket == INVALID_SOCKET) return;
    Mad2Sm64ControlPacket pkt{MAD2SM64_CONTROL_MAGIC, static_cast<uint32_t>(type)};
    sendto(g_ControlSocket, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
           reinterpret_cast<sockaddr*>(&g_RelauncherAddr), sizeof(g_RelauncherAddr));
}

// ---------------------------------------------------------------------
// XInput -> N64 button translation (PR/os_cont.h's CONT_* bits, vendored at
// extern/sm64ex-alo/include/PR/os_cont.h). This packet's `buttons` field
// was, until this was found and fixed, sent as XINPUT_GAMEPAD's raw
// wButtons bitmask completely untranslated -- controller_udp.c's
// controller_udp_read() just does `pad->button = sLatest.buttons` with no
// translation of its own (unlike the real controller_sdl2.c backend, which
// runs every button through a configurable SDL-button -> N64-button bind
// table, see its controller_sdl_bind()). Since XInput's bit positions
// (DPAD_UP=0x0001, A=0x1000, START=0x0010, ...) don't line up with N64's
// (CONT_UP=0x0800, CONT_A=0x8000, CONT_START=0x1000, ...) at almost any
// bit, every single button was effectively scrambled -- this is the bug
// behind "SM64Challenge's controls are broken". Fixing it here (matching
// jak1mod.cpp's XInputToPs2Pad, which got this right from the start for
// the same reason -- neither receiving side should have to know XInput's
// bit layout) rather than in controller_udp.c keeps the translation in one
// place, one language, shared with the Jak1 side's equivalent.
//
// Hand-duplicated from extern/sm64ex-alo/include/PR/os_cont.h's CONT_*
// defines -- not #include-able from here (that header is part of the
// vendored SM64 source tree, a separate build/toolchain entirely; this DLL
// never links against it), same cross-boundary-constant-duplication
// reasoning as mad2jak1_protocol.h's MAD2JAK1_PAD_* bits.
#define N64_CONT_A 0x8000
#define N64_CONT_B 0x4000
#define N64_CONT_G 0x2000  // Z trigger
#define N64_CONT_START 0x1000
#define N64_CONT_UP 0x0800
#define N64_CONT_DOWN 0x0400
#define N64_CONT_LEFT 0x0200
#define N64_CONT_RIGHT 0x0100
#define N64_CONT_L 0x0020
#define N64_CONT_R 0x0010
#define N64_CONT_E 0x0008  // U_CBUTTONS
#define N64_CONT_D 0x0004  // D_CBUTTONS
#define N64_CONT_C 0x0002  // L_CBUTTONS
#define N64_CONT_F 0x0001  // R_CBUTTONS

// Mapping follows this project's own default SDL bindings
// (configfile.c's configKeyA/B/... defaults) where there's a direct
// equivalent, and controller_sdl2.c's own analog-stick-to-C-buttons
// threshold (0x4000) for the right stick, which N64 has no analog
// equivalent for. Y has no natural N64 mapping (N64 has no 4th face
// button) and is intentionally left unbound.
static WORD XInputToN64Buttons(const XINPUT_GAMEPAD& gp) {
    WORD b = 0;
    if (gp.wButtons & XINPUT_GAMEPAD_A) b |= N64_CONT_A;
    if (gp.wButtons & XINPUT_GAMEPAD_B) b |= N64_CONT_B;
    if (gp.wButtons & XINPUT_GAMEPAD_X) b |= N64_CONT_G;  // Z trigger -- common modern-pad remap for N64's Z
    if (gp.wButtons & XINPUT_GAMEPAD_START) b |= N64_CONT_START;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_UP) b |= N64_CONT_UP;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) b |= N64_CONT_DOWN;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) b |= N64_CONT_LEFT;
    if (gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) b |= N64_CONT_RIGHT;
    // N64's L/R are physical shoulder bumpers (not lower analog triggers
    // like a PS2/Xbox controller's LT/RT), so Xbox's LB/RB is the natural
    // match -- matching controller_sdl2.c's own configKeyL/R defaults.
    if (gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= N64_CONT_L;
    if (gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= N64_CONT_R;
    // Right stick -> C-buttons, thresholded exactly like controller_sdl2.c
    // (0x4000 there is SDL's raw joystick-axis magnitude, same 16-bit
    // signed range XInput's sThumbRX/RY already use). Y sign is flipped
    // relative to that file's own rightx/righty comparisons: SDL's axis
    // convention is positive-down, XInput's is positive-up (see
    // Ps2StickByte-equivalent reasoning in jak1mod.cpp), so a positive
    // XInput RY (stick pushed up) must map to U_CBUTTONS, not D_CBUTTONS.
    if (gp.sThumbRX < -0x4000) b |= N64_CONT_C;  // L_CBUTTONS
    if (gp.sThumbRX > 0x4000) b |= N64_CONT_F;   // R_CBUTTONS
    if (gp.sThumbRY > 0x4000) b |= N64_CONT_E;   // U_CBUTTONS
    if (gp.sThumbRY < -0x4000) b |= N64_CONT_D;  // D_CBUTTONS
    return b;
}

// ---------------------------------------------------------------------
// Input streaming -- polls mad2xinput's RAW, pre-override state at ~125Hz
// and forwards it to whichever process is running SM64. Tries
// Config::userIndex first; if BroadcastToAllControllers is set and that
// slot isn't the one that happens to be connected (Steam Input multi-slot
// exposure -- see mad2inputeffectsmod's Config::broadcastToAllControllers
// comment), scans the rest.
//
// Deliberately RAW (Mad2XInput_GetState), not POST-override
// (Mad2XInput_GetOverriddenState), despite the latter's documented appeal
// (an active JoystickReversal/SwapAB/etc. effect would also apply inside
// SM64). Root-caused live (changes.md item 13, "sm64 fails to read
// controller inputs" -- same bug independently affected JakChallenge, see
// jak1mod.cpp's identical fix): ApplyInputBlock() below installs an
// AddOverride layer on this exact controller (forceBlockButtons=0xFFFF,
// forced sticks to dead-center) specifically so Mad2's own game logic stops
// reacting to the player's real input while SM64 has focus. But
// GetOverriddenState() returns state with *every* active layer applied,
// including that very block -- so reading it back here just reads our own
// just-applied zero, every single poll, regardless of what the player
// actually does with the controller. Confirmed live: a synthetic non-zero
// XInput override + this same code path in JakChallenge always produced an
// all-centered/no-buttons packet downstream. Switching to GetState (cached
// pre-any-override, unaffected by our own or anyone else's layer) fixes
// real input flowing through; the tradeoff is other mods' XInput effects no
// longer compose into what SM64/Jak1 sees. Recovering that composition
// would need mad2xinput.dll itself to grow an "exclude this handle" variant
// of GetOverriddenState -- flagged for whoever owns that DLL, not done here
// since correctness (input working at all) matters more than composition
// right now.
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

        Mad2Sm64InputPacket pkt{};
        pkt.magic = MAD2SM64_INPUT_MAGIC;
        pkt.seq = g_Seq.fetch_add(1);
        pkt.buttons = XInputToN64Buttons(state.Gamepad);
        pkt.lx = state.Gamepad.sThumbLX;
        pkt.ly = state.Gamepad.sThumbLY;
        pkt.rx = state.Gamepad.sThumbRX;
        pkt.ry = state.Gamepad.sThumbRY;
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
// Native-Windows-only: launch sm64.exe directly and best-effort focus it.
// ---------------------------------------------------------------------

// Same course pool mad2relauncher.go picks from on the Wine branch (in
// turn taken from ../mad2mod/src/main.cpp's own SM64 sub-game challenge) --
// kept in sync by hand across the two launch paths.
static const int kSm64LevelPool[] = {9, 24, 12, 5, 4, 7, 22, 8, 23, 10, 11, 36, 13, 14, 15, 27};

static bool LaunchWindowsSm64() {
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", g_Config.inputPort);
    // Child inherits the parent's environment block by default when
    // CreateProcess's lpEnvironment is null, so setting this in-process
    // (mutating our own env) is the simplest way to hand the port down --
    // harmless since this mod is the only thing that reads/writes it.
    SetEnvironmentVariableA("MAD2SM64_INPUT_PORT", portStr);

    char workDir[MAX_PATH];
    strncpy(workDir, g_Config.windowsSm64Path, sizeof(workDir) - 1);
    workDir[sizeof(workDir) - 1] = 0;
    char* lastSlash = strrchr(workDir, '\\');
    if (!lastSlash) lastSlash = strrchr(workDir, '/');
    if (lastSlash) *lastSlash = 0;
    else workDir[0] = 0;

    // Plain, no-argument launch boots to SM64's own title/file-select
    // screen -- these flags (sm64ex-alo's own CLI options,
    // extern/sm64ex-alo/src/pc/cliopts.c) warp straight into a short,
    // self-contained course instead, matching the Wine branch's launch
    // (see mad2relauncher/sm64.go's handleLaunch) and what
    // ../mad2mod/src/main.cpp's launch_sm64_level originally did.
    int level = kSm64LevelPool[rand() % (sizeof(kSm64LevelPool) / sizeof(kSm64LevelPool[0]))];
    char cmdLine[MAX_PATH + 64];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\" --skip-intro --level %d --width 1280 --height 720 --fullscreen",
             g_Config.windowsSm64Path, level);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    ZeroMemory(&g_Sm64Process, sizeof(g_Sm64Process));
    BOOL ok = CreateProcessA(g_Config.windowsSm64Path, cmdLine, nullptr, nullptr, FALSE, 0, nullptr,
                              workDir[0] ? workDir : nullptr, &si, &g_Sm64Process);
    if (!ok) {
        Log("CreateProcess(\"%s\") failed, err=%lu\n", g_Config.windowsSm64Path, GetLastError());
        return false;
    }
    Log("Launched native Windows SM64: %s (pid=%lu)\n", g_Config.windowsSm64Path, g_Sm64Process.dwProcessId);
    return true;
}

// Fire-and-forget: self-terminates within ~10s once it finds SM64's window
// or gives up, doesn't touch anything requiring synchronization with
// Clear()/OnSm64ProcessGone beyond reading g_Active (atomic).
static DWORD WINAPI FocusSm64WindowThreadFunc(LPVOID param) {
    HWND mad2Wnd = reinterpret_cast<HWND>(param);
    for (int i = 0; i < 100 && g_Active.load(); ++i) {
        HWND sm64Wnd = FindWindowA(nullptr, "Super Mario 64");
        if (sm64Wnd) {
            if (mad2Wnd) ShowWindow(mad2Wnd, SW_MINIMIZE);
            ShowWindow(sm64Wnd, SW_SHOW);
            SetForegroundWindow(sm64Wnd);
            return 0;
        }
        Sleep(100);
    }
    return 0;
}

// ---------------------------------------------------------------------
// Teardown coordination. Two independent triggers converge here:
//   - ClearSM64Challenge: an external Clear (mad2chaosmod re-roll,
//     ClearAll, a re-trigger while already active) -- owns telling SM64 to
//     die and joining the watcher thread.
//   - OnSm64ProcessGone: the watcher thread itself notices SM64 already
//     exited (Wine: EXITED control packet; native: process handle
//     signaled) -- must NOT join the watcher thread (that's this thread;
//     joining self deadlocks).
// g_Active.compare_exchange_strong is the sole arbiter: whichever of the
// two flips it true->false first does the real teardown; the other sees
// it already false and no-ops. OnSm64ProcessGone's self-clear calls
// Mad2Effects_ClearByName, which synchronously re-invokes
// ClearSM64Challenge -- by then g_Active is already false, so that
// re-entrant call takes the no-op path instead of trying to join its own
// caller's thread.
// ---------------------------------------------------------------------

static void OnSm64ProcessGone() {
    bool expected = true;
    if (!g_Active.compare_exchange_strong(expected, false)) return;

    StopStreaming();
    RemoveInputBlock();
    if (g_Sm64Process.hProcess) {
        CloseHandle(g_Sm64Process.hProcess);
        CloseHandle(g_Sm64Process.hThread);
        g_Sm64Process = {};
    }

    const auto& effects = Mad2Effects_Resolve();
    if (effects.ClearByName) effects.ClearByName("SM64Challenge");
    Log("SM64 exited on its own -- effect cleared\n");
}

static DWORD WINAPI WatcherThreadFunc_Wine(LPVOID) {
    char buf[64];
    for (;;) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n = recvfrom(g_ControlSocket, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == static_cast<int>(sizeof(Mad2Sm64ControlPacket))) {
            auto* pkt = reinterpret_cast<Mad2Sm64ControlPacket*>(buf);
            if (pkt->magic == MAD2SM64_CONTROL_MAGIC && pkt->type == MAD2SM64_CONTROL_EXITED) break;
        } else if (n == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT) {
            Log("[SM64] control recv error %d\n", WSAGetLastError());
        }
        if (!g_Active.load()) return 0;  // ClearSM64Challenge already owns teardown, nothing left to report
    }
    OnSm64ProcessGone();
    return 0;
}

static DWORD WINAPI WatcherThreadFunc_Native(LPVOID) {
    WaitForSingleObject(g_Sm64Process.hProcess, INFINITE);
    OnSm64ProcessGone();
    return 0;
}

// ---------------------------------------------------------------------
// mad2effects Apply/Clear.
// ---------------------------------------------------------------------

static void WINAPI ApplySM64Challenge(void*) {
    bool expected = false;
    if (!g_Active.compare_exchange_strong(expected, true)) {
        Log("SM64Challenge already active -- ignoring re-trigger\n");
        return;
    }

    if (!SetupSockets()) {
        Log("Failed to set up UDP sockets -- aborting SM64Challenge\n");
        g_Active.store(false);
        return;
    }

    ApplyInputBlock();
    StartStreaming();

    bool wine = IsRunningUnderWine();
    bool launched;
    if (wine) {
        SendControl(MAD2SM64_CONTROL_LAUNCH);
        launched = true;  // fire-and-forget -- mad2relauncher logs/handles launch failures on its own side
    } else {
        launched = LaunchWindowsSm64();
        if (launched) {
            HANDLE t = CreateThread(nullptr, 0, FocusSm64WindowThreadFunc, GetForegroundWindow(), 0, nullptr);
            if (t) CloseHandle(t);
        }
    }

    if (!launched) {
        StopStreaming();
        RemoveInputBlock();
        g_Active.store(false);
        return;
    }

    g_WatcherThread = CreateThread(nullptr, 0, wine ? WatcherThreadFunc_Wine : WatcherThreadFunc_Native, nullptr, 0, nullptr);
    Log("SM64Challenge applied (%s)\n", wine ? "Wine -> mad2relauncher" : "native Windows CreateProcess");
}

static void WINAPI ClearSM64Challenge(void*) {
    bool expected = true;
    if (!g_Active.compare_exchange_strong(expected, false)) return;  // already inactive (self-clear path already ran) -- no-op

    if (IsRunningUnderWine()) {
        SendControl(MAD2SM64_CONTROL_STOP);
    } else if (g_Sm64Process.hProcess) {
        TerminateProcess(g_Sm64Process.hProcess, 0);
    }
    if (g_WatcherThread) {
        WaitForSingleObject(g_WatcherThread, 2000);
        CloseHandle(g_WatcherThread);
        g_WatcherThread = nullptr;
    }
    if (g_Sm64Process.hProcess) {
        CloseHandle(g_Sm64Process.hProcess);
        CloseHandle(g_Sm64Process.hThread);
        g_Sm64Process = {};
    }

    StopStreaming();
    RemoveInputBlock();
    Log("SM64Challenge cleared\n");
}

// ---------------------------------------------------------------------
// Manual test-trigger keybind -- polls Config::testTriggerVk (default F10)
// at ~30Hz and calls Mad2Effects_Trigger("SM64Challenge", 0) directly on
// a rising edge, the same way mad2igttimer's ResetKey polls GetAsyncKeyState
// for its own keybind. Exists purely so this effect can be tested on
// demand instead of waiting on mad2chaosmod's weighted RNG to happen to
// pick it (Weight defaults to a small share of the pool).
// ---------------------------------------------------------------------

static DWORD WINAPI TestTriggerThreadFunc(LPVOID) {
    bool wasPressed = false;
    for (;;) {
        Sleep(33);
        if (g_Config.testTriggerVk <= 0) continue;
        bool isPressed = (GetAsyncKeyState(g_Config.testTriggerVk) & 0x8000) != 0;
        if (isPressed && !wasPressed) {
            Log("TestTriggerKey pressed -- forcing SM64Challenge\n");
            const auto& effects = Mad2Effects_Resolve();
            if (effects.Trigger) effects.Trigger("SM64Challenge", 0);
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
        bool xinputOk = Mad2XInput_Resolve().AddOverride != nullptr && Mad2XInput_Resolve().GetState != nullptr;
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

    Mad2EffectDesc desc{};
    desc.name = "SM64Challenge";
    desc.displayName = "Super Mario 64 Challenge";
    desc.defaultWeight = g_Config.weight;
    desc.apply = ApplySM64Challenge;
    desc.clear = ClearSM64Challenge;
    desc.defaultMinDurationMs = 0;
    desc.defaultMaxDurationMs = 0;  // no auto-revert timer -- see file header for why (self-clears when SM64 exits)
    BOOL ok = Mad2Effects_Resolve().Register(&desc);
    Log("Mad2Effects_Register(\"SM64Challenge\") -> %d\n", ok);

    HANDLE testTriggerThread = CreateThread(nullptr, 0, TestTriggerThreadFunc, nullptr, 0, nullptr);
    if (testTriggerThread) CloseHandle(testTriggerThread);

    // JakChallenge (jak1mod.cpp) and MarioLegacyChallenge (mariolegacymod.cpp)
    // share this same init thread and dependency wait --
    // mad2config/mad2xinput/mad2effects are already confirmed resolvable at
    // this point, see the loop above.
    Jak1_RegisterEffect();
    MarioLegacy_RegisterEffect();

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            srand(static_cast<unsigned>(time(nullptr)) ^ GetCurrentProcessId());  // seeds LaunchWindowsSm64's level pick
            HANDLE h = CreateThread(nullptr, 0, InitThreadFunc, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
            break;
        }
        case DLL_PROCESS_DETACH:
            if (g_Active.load()) ClearSM64Challenge(nullptr);
            if (g_WinsockReady) {
                if (g_InputSocket != INVALID_SOCKET) closesocket(g_InputSocket);
                if (g_ControlSocket != INVALID_SOCKET) closesocket(g_ControlSocket);
                WSACleanup();
            }
            Jak1_Shutdown();  // clears JakChallenge if active, closes its sockets -- shared log file closed below
            MarioLegacy_Shutdown();  // same, for MarioLegacyChallenge
            {
                std::lock_guard<std::mutex> lock(g_LogMutex);
                if (g_LogFile) {
                    fclose(g_LogFile);
                    g_LogFile = nullptr;
                }
            }
            break;
        default:
            break;
    }
    return TRUE;
}
