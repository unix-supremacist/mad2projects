// Madagascar 2 player coordinate overlay.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Ports the coordinate
// readout from the external ../mad2mod SDL overlay tool into an in-process
// D3D9 overlay, the same way mad2igttimer ported that tool's timer.
//
// Hooking strategy: identical to mad2igttimer -- Direct3DCreate9 is
// hooked at the IAT level, then IDirect3D9::CreateDevice (vtable slot 16)
// and IDirect3DDevice9::EndScene (vtable slot 42) are vtable-patched
// directly rather than implementing a full COM proxy. See
// mad2igttimer/src/igttimer.cpp's file header for the detailed
// composability argument -- it applies unchanged here: this mod, igttimer,
// and shadowfix can all vtable-patch the same two slots in any load
// order and correctly chain onto each other, because each hook captures
// whatever was in the slot before it (real function or another mod's
// hook) and calls through to that.
//
// Position read: ../mad2mod's follow_chain() walked a pointer chain from
// ActorInfoLib.dll's base out to the player actor's position struct over
// process_vm_readv, tolerating unmapped reads by just failing that one
// read. In-process, the same chain is just repeated pointer dereferences
// -- but an invalid dereference here is a real access violation that
// crashes the whole game, not a syscall that fails gracefully. So each
// hop is checked against a plausible-user-mode-pointer range before it's
// followed (see IsPlausiblePointer/FollowChain) -- this doesn't make the
// chain bulletproof against a garbage-but-plausible-looking pointer, but
// it covers the real failure mode: a null/near-null intermediate pointer
// in the brief window after ActorInfoLib.dll loads but before the player
// actor is fully constructed (e.g. on the title screen).
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#include "../../mad2config/include/mad2config_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2textrenderer/include/mad2textrenderer_api.h"

// ---------------------------------------------------------------------
// Logging -- writes into the shared logs\mad2.log (see mad2sharedlog.h),
// tagged "[mad2playercoords]" so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2playercoords]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [PlayerCoords] section, resolved lazily on the
// first EndScene call -- see mad2config/include/mad2config_api.h for why
// this can't happen from DllMain: mad2/mods/*.dll load order isn't
// guaranteed, so mad2config.dll might not be loaded yet this early.)
// ---------------------------------------------------------------------

struct Config {
    int overlayX = 20;
    int overlayY = 140; // below igttimer's default (20,20) overlay so the two don't overlap
    int fontSize = 24;
    bool showX = true;
    bool showY = true;
    bool showZ = true;
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

    g_Config.overlayX = api.GetInt("PlayerCoords", "X", g_Config.overlayX,
                                    "Screen-space X position (pixels) of the coordinate overlay's top-left corner.");
    g_Config.overlayY = api.GetInt("PlayerCoords", "Y", g_Config.overlayY,
                                    "Screen-space Y position (pixels) of the coordinate overlay's top-left corner.");
    g_Config.fontSize = api.GetInt("PlayerCoords", "FontSize", g_Config.fontSize,
                                    "Pixel height of the coordinate overlay's font.");
    g_Config.showX = api.GetBool("PlayerCoords", "ShowX", TRUE, "Show the player's X coordinate.") != FALSE;
    g_Config.showY =
        api.GetBool("PlayerCoords", "ShowY", TRUE, "Show the player's Y coordinate (vertical/jump height).") !=
        FALSE;
    g_Config.showZ = api.GetBool("PlayerCoords", "ShowZ", TRUE, "Show the player's Z coordinate.") != FALSE;

    Log("Config loaded: overlayX=%d overlayY=%d fontSize=%d showX=%d showY=%d showZ=%d\n", g_Config.overlayX,
        g_Config.overlayY, g_Config.fontSize, g_Config.showX, g_Config.showY, g_Config.showZ);
}

// IAT/vtable hooking now provided by aa_mad2hookutil.dll -- see
// ../../mad2hookutil/include/mad2hookutil_api.h. Resolved synchronously
// (not lazily) since aa_mad2hookutil.dll is guaranteed already loaded by
// the time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Player position read
// ---------------------------------------------------------------------

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

static Vec3 g_PlayerPos;
static bool g_HasPos = false;

static bool IsPlausiblePointer(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

// Mirrors ../mad2mod/src/main.cpp's follow_chain(): reads a pointer at
// `base`, then repeatedly reads a new pointer at (previous + offset) for
// each offset in the chain. See file header for why each hop is checked
// before being followed.
static uintptr_t FollowChain(uintptr_t base, const uintptr_t* offsets, size_t count) {
    uintptr_t addr = *reinterpret_cast<uint32_t*>(base);
    for (size_t i = 0; i < count; ++i) {
        if (!IsPlausiblePointer(addr)) return 0;
        addr = *reinterpret_cast<uint32_t*>(addr + offsets[i]);
    }
    return addr;
}

// Same offset chain the external tool used (see shuffle_object_coordinates/
// teleport_player_randomly/the per-frame position read in
// ../mad2mod/src/main.cpp): ActorInfoLib.dll base + 0x1A030, then this
// chain, lands on an object whose +0xD0/+0xD4/+0xD8 floats are the
// player's x/y/z.
static const uintptr_t kCoordOffsets[] = {0x1C, 0x20, 0x148, 0x8, 0x160, 0x54};

static void UpdatePlayerPosition() {
    static HMODULE actorInfoLib = nullptr;
    if (!actorInfoLib) {
        actorInfoLib = GetModuleHandleA("ActorInfoLib.dll");
        if (!actorInfoLib) {
            g_HasPos = false;
            return;
        }
        Log("Found ActorInfoLib.dll at %p\n", (void*)actorInfoLib);
    }

    uintptr_t coordPtr = FollowChain(reinterpret_cast<uintptr_t>(actorInfoLib) + 0x0001A030, kCoordOffsets,
                                      sizeof(kCoordOffsets) / sizeof(kCoordOffsets[0]));
    if (coordPtr <= 0x1000) {
        g_HasPos = false;
        return;
    }

    const float* pos = reinterpret_cast<const float*>(coordPtr + 0xD0);
    g_PlayerPos.x = pos[0];
    g_PlayerPos.y = pos[1];
    g_PlayerPos.z = pos[2];
    g_HasPos = true;
}

static void FormatCoordLine(char* buf, size_t bufSize, const char* axis, bool hasPos, float value) {
    if (hasPos) {
        snprintf(buf, bufSize, "%s: %.3f", axis, value);
    } else {
        snprintf(buf, bufSize, "%s: --", axis);
    }
}

// ---------------------------------------------------------------------
// Overlay rendering (identical technique to mad2igttimer/src/igttimer.cpp
// -- see that file for the two-pass mask/color rationale).
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

    int textW = api.MeasureTextWidth(g_Surface, "X: -99999.999");
    int lineHeight = api.GetLineHeight(g_Surface);
    int texW = textW + kPadding * 2 + kOutlineOffset * 2;
    int texH = lineHeight * lineCount + kPadding + kOutlineOffset * 2;

    return api.EnsureSize(g_Surface, dev, texW, texH) != FALSE;
}

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
// IDirect3DDevice9::EndScene vtable patch.
// ---------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    EnsureConfigLoaded();
    UpdatePlayerPosition();

    char xBuf[64], yBuf[64], zBuf[64];
    const char* lines[3];
    int lineCount = 0;
    if (g_Config.showX) {
        FormatCoordLine(xBuf, sizeof(xBuf), "X", g_HasPos, g_PlayerPos.x);
        lines[lineCount++] = xBuf;
    }
    if (g_Config.showY) {
        FormatCoordLine(yBuf, sizeof(yBuf), "Y", g_HasPos, g_PlayerPos.y);
        lines[lineCount++] = yBuf;
    }
    if (g_Config.showZ) {
        FormatCoordLine(zBuf, sizeof(zBuf), "Z", g_HasPos, g_PlayerPos.z);
        lines[lineCount++] = zBuf;
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

    void* prev = nullptr;
    int n = Mad2HookUtil_PatchIatAllModules("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(HookDirect3DCreate9), &prev);
    if (prev) RealDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(prev);
    Log("Patched Direct3DCreate9 in %d module(s) (chained real=%p)\n", n, (void*)RealDirect3DCreate9);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            InstallHooks();
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
