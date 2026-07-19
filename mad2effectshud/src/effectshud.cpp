// Active chaos-effects HUD for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Every frame, queries
// mad2effects' registry (mad2/mods/mad2effects.dll -- see
// ../../mad2effects/include/mad2effects_api.h) for which effects are
// currently active and draws their display names as a small list in the
// bottom-right corner. Two purposes: lets a player (or streamer audience)
// see what's currently happening to them, and gives a debugging aid for
// correlating "what's on screen right now" with "what mad2effects thinks
// is active" -- useful when chasing down an effect that doesn't look like
// it's doing what it should.
//
// D3D9 hook technique and GDI-rasterized-text-to-texture rendering are
// the same as mad2igttimer/mad2twitchchat (no D3DX dependency -- see their
// file headers for why). Unlike igttimer's fixed-content overlay, the
// active-effect list changes size and content at runtime, so this follows
// mad2twitchchat's pattern instead: the texture is only rebuilt when the
// active list actually changes (tracked via a simple content comparison,
// not a full diff), and recreated at a new size only when the required
// dimensions change.
//
// Inherits the same draw-ordering limitation mad2graphicseffectmod's file
// header documents: if this mod's EndScene hook happens to run BEFORE
// graphicseffectmod's in the chain (mad2/mods/ load order isn't
// guaranteed), this HUD's own text gets captured and transformed along
// with the rest of the frame by whatever graphics effect is active
// (inverted, rotated, pixelated, ...). Not fixed here for the same reason
// graphicseffectmod doesn't fix it -- no cross-mod draw-ordering
// infrastructure exists in this codebase. In practice this is somewhat
// useful even when it happens: a visibly warped HUD is still evidence an
// effect is active.
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2effects/include/mad2effects_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2textrenderer/include/mad2textrenderer_api.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2effectshud]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [EffectsHud] section, resolved lazily on the
// first EndScene call -- see mad2config/include/mad2config_api.h for why
// this can't happen from DllMain).
// ---------------------------------------------------------------------

struct Config {
    bool enabled = true;
    int fontSize = 16;
    int marginRight = 20;
    int marginBottom = 20;
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

    g_Config.enabled =
        api.GetBool("EffectsHud", "Enabled", TRUE, "Show a list of currently active chaos effects, bottom-right.") !=
        FALSE;
    g_Config.fontSize = api.GetInt("EffectsHud", "FontSize", g_Config.fontSize, "Pixel height of the HUD font.");
    g_Config.marginRight =
        api.GetInt("EffectsHud", "MarginRight", g_Config.marginRight, "Margin (pixels) from the right edge of the screen.");
    g_Config.marginBottom = api.GetInt("EffectsHud", "MarginBottom", g_Config.marginBottom,
                                        "Margin (pixels) from the bottom edge of the screen.");

    Log("Config loaded: enabled=%d fontSize=%d marginRight=%d marginBottom=%d\n", g_Config.enabled,
        g_Config.fontSize, g_Config.marginRight, g_Config.marginBottom);
}

// IAT/vtable hooking now provided by aa_mad2hookutil.dll -- see
// ../../mad2hookutil/include/mad2hookutil_api.h. Resolved synchronously
// (not lazily) since aa_mad2hookutil.dll is guaranteed already loaded by
// the time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Active-effect list, re-queried every frame from mad2effects.
// ---------------------------------------------------------------------

static std::vector<std::string> QueryActiveEffects() {
    std::vector<std::string> active;
    const auto& api = Mad2Effects_Resolve();
    if (!api.GetCount || !api.GetInfo) return active;

    int count = api.GetCount();
    for (int i = 0; i < count; ++i) {
        Mad2EffectInfo info{};
        if (api.GetInfo(i, &info) && info.isActive) active.emplace_back(info.displayName);
    }
    std::sort(active.begin(), active.end());  // stable order regardless of registration/activation order
    return active;
}

// ---------------------------------------------------------------------
// Overlay rendering: GDI-rasterized-to-texture (no D3DX -- see file
// header). Sized dynamically to fit however many effects are active,
// rebuilt only when that list's content actually changes.
// ---------------------------------------------------------------------

static const int kPadding = 6;
static const int kOutlineOffset = 2;
static const char* kHeaderText = "Active Effects:";

static const COLORREF kColorHeader = RGB(255, 200, 60);
static const COLORREF kColorEffect = RGB(255, 255, 255);

// GDI rasterization/quad-draw now provided by mad2textrenderer.dll -- see
// ../../mad2textrenderer/include/mad2textrenderer_api.h. This mod is the
// dynamic-resize consumer Mad2TextRenderer_EnsureSize's contract calls
// out: the texture is resized to fit however many effects are currently
// active, rebuilt only when that list's content actually changes (see
// HookEndScene's contentChanged check) -- EnsureSize itself is a cheap
// no-op when the requested size already matches, so it's safe to call
// unconditionally whenever content changes.
static Mad2TextSurface g_Surface = nullptr;

static bool EnsureFontResources() {
    if (g_Surface) return true;
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.CreateSurface) return false;
    g_Surface = api.CreateSurface(g_Config.fontSize);
    return g_Surface != nullptr;
}

static int MeasureTextWidth(const char* text) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.MeasureTextWidth) return 0;
    return api.MeasureTextWidth(g_Surface, text);
}

// ---------------------------------------------------------------------
// Direct3DCreate9 IAT hook -> IDirect3D9::CreateDevice vtable patch ->
// IDirect3DDevice9::EndScene vtable patch. Identical technique to
// mad2igttimer -- see its file header for why vtable slots 16/42 are
// correct and why this composes with other D3D9-hooking mods regardless of
// mad2/mods/ load order.
// ---------------------------------------------------------------------

static std::vector<std::string> g_LastActive;

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    EnsureConfigLoaded();

    if (!g_Config.enabled || !EnsureFontResources()) return RealEndScene(This);

    std::vector<std::string> active = QueryActiveEffects();
    if (active.empty()) return RealEndScene(This);  // nothing to draw -- zero footprint when chaos mode is idle

    bool contentChanged = (active != g_LastActive);
    const auto& api = Mad2TextRenderer_Resolve();
    bool haveTexture = false;

    if (contentChanged) {
        std::vector<std::string> lines;
        lines.reserve(active.size() + 1);
        lines.emplace_back(kHeaderText);
        for (auto& name : active) lines.push_back(name);

        int maxWidth = 0;
        for (auto& line : lines) maxWidth = std::max(maxWidth, MeasureTextWidth(line.c_str()));

        int lineHeight = api.GetLineHeight ? api.GetLineHeight(g_Surface) : 0;
        int neededW = maxWidth + kPadding * 2 + kOutlineOffset * 2;
        int neededH = static_cast<int>(lines.size()) * lineHeight + kPadding + kOutlineOffset * 2;

        // EnsureSize is a cheap no-op if neededW/neededH already match --
        // safe to call every time content changes, not just on the very
        // first frame (see this mod's own file-header note above).
        if (api.EnsureSize && api.EnsureSize(g_Surface, This, neededW, neededH)) {
            std::vector<Mad2TextRun> runs(lines.size());
            int lineHeightForLayout = api.GetLineHeight(g_Surface);
            for (size_t li = 0; li < lines.size(); ++li) {
                runs[li] = {kPadding + kOutlineOffset,
                            kPadding / 2 + kOutlineOffset + static_cast<int>(li) * lineHeightForLayout,
                            lines[li].c_str(), li == 0 ? kColorHeader : kColorEffect};
            }
            api.Render(g_Surface, runs.data(), static_cast<int>(runs.size()));
            g_LastActive = active;
            haveTexture = true;
        }
    } else {
        int w = 0, h = 0;
        if (api.GetSize) api.GetSize(g_Surface, &w, &h);
        haveTexture = (w > 0 && h > 0);
    }

    if (haveTexture && api.DrawQuad) {
        D3DVIEWPORT9 vp{};
        This->GetViewport(&vp);
        int texW = 0, texH = 0;
        api.GetSize(g_Surface, &texW, &texH);
        float x = static_cast<float>(static_cast<int>(vp.Width) - texW - g_Config.marginRight);
        float y = static_cast<float>(static_cast<int>(vp.Height) - texH - g_Config.marginBottom);
        api.DrawQuad(g_Surface, This, x, y);
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
