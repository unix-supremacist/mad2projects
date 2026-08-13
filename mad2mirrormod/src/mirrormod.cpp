// mad2mirrormod: "Mirror Mode" -- horizontally flips the rendered frame and
// inverts the left/right analog stick X axis to match. Unlike
// mad2graphicseffectmod's effects, this is a persistent, full-session
// toggle driven by config.cfg's [Mirror] section (live-reloaded every ~1s
// so the in-game debug menu's Config tab checkbox actually takes effect --
// see ReloadConfigLive), not a randomized mad2effects chaos effect --
// there's nothing to register with mad2effects here. Used to also have its
// own dedicated ToggleKey hotkey; removed as redundant (and a source of
// live-reload/controller-input glitches) once the debug menu's UI checkbox
// covered the same toggle.
//
// Input half uses mad2xinput's existing invertLX/invertRX override fields
// -- no new XInput hooking needed, this mod is purely a consumer of both
// mad2xinput and mad2config.
//
// Visual half has TWO techniques. Only technique 2 is currently wired in
// (see kEnableProjectionTechnique, near the top of the anonymous namespace
// below) -- technique 1 is fully implemented and left in place for later,
// but two in-game attempts at fixing why it wasn't working (see its own
// entry below) still produced the same broken result, so it's disabled by
// default until that's actually root-caused. What follows describes both
// as designed; treat technique 1's description as "what happens if
// kEnableProjectionTechnique is flipped back to true", not current default
// behavior.
//
//   1. Projection-matrix mirror (HookSetTransform/HookSetRenderState) --
//      the HUD-safe technique, when enabled. When the game calls SetTransform(
//      D3DTS_PROJECTION, matrix) with something that looks like a real 3D
//      perspective projection (heuristic: _34 != 0 -- true for a
//      standard D3DXMatrixPerspective*-style matrix, false for an
//      orthographic one, which is what a 2D HUD projection normally looks
//      like), we negate _11 (the X-axis scale term) before passing it
//      through. This mirrors the 3D world natively, in clip space, before
//      it's ever rasterized -- so 2D/screen-space HUD draws (either a
//      distinct orthographic SetTransform(D3DTS_PROJECTION, ...) call, or
//      D3DFVF_XYZRHW pretransformed vertices that skip the transform
//      pipeline entirely) are simply never touched. A true mirror is an
//      orientation-reversing transform, which flips triangle winding as
//      the rasterizer sees it -- so HookSetRenderState also inverts
//      D3DRS_CULLMODE (CW<->CCW, D3DCULL_NONE untouched) for as long as
//      the currently-active projection is a mirrored one, or every
//      backface-culled object would render inside-out -- this was a real,
//      observed bug on the first attempt (the compensation existed but was
//      only reactive, so it never fired if the game just set cull mode
//      once and left it alone) before SetCullModeForCurrentState's
//      proactive fix below. The broken look was striking enough on its own
//      that it now also lives on independently as mad2graphicseffectmod's
//      "InvertedCulling" ("Inside-Out World") chaos effect -- same
//      technique, no projection-mirroring involved, just the cull-mode
//      inversion alone.
//
//   2. Full-backbuffer mirror (HookEndScene/DrawMirroredQuad) -- the
//      fallback, and the ONLY technique used until/unless technique 1
//      actually engages: capture the backbuffer, redraw it as one
//      screen-space quad with the left/right UV coordinates swapped (the
//      same trick mad2graphicseffectmod's own InvertX effect uses). This
//      unavoidably also mirrors the game's own native HUD (baked into the
//      backbuffer by the time EndScene fires, long before any mod sees the
//      frame) -- but it's guaranteed to produce *some* correct mirror even
//      if the game's main 3D pass turns out not to route through the
//      fixed-function D3DTS_PROJECTION state at all (e.g. if it sets
//      transforms via vertex shader constants instead, which technique 1
//      has no way to intercept -- reverse-engineering an unknown shader's
//      constant-register layout well enough to safely rewrite it blind
//      isn't attempted here). g_ProjectionTechniqueEngaged latches true
//      the first time technique 1 successfully mirrors a perspective
//      projection, and HookEndScene stops doing its own backbuffer flip
//      from that frame on (doing both at once would double-flip). This
//      switch has NOT been verified in-game (this repo's own tooling can't
//      drive/screenshot the running game -- see CLAUDE.md) -- config.cfg's
//      ForceBackbufferMirror key forces technique 2 unconditionally if
//      technique 1 turns out to misbehave for this specific game/engine.
//
// Deployed with a "zz_" prefix (sorting after every other overlay mod,
// right behind zz_mad2graphicseffectmod itself) so that even while falling
// back to technique 2, every other project overlay (mad2effectshud,
// mad2igttimer, mad2playercoords, mad2twitchchat, mad2musichud -- none of
// which are "zz_"-prefixed) loads/hooks earlier and therefore draws its
// own overlay LATER, landing on top of the already-mirrored frame instead
// of being captured and flipped along with it -- see CLAUDE.md's "D3D9
// overlay hooking" section. So even in the worst case (stuck on technique
// 2), only the game's own built-in HUD elements end up mirrored, not any
// other mad2 mod's overlay.
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2xinput/include/mad2xinput_api.h"
#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

namespace {

// Set to true to re-enable the projection-matrix mirroring technique (see
// the file header's "technique 1"). Currently OFF: two attempted fixes (a
// reactive-only cull-mode compensation, then a render-target-signature
// check meant to exclude the shadow pass) each still produced the same
// broken-culling-with-nothing-visibly-mirrored symptom in actual in-game
// testing, and the real root cause hasn't been pinned down yet. Rather
// than rip the code out, this one constant is the entire "wiring" --
// HookCreateDevice only installs the SetTransform/SetRenderState vtable
// hooks (technique 1's actual mechanism) when this is true, so with it
// false the D3D9 runtime never calls into any of technique 1's code at
// all, and mirror mode always uses the full-backbuffer fallback (technique
// 2) unconditionally -- HUD included, but reliable, exactly what shipped
// before technique 1 was attempted. Everything else in this file (the
// helper functions, the config option, HookEndScene's now-permanently-true
// fallback condition) is untouched and still fully wired to each other, so
// flipping this back to true is the only step needed to pick this up again.
constexpr bool kEnableProjectionTechnique = false;

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2mirrormod]", fmt, args);
    va_end(args);
}

struct Config {
    bool enabledAtStart = false;
    DWORD userIndex = 0;
    bool forceBackbufferMirror = false;
};
Config g_Config;

std::atomic<bool> g_MirrorActive{false};
Mad2XInputOverrideHandle g_OverrideHandle = 0;

// Sticky: latches true the first time HookSetTransform successfully mirrors
// a perspective D3DTS_PROJECTION while active, and never resets except when
// mirror mode itself is toggled off (see SetMirrorActive) -- once true,
// HookEndScene stops performing its own full-backbuffer flip.
std::atomic<bool> g_ProjectionTechniqueEngaged{false};

// NOT sticky -- reflects whether the MOST RECENTLY set D3DTS_PROJECTION
// (while mirror mode is active) was one HookSetTransform mirrored, so
// HookSetRenderState knows whether to invert D3DRS_CULLMODE right now. A
// game that switches between a mirrored 3D perspective projection and an
// untouched 2D/orthographic HUD projection within the same frame needs this
// to track the CURRENT state, not just "have we ever seen one".
std::atomic<bool> g_CullShouldInvert{false};

// One-shot diagnostic log gate -- see its use in HookSetTransform. Reset on
// every SetMirrorActive(true) so re-toggling logs fresh instead of staying
// silent because a previous activation already logged once.
std::atomic<bool> g_LoggedSkippedOffscreenPerspective{false};

// --- D3D9 hook state -- same bootstrap shape as every other D3D9-hooking
// overlay mod: IAT-hook Direct3DCreate9 -> vtable-hook IDirect3D9::
// CreateDevice[16] -> on success, vtable-hook IDirect3DDevice9::
// EndScene[42]/Reset[16]/SetTransform[44]/SetRenderState[57] (indices
// verified directly against this toolchain's own d3d9.h interface
// declaration order, not just recalled from memory).
typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* SetTransform_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
typedef HRESULT(STDMETHODCALLTYPE* SetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);

EndScene_t RealEndScene = nullptr;
Reset_t RealReset = nullptr;
SetTransform_t RealSetTransform = nullptr;
SetRenderState_t RealSetRenderState = nullptr;
CreateDevice_t RealCreateDevice = nullptr;
Direct3DCreate9_t RealDirect3DCreate9 = nullptr;
bool g_DeviceHooked = false;
// Captured once in HookCreateDevice -- there is only ever one live device
// for this process's lifetime in practice (a Reset() reuses the same
// object). Lets SetMirrorActive(false) proactively restore cull mode when
// toggled off mid-mirror, the same way HookSetTransform does on an
// in-frame transition -- see its own comment for why a passive-only fix
// (waiting for the game to call SetRenderState/SetTransform again) isn't
// enough on its own.
IDirect3DDevice9* g_Device = nullptr;

IDirect3DTexture9* g_CaptureTex = nullptr;
IDirect3DSurface9* g_CaptureSurf = nullptr;

struct TexVertex {
    float x, y, z, rhw, u, v;
};

void ReleaseCaptureTarget() {
    if (g_CaptureSurf) {
        g_CaptureSurf->Release();
        g_CaptureSurf = nullptr;
    }
    if (g_CaptureTex) {
        g_CaptureTex->Release();
        g_CaptureTex = nullptr;
    }
}

bool EnsureCaptureTarget(IDirect3DDevice9* dev, const D3DSURFACE_DESC& desc) {
    if (g_CaptureTex) return true;
    if (FAILED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
                                   &g_CaptureTex, nullptr))) {
        return false;
    }
    g_CaptureTex->GetSurfaceLevel(0, &g_CaptureSurf);
    return true;
}

// Save/restore just the fixed-function state this mod's single draw call
// touches -- same shape as mad2graphicseffectmod's own SaveState/
// RestoreState, scoped down since this mod only ever draws one quad.
struct SavedState {
    IDirect3DBaseTexture9* tex0 = nullptr;
    DWORD lighting = 0, zEnable = 0, alphaBlend = 0, fog = 0, colorWrite = 0, cullMode = 0, fvf = 0;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DSurface9* rt = nullptr;
};

void SaveState(IDirect3DDevice9* dev, SavedState& s) {
    dev->GetTexture(0, &s.tex0);
    dev->GetRenderState(D3DRS_LIGHTING, &s.lighting);
    dev->GetRenderState(D3DRS_ZENABLE, &s.zEnable);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &s.alphaBlend);
    dev->GetRenderState(D3DRS_FOGENABLE, &s.fog);
    dev->GetRenderState(D3DRS_COLORWRITEENABLE, &s.colorWrite);
    dev->GetRenderState(D3DRS_CULLMODE, &s.cullMode);
    dev->GetFVF(&s.fvf);
    dev->GetVertexShader(&s.vs);
    dev->GetPixelShader(&s.ps);
    dev->GetRenderTarget(0, &s.rt);
}

void RestoreState(IDirect3DDevice9* dev, SavedState& s) {
    dev->SetTexture(0, s.tex0);
    if (s.tex0) s.tex0->Release();
    dev->SetRenderState(D3DRS_LIGHTING, s.lighting);
    dev->SetRenderState(D3DRS_ZENABLE, s.zEnable);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, s.alphaBlend);
    dev->SetRenderState(D3DRS_FOGENABLE, s.fog);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, s.colorWrite);
    dev->SetRenderState(D3DRS_CULLMODE, s.cullMode);
    dev->SetFVF(s.fvf);
    dev->SetVertexShader(s.vs);
    if (s.vs) s.vs->Release();
    dev->SetPixelShader(s.ps);
    if (s.ps) s.ps->Release();
    dev->SetRenderTarget(0, s.rt);
    if (s.rt) s.rt->Release();
}

// u0=1,u1=0 (vs. the identity 0,0,1,1 every other passthrough quad in this
// codebase uses) so the screen's left edge samples the source's right edge
// and vice versa -- the exact horizontal-mirror trick
// mad2graphicseffectmod's own InvertX effect uses (Render_InvertX in
// graphicseffect.cpp), just applied unconditionally here instead of as one
// pipeline stage among many.
void DrawMirroredQuad(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH) {
    dev->SetTexture(0, src);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                     D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

    float w = static_cast<float>(vpW), h = static_cast<float>(vpH);
    TexVertex verts[4] = {
        {0, 0, 0, 1, 1, 0}, {w, 0, 0, 1, 0, 0}, {0, h, 0, 1, 1, 1}, {w, h, 0, 1, 0, 1},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(TexVertex));
}

HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* dev) {
    // Only fall back to the full-backbuffer mirror while the HUD-safe
    // projection-matrix technique hasn't (yet, or ever) actually engaged --
    // see the file header for why both exist and why running both at once
    // would double-flip the frame back to unmirrored.
    if (g_MirrorActive.load() && !g_ProjectionTechniqueEngaged.load()) {
        IDirect3DSurface9* backSurf = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backSurf)) && backSurf) {
            D3DSURFACE_DESC desc;
            backSurf->GetDesc(&desc);
            D3DVIEWPORT9 vp{};
            dev->GetViewport(&vp);

            if (EnsureCaptureTarget(dev, desc)) {
                SavedState saved;
                SaveState(dev, saved);

                dev->StretchRect(backSurf, nullptr, g_CaptureSurf, nullptr, D3DTEXF_NONE);
                dev->SetRenderTarget(0, backSurf);
                DrawMirroredQuad(dev, g_CaptureTex, static_cast<int>(vp.Width), static_cast<int>(vp.Height));

                RestoreState(dev, saved);
            }
            backSurf->Release();
        }
    }
    return RealEndScene(dev);
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    // D3DPOOL_DEFAULT render targets must be released before Reset() and
    // recreated after, or Reset() itself can fail outright -- the same bug
    // class mad2graphicseffectmod was fixed for (improvement-plan.md
    // decision #2); applied here proactively from the start.
    ReleaseCaptureTarget();
    return RealReset(dev, pp);
}

// _34 != 0 is a standard, cheap perspective-vs-orthographic discriminator:
// a D3DXMatrixPerspective*-style matrix has _34 = -1 (enables the
// perspective divide); a D3DXMatrixOrtho*-style matrix has _34 = 0, _44 = 1.
// 2D HUD projections are conventionally orthographic, so this is what lets
// HookSetTransform mirror the 3D world's projection while leaving a
// distinct HUD projection call untouched. On its own this ISN'T enough,
// though -- see IsRenderingToBackbuffer below for why.
bool IsLikelyPerspectiveMatrix(const D3DMATRIX& m) { return m._34 != 0.0f; }

// This game has at least one other perspective-shaped projection besides
// the main camera: mad2shadowfix documents a dedicated shadow pass
// rendering to its own offscreen 1024x1024 D3DFMT_R32F target, which needs
// its own light-space projection matrix -- also very likely _34 != 0. The
// perspective check alone can't tell that apart from the main camera's
// projection, and mirroring the WRONG one is worse than mirroring nothing:
// it still latches g_ProjectionTechniqueEngaged (permanently disabling the
// reliable full-backbuffer fallback in HookEndScene) and starts inverting
// cull mode globally, without ever actually mirroring anything the player
// can see -- exactly the symptom observed (culling broken, world not
// visibly mirrored). The main camera's projection, by definition, is only
// ever in effect while actually rendering to the real backbuffer -- an
// offscreen pass like the shadow map has its own render target bound via
// SetRenderTarget first. Comparing GetRenderTarget(0) against
// GetBackBuffer() (both read-only queries into the real, un-hooked device,
// safe to call from inside this hook) is the same render-target-signature
// technique mad2shadowfix itself uses to detect its own shadow pass.
bool IsRenderingToBackbuffer(IDirect3DDevice9* dev) {
    IDirect3DSurface9* currentRT = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &currentRT)) || !currentRT) return false;
    IDirect3DSurface9* backBuf = nullptr;
    bool isBackbuffer = false;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuf)) && backBuf) {
        isBackbuffer = (currentRT == backBuf);
        backBuf->Release();
    }
    currentRT->Release();
    return isBackbuffer;
}

DWORD InvertCullMode(DWORD mode) {
    if (mode == D3DCULL_CW) return D3DCULL_CCW;
    if (mode == D3DCULL_CCW) return D3DCULL_CW;
    return mode;  // D3DCULL_NONE (or anything unexpected) passes through
}

// The game's actual (unmirrored) cull mode request, tracked from every
// SetRenderState(D3DRS_CULLMODE, ...) call regardless of whether mirroring
// is currently active -- what SetCullModeForCurrentState below re-applies
// (inverted or not) whenever g_CullShouldInvert *changes*, so the
// correction doesn't depend on the game happening to re-issue
// SetRenderState again after that transition. Initialized to D3DCULL_CCW,
// D3D9's own documented default device state, in case mirroring engages
// before the game has ever called SetRenderState(CULLMODE) itself (hooks
// are installed synchronously right after CreateDevice succeeds, before
// the game gets a chance to configure anything on the new device, so this
// default should never actually matter in practice -- kept as a safe seed
// regardless).
DWORD g_LastRequestedCullMode = D3DCULL_CCW;

// Forces the device's cull mode to match g_LastRequestedCullMode, inverted
// if `invert`. Called on every g_CullShouldInvert transition (not just
// reactively from HookSetRenderState) -- this is the actual bug fix: a
// game that sets cull mode once (e.g. during device init) and never
// touches it again for the rest of the session would otherwise never get
// corrected at all, leaving front-facing geometry incorrectly culled for
// the whole time mirroring is active (observed: large solid-black voids
// where terrain should be, only a few coincidentally-still-front-facing
// surfaces visible -- exactly the signature of uncompensated inverted
// culling). Calls RealSetRenderState directly, not through HookSetRenderState,
// to avoid double-inverting.
void SetCullModeForCurrentState(IDirect3DDevice9* dev, bool invert) {
    if (!RealSetRenderState) return;
    DWORD wanted = invert ? InvertCullMode(g_LastRequestedCullMode) : g_LastRequestedCullMode;
    RealSetRenderState(dev, D3DRS_CULLMODE, wanted);
}

HRESULT STDMETHODCALLTYPE HookSetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* matrix) {
    if (g_MirrorActive.load() && !g_Config.forceBackbufferMirror && state == D3DTS_PROJECTION && matrix) {
        bool perspective = IsLikelyPerspectiveMatrix(*matrix);
        bool toBackbuffer = perspective && IsRenderingToBackbuffer(dev);
        if (perspective && !toBackbuffer && !g_LoggedSkippedOffscreenPerspective.exchange(true)) {
            Log("Saw a perspective-shaped projection targeting something other than the backbuffer (a shadow "
                "pass or similar) -- skipping it, not treating it as the main camera. Logged once.\n");
        }
        if (toBackbuffer) {
            D3DMATRIX mirrored = *matrix;
            mirrored._11 = -mirrored._11;
            if (!g_CullShouldInvert.exchange(true)) {
                SetCullModeForCurrentState(dev, true);
            }
            if (!g_ProjectionTechniqueEngaged.exchange(true)) {
                Log("3D perspective projection detected and mirrored -- switching off the full-screen "
                    "fallback mirror; the game's own native HUD should no longer be flipped from here on.\n");
            }
            return RealSetTransform(dev, state, &mirrored);
        }
        // Either looked orthographic (a HUD projection, most likely) or
        // targeted an offscreen render target (a shadow pass or similar,
        // not the main camera) -- leave it untouched, and restore the
        // game's own real cull mode (rather than whatever we last forced
        // for the 3D pass) for as long as we're out of the mirrored
        // projection.
        if (g_CullShouldInvert.exchange(false)) {
            SetCullModeForCurrentState(dev, false);
        }
    }
    return RealSetTransform(dev, state, matrix);
}

HRESULT STDMETHODCALLTYPE HookSetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD value) {
    if (state == D3DRS_CULLMODE) {
        g_LastRequestedCullMode = value;  // remember what the game actually wants, unmirrored
        // A true mirror is an orientation-reversing transform -- it flips
        // triangle winding as the rasterizer sees it, so whatever cull mode
        // the game asks for needs inverting too, or every backface-culled
        // object renders inside-out while the mirrored projection is
        // active. (The transition-time correction in HookSetTransform above
        // handles the case where the game never calls this again at all --
        // this handles every subsequent explicit change.)
        if (g_MirrorActive.load() && g_CullShouldInvert.load()) {
            value = InvertCullMode(value);
        }
    }
    return RealSetRenderState(dev, state, value);
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d3d9, UINT adapter, D3DDEVTYPE devType, HWND hwnd,
                                            DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** outDevice) {
    HRESULT hr = RealCreateDevice(d3d9, adapter, devType, hwnd, flags, pp, outDevice);
    if (SUCCEEDED(hr) && *outDevice && !g_DeviceHooked) {
        g_DeviceHooked = true;
        g_Device = *outDevice;
        Mad2HookUtil_PatchVTableSlot(*outDevice, 42, reinterpret_cast<void*>(HookEndScene),
                                      reinterpret_cast<void**>(&RealEndScene));
        Mad2HookUtil_PatchVTableSlot(*outDevice, 16, reinterpret_cast<void*>(HookReset),
                                      reinterpret_cast<void**>(&RealReset));
        if (kEnableProjectionTechnique) {
            // Technique 1's actual "wiring" -- see kEnableProjectionTechnique's
            // own comment. With it false, these two calls never run, so
            // RealSetTransform/RealSetRenderState stay null and
            // HookSetTransform/HookSetRenderState are never invoked by the
            // D3D9 runtime at all (their addresses are still referenced right
            // here, though, so they're not flagged as unused code).
            Mad2HookUtil_PatchVTableSlot(*outDevice, 44, reinterpret_cast<void*>(HookSetTransform),
                                          reinterpret_cast<void**>(&RealSetTransform));
            Mad2HookUtil_PatchVTableSlot(*outDevice, 57, reinterpret_cast<void*>(HookSetRenderState),
                                          reinterpret_cast<void**>(&RealSetRenderState));
            Log("D3D9 device hooked (EndScene/Reset/SetTransform/SetRenderState)\n");
        } else {
            Log("D3D9 device hooked (EndScene/Reset only -- projection-matrix technique disabled, see "
                "kEnableProjectionTechnique)\n");
        }
    }
    return hr;
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion) {
    IDirect3D9* d3d9 = RealDirect3DCreate9(sdkVersion);
    if (d3d9 && !RealCreateDevice) {
        Mad2HookUtil_PatchVTableSlot(d3d9, 16, reinterpret_cast<void*>(HookCreateDevice),
                                      reinterpret_cast<void**>(&RealCreateDevice));
    }
    return d3d9;
}

// --- Input half: no new hook of our own -- mad2xinput's existing
// invertLX/invertRX override fields do exactly what a stick mirror needs.
void ApplyStickMirror() {
    const auto& api = Mad2XInput_Resolve();
    if (!api.AddOverride) {
        Log("[Dependency] mad2xinput not available -- stick mirroring inactive\n");
        return;
    }
    Mad2XInputOverride ov{};
    ov.invertLX = TRUE;
    ov.invertRX = TRUE;
    g_OverrideHandle = api.AddOverride(g_Config.userIndex, &ov);
}

void ClearStickMirror() {
    const auto& api = Mad2XInput_Resolve();
    if (g_OverrideHandle && api.RemoveOverride) {
        api.RemoveOverride(g_OverrideHandle);
        g_OverrideHandle = 0;
    }
}

void SetMirrorActive(bool active) {
    if (g_MirrorActive.load() == active) return;
    g_MirrorActive.store(active);
    if (active) {
        // Reset detection state on every activation so re-enabling after a
        // toggle-off re-detects fresh rather than trusting stale state from
        // a previous activation (e.g. after a device Reset the game could
        // in principle re-establish its transforms differently).
        g_ProjectionTechniqueEngaged.store(false);
        g_CullShouldInvert.store(false);
        g_LoggedSkippedOffscreenPerspective.store(false);
        ApplyStickMirror();
        if (g_Config.forceBackbufferMirror) {
            Log("Mirror mode ON (ForceBackbufferMirror set -- full-screen mirror only, HUD will be mirrored too)\n");
        } else {
            Log("Mirror mode ON (starting with the full-screen fallback mirror; will switch automatically to "
                "a HUD-safe technique if a 3D perspective projection is detected)\n");
        }
    } else {
        ClearStickMirror();
        // Proactively restore cull mode if we're turning off mid-mirror,
        // the same reasoning as HookSetTransform's own in-frame transition
        // fix: nothing guarantees the game calls SetRenderState/SetTransform
        // again on its own after this, so waiting for that would leave cull
        // mode wrong indefinitely.
        if (g_CullShouldInvert.exchange(false) && g_Device) {
            SetCullModeForCurrentState(g_Device, false);
        }
        g_ProjectionTechniqueEngaged.store(false);
        Log("Mirror mode OFF\n");
    }
}

void LoadConfig() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetBool) {
        Log("[Dependency] mad2config not available -- using built-in defaults (mirror mode off)\n");
        return;
    }
    g_Config.enabledAtStart =
        api.GetBool("Mirror", "Enabled", FALSE,
                    "Start the game in Mirror Mode: the screen is flipped horizontally and the left/right\n"
                    "stick X axis is inverted to match. This is a persistent mode for the whole session,\n"
                    "not a random chaos effect. Tries to mirror the 3D world itself (leaving the game's own\n"
                    "HUD unflipped) and only falls back to mirroring the whole screen (HUD included) if that\n"
                    "doesn't work for this game's rendering -- see ForceBackbufferMirror below to force the\n"
                    "fallback deliberately. Every other mad2 overlay mod's HUD (timer, effects list, coords,\n"
                    "twitch chat, now-playing) stays legible either way. Toggle live via the in-game debug\n"
                    "menu's Config tab (or by hand-editing this value -- picked up within ~1s either way).") !=
        FALSE;
    g_Config.userIndex = static_cast<DWORD>(
        api.GetInt("Mirror", "UserIndex", 0,
                   "Which XInput controller slot (0-3) gets its left/right stick X axis inverted to\n"
                   "match the mirrored screen."));
    g_Config.forceBackbufferMirror =
        api.GetBool("Mirror", "ForceBackbufferMirror", FALSE,
                     "Force the simple full-screen mirror (which also flips the game's own HUD) instead of\n"
                     "trying the HUD-preserving 3D-projection technique first. Set this to true if Mirror\n"
                     "Mode looks visually broken (e.g. objects rendering inside-out) -- that would mean this\n"
                     "game's rendering doesn't cooperate with the smarter technique.") != FALSE;
}

// Mirror Mode's settings are edited via the debug menu's generic "Raw
// Config" tab now (mad2settings.dll registration removed -- it was
// redundant once Raw Config could show/edit every config.cfg key, vkeys
// included). Raw Config only ever writes straight to config.cfg though --
// it has no way to reach into this mod's own live g_Config -- so this mod
// has to notice those edits itself. ReloadConfigLive re-reads Enabled/
// ForceBackbufferMirror/ToggleKey every ~1s (see the polling loop below)
// and, if Enabled actually changed, drives SetMirrorActive itself (a plain
// pointer write wouldn't engage/disengage ApplyStickMirror/
// ClearStickMirror -- SetMirrorActive's side effects are what actually
// matter here).
void ReloadConfigLive() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetBool) return;
    bool wantEnabled = api.GetBool("Mirror", "Enabled", g_Config.enabledAtStart ? TRUE : FALSE, "") != FALSE;
    g_Config.forceBackbufferMirror =
        api.GetBool("Mirror", "ForceBackbufferMirror", g_Config.forceBackbufferMirror ? TRUE : FALSE, "") != FALSE;
    g_Config.enabledAtStart = wantEnabled;
    if (wantEnabled != g_MirrorActive.load()) SetMirrorActive(wantEnabled);
}

DWORD WINAPI InitThreadFunc(LPVOID) {
    for (int i = 0; i < 100; ++i) {
        if (Mad2Config_Resolve().GetBool && Mad2XInput_Resolve().AddOverride) break;
        Sleep(50);
    }
    LoadConfig();
    SetMirrorActive(g_Config.enabledAtStart);

    int reloadCounter = 0;
    for (;;) {
        if (++reloadCounter >= 20) {  // ~1s at the 50ms sleep below
            reloadCounter = 0;
            ReloadConfigLive();
        }
        Sleep(50);
    }
    return 0;
}

// IAT-hook Direct3DCreate9 synchronously from DllMain -- aa_mad2hookutil.dll
// is guaranteed already loaded by the time any other mod's DllMain runs
// (see mad2hookutil_api.h's own header comment), so this doesn't need the
// lazy-resolve-from-a-background-thread pattern mad2xinput/mad2config
// consumers use.
void InstallD3D9Hook() {
    Mad2HookUtil_PatchIatAllModules("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(HookDirect3DCreate9),
                                     reinterpret_cast<void**>(&RealDirect3DCreate9));
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InstallD3D9Hook();
        CreateThread(nullptr, 0, InitThreadFunc, nullptr, 0, nullptr);
    }
    return TRUE;
}
