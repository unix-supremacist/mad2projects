// Madagascar 2 IGT/RTA/Playtime timer overlay.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Ports the timer feature
// of the external ../mad2mod SDL overlay tool into an in-process D3D9
// overlay: no separate process, no /proc/<pid>/mem reads, no shared-memory
// controller struct -- everything here runs inside Mad2.exe itself.
//
// Hooking strategy: like mad2shadowfix, Direct3DCreate9/Ex is hooked at
// the IAT level (see PatchIat below) so this works with whatever d3d9.dll
// backend is actually active (DXVK, wine3d, dxwrapper-chained, ...).
// Unlike shadowfix, this mod only needs one callback per frame (to draw
// the overlay), so instead of implementing a full IDirect3D9/
// IDirect3DDevice9 COM proxy (shadowfix's ~1300-line approach, needed
// there because it inspects/rewrites dozens of different calls), this
// vtable-patches just two slots on the real objects:
//   IDirect3D9::CreateDevice      (vtable slot 16)
//   IDirect3DDevice9::EndScene    (vtable slot 42)
// (Indices confirmed against /usr/i686-w64-mingw32/include/d3d9.h --
// IUnknown's 3 methods plus the interface's declared method order.)
//
// This composes correctly with shadowfix regardless of mad2/mods/ load
// order: whichever mod's Direct3DCreate9 hook runs first, the *other*
// mod's chained call eventually bottoms out at the real d3d9.dll object
// (either directly, or via shadowfix's m_pReal inside its proxy). Because
// this mod always vtable-patches whatever object it is handed -- the raw
// native object, or shadowfix's proxy object if that's what came back --
// and shadowfix's proxy always forwards through m_pReal to that same
// object, the patched slot ends up in the call chain exactly once, right
// above the true backend, no matter which mod wrapped/patched first.
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <xinput.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2xinput/include/mad2xinput_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2textrenderer/include/mad2textrenderer_api.h"

// ---------------------------------------------------------------------
// Logging -- writes into the shared logs\mad2.log (see mad2sharedlog.h),
// tagged "[mad2igttimer]" so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2igttimer]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [Timer] section, resolved lazily on the first
// EndScene call -- see mad2config/include/mad2config_api.h for why this
// can't happen from DllMain: mad2/mods/*.dll load order isn't guaranteed,
// so mad2config.dll might not be loaded yet this early.)
// ---------------------------------------------------------------------

struct Config {
    int resetVk = VK_F9;   // key that resets IGT + RTA (not Playtime)
    WORD resetComboButtons = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;  // controller combo, same effect
    int overlayX = 20;
    int overlayY = 20;
    int fontSize = 24;     // pixel height; texture is sized to fit at this size
    bool showIGT = true;
    bool showRTA = true;
    bool showPlaytime = true;
} g_Config;

static bool g_ConfigLoaded = false;

static void EnsureConfigLoaded() {
    if (g_ConfigLoaded) return;
    g_ConfigLoaded = true;

    const auto& api = Mad2Config_Resolve();
    if (!api.GetInt) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in defaults, "
            "settings in config.cfg will be ignored (place mad2config.dll in mad2/mods/)\n");
        return;
    }

    g_Config.overlayX = api.GetInt("Timer", "X", g_Config.overlayX,
                                    "Screen-space X position (pixels) of the timer overlay's top-left corner.");
    g_Config.overlayY = api.GetInt("Timer", "Y", g_Config.overlayY,
                                    "Screen-space Y position (pixels) of the timer overlay's top-left corner.");
    g_Config.fontSize = api.GetInt("Timer", "FontSize", g_Config.fontSize,
                                    "Pixel height of the timer overlay's font.");
    g_Config.resetVk = api.GetVirtualKey(
        "Timer", "ResetKey", g_Config.resetVk,
        "Keyboard key that resets IGT+RTA (not Playtime).\n"
        "Virtual-key code name with the VK_ prefix stripped (e.g. F9), or a 0xNN hex code.\n"
        "See https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes");
    g_Config.resetComboButtons = api.GetGamepadButtonMask(
        "Timer", "ResetComboButtons", "START,BACK",
        "Comma-separated controller buttons that must be held together to reset IGT+RTA.\n"
        "XINPUT_GAMEPAD_* names with the prefix stripped (e.g. START,BACK). Leave empty to disable.\n"
        "See https://learn.microsoft.com/windows/win32/api/xinput/ns-xinput-xinput_gamepad");
    g_Config.showIGT = api.GetBool("Timer", "ShowIGT", TRUE, "Show the in-game-time line.") != FALSE;
    g_Config.showRTA = api.GetBool("Timer", "ShowRTA", TRUE, "Show the real-time-attack line.") != FALSE;
    g_Config.showPlaytime = api.GetBool("Timer", "ShowPlaytime", TRUE, "Show the total-playtime line.") != FALSE;

    Log("Config loaded: resetVk=0x%X resetCombo=0x%04X overlayX=%d overlayY=%d fontSize=%d showIGT=%d showRTA=%d "
        "showPlaytime=%d\n",
        g_Config.resetVk, g_Config.resetComboButtons, g_Config.overlayX, g_Config.overlayY, g_Config.fontSize,
        g_Config.showIGT, g_Config.showRTA, g_Config.showPlaytime);
}

// IAT/vtable hooking now provided by aa_mad2hookutil.dll -- see
// ../../mad2hookutil/include/mad2hookutil_api.h. Resolved synchronously
// (not lazily) since aa_mad2hookutil.dll is guaranteed already loaded by
// the time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Timer state + persistence
//
// Mirrors ../mad2mod/src/main.cpp's timer logic exactly: IGT only
// accretes while TfbHavokLibrary.dll's load-flag byte is non-zero (the
// same offset that tool read externally via process_vm_readv -- here we
// just dereference it directly since we're in-process), RTA and Playtime
// always accrete. All three persist to disk every 15s with a 15s buffer
// added to the written value (so a crash between saves loses at most one
// save interval instead of undercounting). Reset (IGT + RTA only, not
// Playtime) writes 0.0 immediately rather than waiting for the next
// periodic save.
// ---------------------------------------------------------------------

struct TimerState {
    double igtSeconds = 0.0;
    double rtaSeconds = 0.0;
    double playtimeSeconds = 0.0;
};

static TimerState g_Timer;
static uint64_t g_LastTickMs = 0;
static uint64_t g_LastSaveMs = 0;

static const char* kIgtFile = "igttimer_igt.txt";
static const char* kRtaFile = "igttimer_rta.txt";
static const char* kPlaytimeFile = "igttimer_playtime.txt";

static double LoadTimerFile(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0.0;
    double value = 0.0;
    if (fscanf(f, "%lf", &value) != 1) value = 0.0;
    fclose(f);
    return value;
}

static void SaveTimerFile(const char* path, double value) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%f", value);
    fclose(f);
}

static void LoadTimerState() {
    g_Timer.igtSeconds = LoadTimerFile(kIgtFile);
    g_Timer.rtaSeconds = LoadTimerFile(kRtaFile);
    g_Timer.playtimeSeconds = LoadTimerFile(kPlaytimeFile);
    Log("Loaded timer state: IGT=%.3f RTA=%.3f Playtime=%.3f\n", g_Timer.igtSeconds, g_Timer.rtaSeconds,
        g_Timer.playtimeSeconds);
}

// Non-zero while the game is actually running (not loading/paused). Same
// fixed offset the external overlay tool used, resolved against our own
// in-process module handle instead of a remote memory read.
static bool IsGameActive() {
    static HMODULE havokModule = nullptr;
    if (!havokModule) {
        havokModule = GetModuleHandleA("TfbHavokLibrary.dll");
        if (!havokModule) return false;
        Log("Found TfbHavokLibrary.dll at %p\n", (void*)havokModule);
    }
    const uint8_t* flagAddr = reinterpret_cast<const uint8_t*>(havokModule) + 0x342140;
    return *flagAddr != 0;
}

static void UpdateTimer() {
    uint64_t now = GetTickCount64();
    if (g_LastTickMs == 0) {
        g_LastTickMs = now;
        g_LastSaveMs = now;
        return;
    }
    double dt = (now - g_LastTickMs) / 1000.0;
    g_LastTickMs = now;

    if (IsGameActive()) g_Timer.igtSeconds += dt;
    g_Timer.rtaSeconds += dt;
    g_Timer.playtimeSeconds += dt;

    if (now - g_LastSaveMs > 15000) {
        SaveTimerFile(kIgtFile, g_Timer.igtSeconds + 15.0);
        SaveTimerFile(kRtaFile, g_Timer.rtaSeconds + 15.0);
        SaveTimerFile(kPlaytimeFile, g_Timer.playtimeSeconds + 15.0);
        g_LastSaveMs = now;
    }

    static bool wasResetPressed = false;
    bool isResetPressed = (GetAsyncKeyState(g_Config.resetVk) & 0x8000) != 0;

    // Controller hotkey: hold the configured combo together (default
    // Start+Back, mirrors ../mad2mod/src/main.cpp's "T (or Back+Start on
    // controller)" reset binding; see [Timer] ResetComboButtons in
    // config.cfg). Polled via mad2xinput rather than XInputGetState directly
    // so this composes with whatever other mods are doing to controller 0's
    // state -- see mad2xinput/include/mad2xinput_api.h.
    static bool wasControllerResetPressed = false;
    bool isControllerResetPressed = false;
    if (const auto& api = Mad2XInput_Resolve(); api.GetState) {
        XINPUT_STATE state{};
        if (api.GetState(0, &state) && g_Config.resetComboButtons != 0) {
            isControllerResetPressed =
                (state.Gamepad.wButtons & g_Config.resetComboButtons) == g_Config.resetComboButtons;
        }
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("[Dependency] mad2xinput.dll not found or missing exports -- controller reset combo disabled "
                "(place mad2xinput.dll in mad2/mods/)\n");
        }
    }

    if ((isResetPressed && !wasResetPressed) || (isControllerResetPressed && !wasControllerResetPressed)) {
        g_Timer.igtSeconds = 0.0;
        g_Timer.rtaSeconds = 0.0;
        SaveTimerFile(kIgtFile, 0.0);
        SaveTimerFile(kRtaFile, 0.0);
        Log("Timer reset (IGT & RTA); Playtime unaffected (%.3f)\n", g_Timer.playtimeSeconds);
    }
    wasResetPressed = isResetPressed;
    wasControllerResetPressed = isControllerResetPressed;
}

static void FormatTimer(char* buf, size_t bufSize, const char* label, double seconds) {
    int h = (int)(seconds / 3600.0);
    int m = ((int)seconds % 3600) / 60;
    int s = (int)seconds % 60;
    int ms = (int)(seconds * 1000.0) % 1000;
    snprintf(buf, bufSize, "%s: %02d:%02d:%02d.%03d", label, h, m, s, ms);
}

// ---------------------------------------------------------------------
// Overlay rendering: text is rasterized via GDI into a DIB (no D3DX
// dependency, which isn't reliably present under Proton/Wine), converted
// to a D3D9 texture, then drawn as a screen-space textured quad.
//
// Text is drawn with a black outline (stroked via offset copies) so it
// stays legible over bright/white backgrounds, not just dark ones. Since
// a black outline can't also double as its own alpha-coverage source
// (black-on-black brightness is zero), alpha is derived from a separate
// all-white "mask" pass of the exact same strokes rather than from the
// final colored pixels -- see RenderOverlayTexture.
// ---------------------------------------------------------------------

static const int kPadding = 6;
static const int kOutlineOffset = 2;

// GDI rasterization/quad-draw now provided by mad2textrenderer.dll -- see
// ../../mad2textrenderer/include/mad2textrenderer_api.h. Resolved lazily
// (first EndScene call), same pattern mad2xinput consumers already use.
static Mad2TextSurface g_Surface = nullptr;

static bool EnsureOverlaySurface(IDirect3DDevice9* dev, int lineCount) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.CreateSurface) return false;

    if (!g_Surface) {
        g_Surface = api.CreateSurface(g_Config.fontSize);
        if (!g_Surface) return false;
    }

    // Widest possible line determines texture width regardless of which
    // lines are actually enabled (so the overlay doesn't change width
    // depending on Show*=false toggles); measured against the actual font
    // rather than guessed, so FontSize always produces a correctly sized
    // texture. Height only accounts for the lines actually enabled.
    int textW = api.MeasureTextWidth(g_Surface, "Playtime: 00:00:00.000");
    int lineHeight = api.GetLineHeight(g_Surface);
    int texW = textW + kPadding * 2 + kOutlineOffset * 2;
    int texH = lineHeight * lineCount + kPadding + kOutlineOffset * 2;

    return api.EnsureSize(g_Surface, dev, texW, texH) != FALSE;
}

// Lays out `count` lines top-to-bottom (same padding/offset math the
// original inlined DrawStrokedLines/RenderOverlayTexture used) and hands
// them to mad2textrenderer as white-filled runs -- outline is always black,
// per Mad2TextRenderer_Render's contract.
static void RenderOverlayTexture(const char* const* lines, int count) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.Render) return;
    int lineHeight = api.GetLineHeight(g_Surface);

    Mad2TextRun runs[3];
    for (int li = 0; li < count; ++li) {
        runs[li] = {kPadding + kOutlineOffset, kPadding / 2 + kOutlineOffset + li * lineHeight, lines[li],
                    RGB(255, 255, 255)};
    }
    api.Render(g_Surface, runs, count);
}

static void DrawOverlayQuad(IDirect3DDevice9* dev) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.DrawQuad) return;
    api.DrawQuad(g_Surface, dev, static_cast<float>(g_Config.overlayX), static_cast<float>(g_Config.overlayY));
}

// ---------------------------------------------------------------------
// Direct3DCreate9 IAT hook -> IDirect3D9::CreateDevice vtable patch ->
// IDirect3DDevice9::EndScene vtable patch (see file header for why
// vtable slots 16/42 and why this composes with shadowfix).
// ---------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    EnsureConfigLoaded();
    UpdateTimer();

    char igtBuf[64], rtaBuf[64], playtimeBuf[64];
    const char* lines[3];
    int lineCount = 0;
    if (g_Config.showIGT) {
        FormatTimer(igtBuf, sizeof(igtBuf), "IGT", g_Timer.igtSeconds);
        lines[lineCount++] = igtBuf;
    }
    if (g_Config.showRTA) {
        FormatTimer(rtaBuf, sizeof(rtaBuf), "RTA", g_Timer.rtaSeconds);
        lines[lineCount++] = rtaBuf;
    }
    if (g_Config.showPlaytime) {
        FormatTimer(playtimeBuf, sizeof(playtimeBuf), "Playtime", g_Timer.playtimeSeconds);
        lines[lineCount++] = playtimeBuf;
    }

    if (lineCount > 0 && EnsureOverlaySurface(This, lineCount)) {
        RenderOverlayTexture(lines, lineCount);
        DrawOverlayQuad(This);
    }
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
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface && !RealEndScene) {
        void* prevEndScene = nullptr;
        Mad2HookUtil_PatchVTableSlot(*ppReturnedDeviceInterface, 42, reinterpret_cast<void*>(HookEndScene), &prevEndScene);
        RealEndScene = reinterpret_cast<EndScene_t>(prevEndScene);
        Log("Hooked IDirect3DDevice9::EndScene (vtable[42], device=%p)\n", (void*)*ppReturnedDeviceInterface);
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

    // Chain onto whatever's currently in the IAT slot (real d3d9.dll, or
    // another mod's hook -- e.g. shadowfix -- if it patched first). See
    // file header for how this keeps both mods' hooks composed correctly
    // regardless of mad2/mods/ load order.
    void* prev = nullptr;
    int n = Mad2HookUtil_PatchIatAllModules("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(HookDirect3DCreate9), &prev);
    if (prev) RealDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(prev);
    Log("Patched Direct3DCreate9 in %d module(s) (chained real=%p)\n", n, (void*)RealDirect3DCreate9);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            LoadTimerState();
            InstallHooks();
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
