// Graphics chaos effects for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Registers twenty effects
// with mad2effects (mad2/mods/mad2effects.dll -- see
// ../../mad2effects/include/mad2effects_api.h), implemented as a D3D9
// EndScene post-process PIPELINE: every frame, if one or more pipeline
// effects are active, the real backbuffer is captured into an offscreen
// texture, then each active effect's Render() transforms that texture
// into the next ping-pong texture (or, for the last active effect, directly
// into the real backbuffer), chaining however many effects happen to be
// simultaneously active via mad2effects/mad2chaosmod. Same D3D9 hook
// technique as mad2igttimer/mad2inputdisplay (Direct3DCreate9 IAT hook ->
// IDirect3D9::CreateDevice vtable patch [16] -> IDirect3DDevice9::EndScene
// vtable patch [42]); only Direct3DCreate9 is hooked, not Ex, matching
// those simpler mods rather than mad2shadowfix's belt-and-suspenders
// handling.
//
// No D3DX (unreliable under Proton/Wine, per mad2igttimer/mad2shadowfix's
// precedent) and no pixel shader compiler is available in this MinGW
// cross-compile toolchain (hand-assembling D3D9 shader bytecode by hand
// without a real compiler was judged too risky to get bit-exact-correct
// blind) -- so every effect here is built from D3D9 FIXED-FUNCTION
// techniques only:
//   - Rotate/Zoom/Pulse: a single screen-aligned quad with a custom UV at
//     each of its 4 corners. D3DFVF_XYZRHW vertices skip the perspective
//     divide, so interpolation across the quad is exactly screen-space
//     linear -- which means a per-corner UV computed from an *affine*
//     transform (rotation + uniform scale) reproduces that transform
//     EXACTLY at every interior pixel, not just approximately. See
//     ComputeAffineCornerUVs.
//   - Sine displace / Melt / Kaleidoscope: these are NOT affine (a sine
//     wave, a per-column ramp, and angular mirroring all vary non-linearly
//     across the screen), so a single quad can't reproduce them exactly.
//     Each is instead tessellated into many thin quads/wedges, each with
//     its own locally-affine UV mapping computed on the CPU -- a real,
//     classic pre-shader technique, just coarser than a true per-pixel
//     warp.
//   - Pixelate: downsample to a small offscreen texture (StretchRect,
//     linear filter) then upsample back to full screen with point
//     filtering.
//   - Eliminate-channel / split-channels: D3DRS_COLORWRITEENABLE masks a
//     draw to affect only specific output channels, letting several
//     passes each touch one channel independently (fixed-function has no
//     single-pass cross-channel routing, but COLORWRITEENABLE + multiple
//     passes gets the same result).
//   - Vignette: a procedurally-baked radial gradient texture, drawn with
//     SRCBLEND=ZERO/DESTBLEND=SRCCOLOR (a multiply blend against whatever
//     is already in the render target).
//   - Echo/blur: a persistent accumulation texture, alpha-blended with
//     each new frame every tick (exponential-decay ghosting, a standard
//     pre-shader motion-blur trick).
//   - 3 FPS cap: simply refuses to re-capture the backbuffer more often
//     than every ~333ms, redrawing the stale capture on the frames between.
//
// A "Wireframe" effect (forcing D3DRS_FILLMODE=WIREFRAME as a persistent
// device state so the GAME's own next-frame draws rasterize as wireframe)
// was attempted and removed: even after fixing the state-leak into other
// mods' HUD draws, it had no visible effect on the game's own rendering --
// almost certainly because the game resets FILLMODE itself as part of its
// own per-object render state setup before each of its draw calls, which a
// state persisted from EndScene has no way to survive. Doing this properly
// would need intercepting IDirect3DDevice9::SetRenderState itself (to veto
// the game's own attempts to reset FILLMODE while active), a materially
// more invasive hook than anything else in this file -- not worth it for
// one effect given everything else here works with the simpler technique.
//
// Three effects are deliberately REINTERPRETED rather than implemented
// literally, because a faithful version needs true per-pixel cross-channel
// math (luminance/permutation), which D3D9 fixed-function genuinely cannot
// do in one or few passes without either a real pixel shader or a
// DOTPRODUCT3-based derivation that (worked through by hand here) crushes
// shadow detail badly enough to not be worth shipping unverified:
//   - "Gameboy color palette" -> a strong green MODULATE tint (the
//     characteristic GB-screen color cast), not true 4-level posterization.
//   - "Mixup the RGB channels" -> each channel gets an independent random
//     darkening multiplier (a chaotic color-cast), not a true channel
//     permutation (there's no fixed-function op that routes one channel's
//     *value* into a *different* output channel without cross-channel
//     math).
//   - "Thermal Inversion (Sin City Mode)" -> the two non-selected color
//     channels are zeroed (COLORWRITEENABLE + black, the same proven
//     technique as "eliminate 1 channel") rather than converted to true
//     grayscale, cycling which channel survives every few seconds. Visually
//     in the right family (one channel "pops", the rest goes dark) without
//     the crushed-shadow risk of a hand-derived grayscale dot product.
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2effects/include/mad2effects_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2graphicseffectmod]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [GraphicsEffects] section -- one section for
// the whole mod, keys prefixed per effect. Weight + duration are
// configurable per effect, matching every other effect mod in this repo;
// "flavor" parameters (speeds, amplitudes, pixel block sizes etc.) are
// fixed constants rather than exposed per-key, to keep this mod's config
// surface bounded given how many effects it has -- see each effect's own
// section below for those constants.)
// ---------------------------------------------------------------------

struct EffectTiming {
    int weight;
    DWORD minDurationMs, maxDurationMs;
};

struct Config {
    EffectTiming invertColors{8, 10000, 20000};
    EffectTiming rotate{8, 8000, 15000};
    EffectTiming zoom{8, 8000, 15000};
    EffectTiming pulse{8, 10000, 20000};
    EffectTiming eliminateChannel{8, 10000, 20000};
    EffectTiming pixelate{8, 10000, 20000};
    EffectTiming invertX{6, 8000, 15000};
    EffectTiming invertY{6, 8000, 15000};
    EffectTiming splitChannels{8, 10000, 20000};
    EffectTiming deepFry{6, 8000, 15000};
    EffectTiming gameboy{6, 10000, 20000};
    EffectTiming mixupRgb{8, 8000, 15000};
    EffectTiming vignette{8, 10000, 20000};
    EffectTiming sineDisplace{8, 8000, 15000};
    EffectTiming kaleidoscope{6, 10000, 20000};
    EffectTiming echo{6, 10000, 20000};
    EffectTiming lowFps{6, 8000, 15000};
    EffectTiming thermalInversion{6, 10000, 20000};
    EffectTiming melt{6, 10000, 20000};
} g_Config;

static void LoadTiming(const Mad2ConfigApi& api, const char* prefix, EffectTiming& t, const char* comment) {
    char weightKey[64], minKey[64], maxKey[64];
    snprintf(weightKey, sizeof(weightKey), "%sWeight", prefix);
    snprintf(minKey, sizeof(minKey), "%sMinDurationMs", prefix);
    snprintf(maxKey, sizeof(maxKey), "%sMaxDurationMs", prefix);
    t.weight = api.GetInt("GraphicsEffects", weightKey, t.weight, comment);
    t.minDurationMs = static_cast<DWORD>(api.GetInt("GraphicsEffects", minKey, static_cast<int>(t.minDurationMs), ""));
    t.maxDurationMs = static_cast<DWORD>(api.GetInt("GraphicsEffects", maxKey, static_cast<int>(t.maxDurationMs), ""));
}

static bool g_ConfigLoaded = false;

static void EnsureConfigLoaded() {
    if (g_ConfigLoaded) return;
    g_ConfigLoaded = true;

    const auto& api = Mad2Config_Resolve();
    if (!api.GetInt) {
        Log("[Dependency] mad2config.dll not found or missing exports -- using built-in defaults\n");
        return;
    }

    LoadTiming(api, "", g_Config.invertColors, "InvertColors: chance-pool weight, and active duration (ms) range.");
    LoadTiming(api, "Rotate", g_Config.rotate, "Rotate: chance-pool weight/duration.");
    LoadTiming(api, "Zoom", g_Config.zoom, "Zoom: chance-pool weight/duration.");
    LoadTiming(api, "Pulse", g_Config.pulse, "Pulse: chance-pool weight/duration.");
    LoadTiming(api, "EliminateChannel", g_Config.eliminateChannel, "EliminateChannel: chance-pool weight/duration.");
    LoadTiming(api, "Pixelate", g_Config.pixelate, "Pixelate: chance-pool weight/duration.");
    LoadTiming(api, "InvertX", g_Config.invertX, "InvertX: chance-pool weight/duration.");
    LoadTiming(api, "InvertY", g_Config.invertY, "InvertY: chance-pool weight/duration.");
    LoadTiming(api, "SplitChannels", g_Config.splitChannels, "SplitChannels: chance-pool weight/duration.");
    LoadTiming(api, "DeepFry", g_Config.deepFry, "DeepFry: chance-pool weight/duration.");
    LoadTiming(api, "Gameboy", g_Config.gameboy,
               "Gameboy: chance-pool weight/duration. Approximated as a green tint -- see file header.");
    LoadTiming(api, "MixupRgb", g_Config.mixupRgb,
               "MixupRgb: chance-pool weight/duration. Approximated as randomized per-channel tinting -- see "
               "file header.");
    LoadTiming(api, "Vignette", g_Config.vignette, "Vignette: chance-pool weight/duration.");
    LoadTiming(api, "SineDisplace", g_Config.sineDisplace, "SineDisplace: chance-pool weight/duration.");
    LoadTiming(api, "Kaleidoscope", g_Config.kaleidoscope, "Kaleidoscope: chance-pool weight/duration.");
    LoadTiming(api, "Echo", g_Config.echo, "Echo: chance-pool weight/duration.");
    LoadTiming(api, "LowFps", g_Config.lowFps, "LowFps: chance-pool weight/duration. Caps rendering to ~3fps.");
    LoadTiming(api, "ThermalInversion", g_Config.thermalInversion,
               "ThermalInversion: chance-pool weight/duration. Approximated by zeroing 2 of 3 color channels "
               "(cycling which one survives) rather than true grayscale -- see file header.");
    LoadTiming(api, "Melt", g_Config.melt, "Melt: chance-pool weight/duration.");

    Log("Config loaded.\n");
}

// IAT/vtable hooking now provided by aa_mad2hookutil.dll -- see
// ../../mad2hookutil/include/mad2hookutil_api.h. Resolved synchronously
// (not lazily) since aa_mad2hookutil.dll is guaranteed already loaded by
// the time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Ping-pong offscreen textures the pipeline transforms through, lazily
// (re)created to match the backbuffer's current size/format.
// ---------------------------------------------------------------------

struct PingPongTarget {
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9* surf = nullptr;
};

static PingPongTarget g_Ping[2];
static UINT g_PingW = 0, g_PingH = 0;
static D3DFORMAT g_PingFmt = D3DFMT_UNKNOWN;

static void ReleasePingPong() {
    for (auto& p : g_Ping) {
        if (p.surf) {
            p.surf->Release();
            p.surf = nullptr;
        }
        if (p.tex) {
            p.tex->Release();
            p.tex = nullptr;
        }
    }
}

static bool EnsurePingPong(IDirect3DDevice9* dev, const D3DSURFACE_DESC& desc) {
    if (g_Ping[0].tex && desc.Width == g_PingW && desc.Height == g_PingH && desc.Format == g_PingFmt) return true;

    ReleasePingPong();
    for (auto& p : g_Ping) {
        if (FAILED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                       D3DPOOL_DEFAULT, &p.tex, nullptr))) {
            Log("CreateTexture (ping-pong, %ux%u fmt=%d) failed\n", desc.Width, desc.Height, desc.Format);
            ReleasePingPong();
            return false;
        }
        p.tex->GetSurfaceLevel(0, &p.surf);
    }
    g_PingW = desc.Width;
    g_PingH = desc.Height;
    g_PingFmt = desc.Format;
    Log("Ping-pong textures (re)created: %ux%u fmt=%d\n", g_PingW, g_PingH, g_PingFmt);
    return true;
}

// ---------------------------------------------------------------------
// Saved render state -- ONE save/restore wraps the whole per-frame
// pipeline (every active effect's passes run inside it), not one per
// effect, since state changes compose fine within a single frame as long
// as everything is restored at the very end.
// ---------------------------------------------------------------------

struct SavedState {
    DWORD alphaBlend, srcBlend, destBlend, zEnable, alphaTest, alphaFunc, alphaRef, cullMode, lighting, fogEnable;
    DWORD texFactor, colorWrite;
    DWORD colorOp, colorArg1, colorArg2, alphaOp, alphaArg1, alphaArg2;
    DWORD minFilter, magFilter, addressU, addressV;
    DWORD fvf;
    IDirect3DBaseTexture9* texture0 = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DSurface9* renderTarget = nullptr;
};

static void SaveState(IDirect3DDevice9* dev, SavedState& s) {
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &s.alphaBlend);
    dev->GetRenderState(D3DRS_SRCBLEND, &s.srcBlend);
    dev->GetRenderState(D3DRS_DESTBLEND, &s.destBlend);
    dev->GetRenderState(D3DRS_ZENABLE, &s.zEnable);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &s.alphaTest);
    dev->GetRenderState(D3DRS_ALPHAFUNC, &s.alphaFunc);
    dev->GetRenderState(D3DRS_ALPHAREF, &s.alphaRef);
    dev->GetRenderState(D3DRS_CULLMODE, &s.cullMode);
    dev->GetRenderState(D3DRS_LIGHTING, &s.lighting);
    dev->GetRenderState(D3DRS_FOGENABLE, &s.fogEnable);
    dev->GetRenderState(D3DRS_TEXTUREFACTOR, &s.texFactor);
    dev->GetRenderState(D3DRS_COLORWRITEENABLE, &s.colorWrite);
    dev->GetTextureStageState(0, D3DTSS_COLOROP, &s.colorOp);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &s.colorArg1);
    dev->GetTextureStageState(0, D3DTSS_COLORARG2, &s.colorArg2);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &s.alphaOp);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &s.alphaArg1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG2, &s.alphaArg2);
    dev->GetSamplerState(0, D3DSAMP_MINFILTER, &s.minFilter);
    dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &s.magFilter);
    dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &s.addressU);
    dev->GetSamplerState(0, D3DSAMP_ADDRESSV, &s.addressV);
    dev->GetFVF(&s.fvf);
    dev->GetTexture(0, &s.texture0);
    dev->GetVertexShader(&s.vs);
    dev->GetPixelShader(&s.ps);
    dev->GetRenderTarget(0, &s.renderTarget);
}

static void RestoreState(IDirect3DDevice9* dev, SavedState& s) {
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, s.alphaBlend);
    dev->SetRenderState(D3DRS_SRCBLEND, s.srcBlend);
    dev->SetRenderState(D3DRS_DESTBLEND, s.destBlend);
    dev->SetRenderState(D3DRS_ZENABLE, s.zEnable);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, s.alphaTest);
    dev->SetRenderState(D3DRS_ALPHAFUNC, s.alphaFunc);
    dev->SetRenderState(D3DRS_ALPHAREF, s.alphaRef);
    dev->SetRenderState(D3DRS_CULLMODE, s.cullMode);
    dev->SetRenderState(D3DRS_LIGHTING, s.lighting);
    dev->SetRenderState(D3DRS_FOGENABLE, s.fogEnable);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, s.texFactor);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, s.colorWrite);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, s.colorOp);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, s.colorArg1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, s.colorArg2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, s.alphaOp);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, s.alphaArg1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, s.alphaArg2);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, s.minFilter);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, s.magFilter);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, s.addressU);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, s.addressV);
    dev->SetFVF(s.fvf);
    dev->SetTexture(0, s.texture0);
    if (s.texture0) s.texture0->Release();
    dev->SetVertexShader(s.vs);
    if (s.vs) s.vs->Release();
    dev->SetPixelShader(s.ps);
    if (s.ps) s.ps->Release();
    dev->SetRenderTarget(0, s.renderTarget);
    if (s.renderTarget) s.renderTarget->Release();
}

// ---------------------------------------------------------------------
// Draw helpers.
// ---------------------------------------------------------------------

struct TexVertex {
    float x, y, z, rhw;
    float u, v;
};

// Baseline texture-stage setup most effects want: sample the texture
// as-is (no tint/blend), replacing whatever's in the render target.
static void SetPassthroughTextureState(IDirect3DDevice9* dev, IDirect3DTexture9* tex, DWORD filter) {
    dev->SetTexture(0, tex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE,
                         D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE |
                             D3DCOLORWRITEENABLE_ALPHA);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
}

static void DrawQuad(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                      float v1) {
    TexVertex verts[4] = {
        {x0, y0, 0, 1, u0, v0}, {x1, y0, 0, 1, u1, v0}, {x0, y1, 0, 1, u0, v1}, {x1, y1, 0, 1, u1, v1},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(TexVertex));
}

// Draws a quad with an independently-specified UV at each of the 4
// corners (top-left, top-right, bottom-left, bottom-right) rather than a
// simple rect -- used by rotate/zoom/pulse. See ComputeAffineCornerUVs.
static void DrawQuadCornerUV(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, const float uv[4][2]) {
    TexVertex verts[4] = {
        {x0, y0, 0, 1, uv[0][0], uv[0][1]},
        {x1, y0, 0, 1, uv[1][0], uv[1][1]},
        {x0, y1, 0, 1, uv[2][0], uv[2][1]},
        {x1, y1, 0, 1, uv[3][0], uv[3][1]},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(TexVertex));
}

static void DrawTriList(IDirect3DDevice9* dev, const std::vector<TexVertex>& verts) {
    if (verts.size() < 3) return;
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(TexVertex));
}

// Computes each corner's source-sampling UV for an affine transform
// (rotate by angleRad around the image center, then scale by 1/zoomFactor
// -- zoomFactor > 1 samples a smaller centered region, i.e. zooms in). See
// the file header for why this is mathematically exact (not an
// approximation) when drawn as one D3DFVF_XYZRHW quad.
static void ComputeAffineCornerUVs(float angleRad, float zoomFactor, float outUV[4][2]) {
    // Corner logical UVs before transform: TL, TR, BL, BR.
    static const float kCorners[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    float cosA = cosf(-angleRad), sinA = sinf(-angleRad);
    float invZoom = 1.0f / zoomFactor;
    for (int i = 0; i < 4; ++i) {
        float du = kCorners[i][0] - 0.5f, dv = kCorners[i][1] - 0.5f;
        float rdu = du * cosA - dv * sinA;
        float rdv = du * sinA + dv * cosA;
        outUV[i][0] = 0.5f + rdu * invZoom;
        outUV[i][1] = 0.5f + rdv * invZoom;
    }
}

static float RandRangeF(float lo, float hi) { return lo + (hi - lo) * (static_cast<float>(rand()) / RAND_MAX); }

// ---------------------------------------------------------------------
// Effect state. Each effect: a static state struct (if it needs one), an
// atomic active flag, Apply/Clear (called by mad2effects), and a Render
// function called once per frame by the pipeline while active. Render
// receives the source texture (result of the previous stage) and draws
// into whatever render target is currently bound -- the pipeline handles
// SetRenderTarget before calling.
// ---------------------------------------------------------------------

// -- InvertColors (pre-existing, ported into the generalized pipeline) --
static std::atomic<bool> g_InvertColorsActive{false};
static void WINAPI ApplyInvertColors(void*) { g_InvertColorsActive.store(true); }
static void WINAPI ClearInvertColors(void*) { g_InvertColorsActive.store(false); }
static void Render_InvertColors(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    // Effects earlier in this frame's chain (e.g. EliminateChannel,
    // ThermalInversion) may leave D3DRS_COLORWRITEENABLE restricted to a
    // single channel -- every Render_* function that doesn't go through
    // SetPassthroughTextureState (which resets it) must reset it itself,
    // or it'd silently only write one channel here.
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                     D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    dev->SetTexture(0, src);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SUBTRACT);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    // FVF was never set here -- this function drew garbage whenever it ran
    // as the first draw of the pipeline, since it inherited whatever
    // vertex format the game (or nothing at all) last left active instead
    // of the XYZRHW|TEX1 layout DrawQuad's TexVertex actually uses.
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    DrawQuad(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), 0, 0, 1, 1);
}

// -- Rotate --
struct RotateState {
    uint64_t startMs = 0;
    float degPerSec = 0;
    float direction = 1;
};
static RotateState g_RotateState;
static std::atomic<bool> g_RotateActive{false};
static void WINAPI ApplyRotate(void*) {
    g_RotateState.startMs = GetTickCount64();
    g_RotateState.degPerSec = RandRangeF(20.0f, 90.0f);
    g_RotateState.direction = (rand() % 2) ? 1.0f : -1.0f;
    g_RotateActive.store(true);
    Log("Rotate applied (%.1f deg/s, dir=%.0f)\n", g_RotateState.degPerSec, g_RotateState.direction);
}
static void WINAPI ClearRotate(void*) { g_RotateActive.store(false); }
static void Render_Rotate(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t nowMs) {
    float elapsedSec = (nowMs - g_RotateState.startMs) / 1000.0f;
    float angle = g_RotateState.direction * g_RotateState.degPerSec * elapsedSec * (3.14159265f / 180.0f);
    float uv[4][2];
    ComputeAffineCornerUVs(angle, 1.0f, uv);
    SetPassthroughTextureState(dev, src, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    DrawQuadCornerUV(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), uv);
}

// -- Zoom (continuous zoom-in) --
struct ZoomState {
    uint64_t startMs = 0;
    float ratePerSec = 0;
};
static ZoomState g_ZoomState;
static std::atomic<bool> g_ZoomActive{false};
static const float kZoomMax = 4.0f;
static void WINAPI ApplyZoom(void*) {
    g_ZoomState.startMs = GetTickCount64();
    g_ZoomState.ratePerSec = RandRangeF(0.15f, 0.5f);
    g_ZoomActive.store(true);
    Log("Zoom applied (%.2f/s)\n", g_ZoomState.ratePerSec);
}
static void WINAPI ClearZoom(void*) { g_ZoomActive.store(false); }
static void Render_Zoom(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t nowMs) {
    float elapsedSec = (nowMs - g_ZoomState.startMs) / 1000.0f;
    float zoom = 1.0f + g_ZoomState.ratePerSec * elapsedSec;
    if (zoom > kZoomMax) zoom = kZoomMax;
    float uv[4][2];
    ComputeAffineCornerUVs(0.0f, zoom, uv);
    SetPassthroughTextureState(dev, src, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    DrawQuadCornerUV(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), uv);
}

// -- Pulse (zoom oscillating back and forth) --
struct PulseState {
    uint64_t startMs = 0;
    float amplitude = 0, freqHz = 0, phase = 0;
};
static PulseState g_PulseState;
static std::atomic<bool> g_PulseActive{false};
static void WINAPI ApplyPulse(void*) {
    g_PulseState.startMs = GetTickCount64();
    g_PulseState.amplitude = RandRangeF(0.1f, 0.3f);
    g_PulseState.freqHz = RandRangeF(0.3f, 1.0f);
    g_PulseState.phase = RandRangeF(0.0f, 6.28318f);
    g_PulseActive.store(true);
    Log("Pulse applied (amp=%.2f freq=%.2fHz)\n", g_PulseState.amplitude, g_PulseState.freqHz);
}
static void WINAPI ClearPulse(void*) { g_PulseActive.store(false); }
static void Render_Pulse(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t nowMs) {
    float elapsedSec = (nowMs - g_PulseState.startMs) / 1000.0f;
    float zoom = 1.0f + g_PulseState.amplitude *
                             sinf(g_PulseState.phase + elapsedSec * g_PulseState.freqHz * 6.28318f);
    if (zoom < 0.1f) zoom = 0.1f;
    float uv[4][2];
    ComputeAffineCornerUVs(0.0f, zoom, uv);
    SetPassthroughTextureState(dev, src, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    DrawQuadCornerUV(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), uv);
}

// -- EliminateChannel --
struct EliminateChannelState {
    DWORD writeMask = D3DCOLORWRITEENABLE_BLUE;
};
static EliminateChannelState g_EliminateChannelState;
static std::atomic<bool> g_EliminateChannelActive{false};
static void WINAPI ApplyEliminateChannel(void*) {
    static const DWORD kMasks[3] = {D3DCOLORWRITEENABLE_RED, D3DCOLORWRITEENABLE_GREEN, D3DCOLORWRITEENABLE_BLUE};
    g_EliminateChannelState.writeMask = kMasks[rand() % 3];
    g_EliminateChannelActive.store(true);
    Log("EliminateChannel applied (mask=0x%X)\n", g_EliminateChannelState.writeMask);
}
static void WINAPI ClearEliminateChannel(void*) { g_EliminateChannelActive.store(false); }
static void Render_EliminateChannel(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    // Pass the source through unchanged everywhere except the eliminated
    // channel, which gets overwritten with a solid black quad.
    SetPassthroughTextureState(dev, src, D3DTEXF_POINT);
    DrawQuad(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), 0, 0, 1, 1);

    dev->SetRenderState(D3DRS_COLORWRITEENABLE, g_EliminateChannelState.writeMask);
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF000000);
    dev->SetFVF(D3DFVF_XYZRHW);
    struct ColorVertex {
        float x, y, z, rhw;
        DWORD color;
    };
    ColorVertex verts[4] = {
        {0, 0, 0, 1, 0xFF000000},
        {static_cast<float>(vpW), 0, 0, 1, 0xFF000000},
        {0, static_cast<float>(vpH), 0, 1, 0xFF000000},
        {static_cast<float>(vpW), static_cast<float>(vpH), 0, 1, 0xFF000000},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ColorVertex));
}

// -- Pixelate --
static std::atomic<bool> g_PixelateActive{false};
static const int kPixelateDownscale = 12;
static IDirect3DTexture9* g_PixelateTex = nullptr;
static IDirect3DSurface9* g_PixelateSurf = nullptr;
static int g_PixelateW = 0, g_PixelateH = 0;
static void WINAPI ApplyPixelate(void*) { g_PixelateActive.store(true); }
static void WINAPI ClearPixelate(void*) { g_PixelateActive.store(false); }
static void Render_Pixelate(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    int smallW = vpW / kPixelateDownscale, smallH = vpH / kPixelateDownscale;
    if (smallW < 1) smallW = 1;
    if (smallH < 1) smallH = 1;
    if (!g_PixelateTex || smallW != g_PixelateW || smallH != g_PixelateH) {
        if (g_PixelateSurf) {
            g_PixelateSurf->Release();
            g_PixelateSurf = nullptr;
        }
        if (g_PixelateTex) {
            g_PixelateTex->Release();
            g_PixelateTex = nullptr;
        }
        if (FAILED(dev->CreateTexture(smallW, smallH, 1, D3DUSAGE_RENDERTARGET, g_PingFmt, D3DPOOL_DEFAULT,
                                       &g_PixelateTex, nullptr))) {
            return;
        }
        g_PixelateTex->GetSurfaceLevel(0, &g_PixelateSurf);
        g_PixelateW = smallW;
        g_PixelateH = smallH;
    }

    IDirect3DSurface9* srcSurf = nullptr;
    src->GetSurfaceLevel(0, &srcSurf);
    dev->StretchRect(srcSurf, nullptr, g_PixelateSurf, nullptr, D3DTEXF_LINEAR);
    srcSurf->Release();

    SetPassthroughTextureState(dev, g_PixelateTex, D3DTEXF_POINT);
    DrawQuad(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), 0, 0, 1, 1);
}

// -- InvertX / InvertY (mirror) --
static std::atomic<bool> g_InvertXActive{false};
static std::atomic<bool> g_InvertYActive{false};
static void WINAPI ApplyInvertX(void*) { g_InvertXActive.store(true); }
static void WINAPI ClearInvertX(void*) { g_InvertXActive.store(false); }
static void WINAPI ApplyInvertY(void*) { g_InvertYActive.store(true); }
static void WINAPI ClearInvertY(void*) { g_InvertYActive.store(false); }
static void Render_InvertX(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    SetPassthroughTextureState(dev, src, D3DTEXF_LINEAR);
    DrawQuad(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), 1, 0, 0, 1);
}
static void Render_InvertY(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    SetPassthroughTextureState(dev, src, D3DTEXF_LINEAR);
    DrawQuad(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), 0, 1, 1, 0);
}

// -- SplitChannels (chromatic aberration) --
struct SplitChannelsState {
    float offR[2], offG[2], offB[2];  // pixel offsets (x,y) per channel
};
static SplitChannelsState g_SplitState;
static std::atomic<bool> g_SplitChannelsActive{false};
// A uniform [-max,max] offset too often lands near zero for all three
// channels at once (barely visible aberration) -- guarantee a minimum
// magnitude instead, random sign.
static float RandOffsetPx(float minMag, float maxMag) {
    float mag = RandRangeF(minMag, maxMag);
    return (rand() % 2) ? mag : -mag;
}
static void WINAPI ApplySplitChannels(void*) {
    for (int c = 0; c < 2; ++c) {
        g_SplitState.offR[c] = RandOffsetPx(12.0f, 30.0f);
        g_SplitState.offG[c] = RandOffsetPx(12.0f, 30.0f);
        g_SplitState.offB[c] = RandOffsetPx(12.0f, 30.0f);
    }
    g_SplitChannelsActive.store(true);
    Log("SplitChannels applied\n");
}
static void WINAPI ClearSplitChannels(void*) { g_SplitChannelsActive.store(false); }
static void Render_SplitChannels(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    float w = static_cast<float>(vpW), h = static_cast<float>(vpH);
    struct ChannelPass {
        DWORD mask;
        float dx, dy;
    };
    ChannelPass passes[3] = {
        {D3DCOLORWRITEENABLE_RED, g_SplitState.offR[0], g_SplitState.offR[1]},
        {D3DCOLORWRITEENABLE_GREEN, g_SplitState.offG[0], g_SplitState.offG[1]},
        {D3DCOLORWRITEENABLE_BLUE, g_SplitState.offB[0], g_SplitState.offB[1]},
    };
    for (const auto& p : passes) {
        SetPassthroughTextureState(dev, src, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, p.mask | D3DCOLORWRITEENABLE_ALPHA);
        // Shift the SAMPLE point (not the quad) by the offset in UV space
        // so this channel's content appears displaced on screen.
        float du = p.dx / w, dv = p.dy / h;
        DrawQuad(dev, 0, 0, w, h, du, dv, 1.0f + du, 1.0f + dv);
    }
}

// -- DeepFry --
static std::atomic<bool> g_DeepFryActive{false};
static void WINAPI ApplyDeepFry(void*) { g_DeepFryActive.store(true); }
static void WINAPI ClearDeepFry(void*) { g_DeepFryActive.store(false); }
static void Render_DeepFry(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    float w = static_cast<float>(vpW), h = static_cast<float>(vpH);
    // Base pass: draw the source once (SetPassthroughTextureState resets
    // D3DRS_COLORWRITEENABLE to full -- see Render_InvertColors's comment
    // for why that matters when effects chain).
    SetPassthroughTextureState(dev, src, D3DTEXF_POINT);
    DrawQuad(dev, 0, 0, w, h, 0, 0, 1, 1);
    // A single additive pass at ~45% strength boosts contrast/brightness
    // (a crude "fried" look) without blowing every non-dark pixel straight
    // to white the way two full-strength (SRCBLEND=ONE/DESTBLEND=ONE)
    // passes would -- per-vertex diffuse alpha controls the blend strength
    // here, same technique as Render_Echo's accumulation blend.
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    struct AlphaTexVertex {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    DWORD boostAlpha = 115u << 24;  // ~45% of 255
    AlphaTexVertex boostVerts[4] = {
        {0, 0, 0, 1, boostAlpha, 0, 0},
        {w, 0, 0, 1, boostAlpha, 1, 0},
        {0, h, 0, 1, boostAlpha, 0, 1},
        {w, h, 0, 1, boostAlpha, 1, 1},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, boostVerts, sizeof(AlphaTexVertex));
    // Warm orange/red tint via a multiply pass.
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFF8020);
    dev->SetFVF(D3DFVF_XYZRHW);
    struct ColorVertex {
        float x, y, z, rhw;
        DWORD color;
    };
    ColorVertex verts[4] = {{0, 0, 0, 1, 0xFFFF8020},
                             {w, 0, 0, 1, 0xFFFF8020},
                             {0, h, 0, 1, 0xFFFF8020},
                             {w, h, 0, 1, 0xFFFF8020}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ColorVertex));
}

// -- Gameboy (green-tint approximation -- see file header) --
static std::atomic<bool> g_GameboyActive{false};
static void WINAPI ApplyGameboy(void*) { g_GameboyActive.store(true); }
static void WINAPI ClearGameboy(void*) { g_GameboyActive.store(false); }
static void Render_Gameboy(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    dev->SetTexture(0, src);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF9BBC0F);  // classic GB screen green
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                     D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    DrawQuad(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), 0, 0, 1, 1);
}

// -- MixupRgb (randomized per-channel tint -- see file header) --
struct MixupRgbState {
    float factorR = 1, factorG = 1, factorB = 1;
};
static MixupRgbState g_MixupState;
static std::atomic<bool> g_MixupRgbActive{false};
static void WINAPI ApplyMixupRgb(void*) {
    // Force one channel near-zero and one near-full so the cast always
    // reads as an obvious color-mangling rather than a subtle dimming --
    // three independently random [0.2,1.0] factors too often land close
    // enough together (e.g. 0.6/0.65/0.55) to barely register.
    float factors[3] = {RandRangeF(0.0f, 0.15f), RandRangeF(0.85f, 1.0f), RandRangeF(0.3f, 0.7f)};
    for (int i = 2; i > 0; --i) std::swap(factors[i], factors[rand() % (i + 1)]);
    g_MixupState.factorR = factors[0];
    g_MixupState.factorG = factors[1];
    g_MixupState.factorB = factors[2];
    g_MixupRgbActive.store(true);
    Log("MixupRgb applied (r=%.2f g=%.2f b=%.2f)\n", g_MixupState.factorR, g_MixupState.factorG,
        g_MixupState.factorB);
}
static void WINAPI ClearMixupRgb(void*) { g_MixupRgbActive.store(false); }
static void Render_MixupRgb(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    dev->SetTexture(0, src);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    DWORD tfactor = 0xFF000000 | (static_cast<DWORD>(g_MixupState.factorR * 255) << 16) |
                    (static_cast<DWORD>(g_MixupState.factorG * 255) << 8) |
                    static_cast<DWORD>(g_MixupState.factorB * 255);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, tfactor);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                     D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    DrawQuad(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), 0, 0, 1, 1);
}

// -- Vignette --
static IDirect3DTexture9* g_VignetteTex = nullptr;
static std::atomic<bool> g_VignetteActive{false};
static void WINAPI ApplyVignette(void*) { g_VignetteActive.store(true); }
static void WINAPI ClearVignette(void*) { g_VignetteActive.store(false); }
static bool EnsureVignetteTexture(IDirect3DDevice9* dev) {
    if (g_VignetteTex) return true;
    const int kSize = 256;
    if (FAILED(dev->CreateTexture(kSize, kSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_VignetteTex, nullptr))) {
        return false;
    }
    D3DLOCKED_RECT locked;
    if (FAILED(g_VignetteTex->LockRect(0, &locked, nullptr, 0))) {
        g_VignetteTex->Release();
        g_VignetteTex = nullptr;
        return false;
    }
    float center = (kSize - 1) / 2.0f;
    float maxDist = center * 0.92f;
    for (int y = 0; y < kSize; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(locked.pBits) + y * locked.Pitch);
        for (int x = 0; x < kSize; ++x) {
            float dx = x - center, dy = y - center;
            float dist = sqrtf(dx * dx + dy * dy) / maxDist;
            if (dist > 1.0f) dist = 1.0f;
            // Aggressive falloff -- an exponent BELOW 1 makes brightness
            // drop off fast starting close to center (dist^0.6 rises
            // quickly for small dist), unlike a >1 exponent which would
            // stay bright until right at the edge.
            float bright = 1.0f - powf(dist, 0.6f);
            if (bright < 0.0f) bright = 0.0f;
            uint8_t v = static_cast<uint8_t>(bright * 255);
            row[x] = 0xFF000000 | (v << 16) | (v << 8) | v;
        }
    }
    g_VignetteTex->UnlockRect(0);
    Log("Vignette texture baked (%dx%d)\n", kSize, kSize);
    return true;
}
static void Render_Vignette(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    float w = static_cast<float>(vpW), h = static_cast<float>(vpH);
    // Pass the source through first (this effect only darkens, it doesn't
    // otherwise transform the frame).
    SetPassthroughTextureState(dev, src, D3DTEXF_POINT);
    DrawQuad(dev, 0, 0, w, h, 0, 0, 1, 1);

    if (!EnsureVignetteTexture(dev)) return;
    dev->SetTexture(0, g_VignetteTex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    // Multiply blend against whatever's already in the render target:
    // dest = dest * src.
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    DrawQuad(dev, 0, 0, w, h, 0, 0, 1, 1);
}

// -- SineDisplace --
struct SineDisplaceState {
    uint64_t startMs = 0;
    float amplitudePx = 0, wavelengthPx = 0, speed = 0;
};
static SineDisplaceState g_SineState;
static std::atomic<bool> g_SineDisplaceActive{false};
static void WINAPI ApplySineDisplace(void*) {
    g_SineState.startMs = GetTickCount64();
    g_SineState.amplitudePx = RandRangeF(10.0f, 35.0f);
    g_SineState.wavelengthPx = RandRangeF(60.0f, 200.0f);
    g_SineState.speed = RandRangeF(1.0f, 4.0f);
    g_SineDisplaceActive.store(true);
    Log("SineDisplace applied (amp=%.1f wavelen=%.1f speed=%.1f)\n", g_SineState.amplitudePx,
        g_SineState.wavelengthPx, g_SineState.speed);
}
static void WINAPI ClearSineDisplace(void*) { g_SineDisplaceActive.store(false); }
static void Render_SineDisplace(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t nowMs) {
    const int kStrips = 120;
    float elapsedSec = (nowMs - g_SineState.startMs) / 1000.0f;
    float w = static_cast<float>(vpW), h = static_cast<float>(vpH);
    float stripH = h / kStrips;

    std::vector<TexVertex> verts;
    verts.reserve(kStrips * 6);
    for (int i = 0; i < kStrips; ++i) {
        float y0 = i * stripH, y1 = (i + 1) * stripH;
        float rowCenter = (y0 + y1) * 0.5f;
        float phase = (rowCenter / g_SineState.wavelengthPx) * 6.28318f + elapsedSec * g_SineState.speed;
        float uOffset = (g_SineState.amplitudePx * sinf(phase)) / w;
        float v0 = y0 / h, v1 = y1 / h;
        float u0 = 0.0f + uOffset, u1 = 1.0f + uOffset;
        TexVertex a{0, y0, 0, 1, u0, v0}, b{w, y0, 0, 1, u1, v0}, c{0, y1, 0, 1, u0, v1}, d{w, y1, 0, 1, u1, v1};
        verts.push_back(a);
        verts.push_back(b);
        verts.push_back(c);
        verts.push_back(b);
        verts.push_back(d);
        verts.push_back(c);
    }

    SetPassthroughTextureState(dev, src, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_MIRROR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    DrawTriList(dev, verts);
}

// -- Melt --
struct MeltState {
    uint64_t startMs = 0;
    std::vector<float> columnRatePerSec;
};
static MeltState g_MeltState;
static std::atomic<bool> g_MeltActive{false};
static const int kMeltColumns = 160;
static void WINAPI ApplyMelt(void*) {
    g_MeltState.startMs = GetTickCount64();
    g_MeltState.columnRatePerSec.resize(kMeltColumns);
    for (auto& r : g_MeltState.columnRatePerSec) r = RandRangeF(0.03f, 0.22f);
    g_MeltActive.store(true);
    Log("Melt applied\n");
}
static void WINAPI ClearMelt(void*) { g_MeltActive.store(false); }
static void Render_Melt(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t nowMs) {
    float elapsedSec = (nowMs - g_MeltState.startMs) / 1000.0f;
    float w = static_cast<float>(vpW), h = static_cast<float>(vpH);
    float colW = w / kMeltColumns;

    std::vector<TexVertex> verts;
    verts.reserve(kMeltColumns * 6);
    for (int i = 0; i < kMeltColumns; ++i) {
        float x0 = i * colW, x1 = (i + 1) * colW;
        float vOffset = g_MeltState.columnRatePerSec[i] * elapsedSec;  // grows without bound -- WRAP addressing below
        float u0 = x0 / w, u1 = x1 / w;
        float v0 = 0.0f + vOffset, v1 = 1.0f + vOffset;
        TexVertex a{x0, 0, 0, 1, u0, v0}, b{x1, 0, 0, 1, u1, v0}, c{x0, h, 0, 1, u0, v1}, d{x1, h, 0, 1, u1, v1};
        verts.push_back(a);
        verts.push_back(b);
        verts.push_back(c);
        verts.push_back(b);
        verts.push_back(d);
        verts.push_back(c);
    }

    SetPassthroughTextureState(dev, src, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    DrawTriList(dev, verts);
}

// -- Kaleidoscope --
struct KaleidoscopeState {
    uint64_t startMs = 0;
};
static KaleidoscopeState g_KaleidoscopeState;
static std::atomic<bool> g_KaleidoscopeActive{false};
static const int kKaleidoscopeSegments = 8;
static const int kKaleidoscopeSubdiv = 6;  // triangles per segment, for a smoother curved sample region
static const float kKaleidoscopeRotSpeed = 0.15f;  // rad/sec, slow constant spin
static void WINAPI ApplyKaleidoscope(void*) {
    g_KaleidoscopeState.startMs = GetTickCount64();
    g_KaleidoscopeActive.store(true);
    Log("Kaleidoscope applied\n");
}
static void WINAPI ClearKaleidoscope(void*) { g_KaleidoscopeActive.store(false); }
static void Render_Kaleidoscope(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t nowMs) {
    float elapsedSec = (nowMs - g_KaleidoscopeState.startMs) / 1000.0f;
    float cx = vpW * 0.5f, cy = vpH * 0.5f;
    float maxRadius = sqrtf(cx * cx + cy * cy);
    float segAngle = 6.28318f / kKaleidoscopeSegments;
    float spin = elapsedSec * kKaleidoscopeRotSpeed;

    std::vector<TexVertex> verts;
    verts.reserve(kKaleidoscopeSegments * kKaleidoscopeSubdiv * 3);

    for (int seg = 0; seg < kKaleidoscopeSegments; ++seg) {
        bool mirrored = (seg % 2) != 0;
        for (int sub = 0; sub < kKaleidoscopeSubdiv; ++sub) {
            float a0 = seg * segAngle + sub * (segAngle / kKaleidoscopeSubdiv) + spin;
            float a1 = seg * segAngle + (sub + 1) * (segAngle / kKaleidoscopeSubdiv) + spin;

            // Triangle fan from center: center vertex + two outer edge
            // vertices. Sample UV computed by mapping each vertex's angle
            // (relative to its segment's start) into the base wedge
            // [0, segAngle), mirrored on odd segments -- this is what
            // produces the repeating mirror-symmetric kaleidoscope look.
            auto sampleAt = [&](float angle, float radius) -> TexVertex {
                float screenX = cx + cosf(angle) * radius;
                float screenY = cy + sinf(angle) * radius;

                float rel = fmodf(angle - spin - seg * segAngle, segAngle);
                if (rel < 0) rel += segAngle;
                if (mirrored) rel = segAngle - rel;

                float sampleX = cx + cosf(rel) * radius;
                float sampleY = cy + sinf(rel) * radius;
                float u = sampleX / vpW, v = sampleY / vpH;
                if (u < 0) u = 0;
                if (u > 1) u = 1;
                if (v < 0) v = 0;
                if (v > 1) v = 1;
                return TexVertex{screenX, screenY, 0, 1, u, v};
            };

            TexVertex centerV{cx, cy, 0, 1, 0.5f, 0.5f};
            verts.push_back(centerV);
            verts.push_back(sampleAt(a0, maxRadius));
            verts.push_back(sampleAt(a1, maxRadius));
        }
    }

    SetPassthroughTextureState(dev, src, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    DrawTriList(dev, verts);
}

// -- Echo (blend previous frames -- ghosting/motion blur) --
static IDirect3DTexture9* g_EchoTex = nullptr;
static IDirect3DSurface9* g_EchoSurf = nullptr;
static UINT g_EchoW = 0, g_EchoH = 0;
static std::atomic<bool> g_EchoActive{false};
static const float kEchoDecay = 0.14f;  // fraction of the new frame blended in each tick
static void WINAPI ApplyEcho(void*) { g_EchoActive.store(true); }

// Releases the echo accumulation texture/surface without touching
// g_EchoActive -- shared by ClearEcho (effect ending) and the device
// Reset() hook (device resources invalidated, effect may still be active
// and should just re-seed its accumulation buffer on the next frame).
static void ReleaseEchoTargets() {
    if (g_EchoSurf) {
        g_EchoSurf->Release();
        g_EchoSurf = nullptr;
    }
    if (g_EchoTex) {
        g_EchoTex->Release();
        g_EchoTex = nullptr;
    }
    g_EchoW = g_EchoH = 0;
}
static void WINAPI ClearEcho(void*) {
    g_EchoActive.store(false);
    ReleaseEchoTargets();
}
static void Render_Echo(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t) {
    float w = static_cast<float>(vpW), h = static_cast<float>(vpH);

    if (!g_EchoTex || static_cast<UINT>(vpW) != g_EchoW || static_cast<UINT>(vpH) != g_EchoH) {
        ReleaseEchoTargets();
        if (FAILED(dev->CreateTexture(vpW, vpH, 1, D3DUSAGE_RENDERTARGET, g_PingFmt, D3DPOOL_DEFAULT, &g_EchoTex,
                                       nullptr))) {
            return;
        }
        g_EchoTex->GetSurfaceLevel(0, &g_EchoSurf);
        g_EchoW = vpW;
        g_EchoH = vpH;
        // Seed the accumulation buffer from the current frame so the first
        // tick doesn't flash from a blank/undefined history.
        IDirect3DSurface9* srcSurf = nullptr;
        src->GetSurfaceLevel(0, &srcSurf);
        dev->StretchRect(srcSurf, nullptr, g_EchoSurf, nullptr, D3DTEXF_NONE);
        srcSurf->Release();
    }

    // The pipeline already pointed the render target at this stage's real
    // destination before calling us; remember it, since blending the new
    // frame into the accumulation buffer below needs to temporarily
    // redirect the render target to g_EchoSurf instead.
    IDirect3DSurface9* pipelineDst = nullptr;
    dev->GetRenderTarget(0, &pipelineDst);

    // Blend the new frame into the accumulation buffer: accum = accum*(1-decay) + new*decay.
    // Uses per-vertex diffuse alpha (SRCALPHA/INVSRCALPHA) rather than a
    // device-level D3DRS_BLENDFACTOR constant -- the latter needs a less
    // universally-supported D3D9 blend capability; SRCALPHA/INVSRCALPHA is
    // the same proven blend mode every overlay mod in this repo already
    // relies on.
    dev->SetRenderTarget(0, g_EchoSurf);
    // A prior effect in this frame's chain may have left
    // D3DRS_COLORWRITEENABLE restricted to one channel -- reset it before
    // this pass, same reasoning as Render_InvertColors's comment.
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                     D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    dev->SetTexture(0, src);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    struct AlphaTexVertex {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    DWORD decayAlpha = static_cast<DWORD>(kEchoDecay * 255) << 24;
    AlphaTexVertex verts[4] = {
        {0, 0, 0, 1, decayAlpha, 0, 0},
        {w, 0, 0, 1, decayAlpha, 1, 0},
        {0, h, 0, 1, decayAlpha, 0, 1},
        {w, h, 0, 1, decayAlpha, 1, 1},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(AlphaTexVertex));

    // Draw the (now updated) accumulation buffer as this stage's actual
    // output, back on the pipeline's real destination.
    dev->SetRenderTarget(0, pipelineDst);
    if (pipelineDst) pipelineDst->Release();
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    SetPassthroughTextureState(dev, g_EchoTex, D3DTEXF_LINEAR);
    DrawQuad(dev, 0, 0, w, h, 0, 0, 1, 1);
}

// -- LowFps (cap capture/redraw to ~3fps) --
static IDirect3DTexture9* g_LowFpsTex = nullptr;
static IDirect3DSurface9* g_LowFpsSurf = nullptr;
static UINT g_LowFpsW = 0, g_LowFpsH = 0;
static uint64_t g_LowFpsLastCaptureMs = 0;
static std::atomic<bool> g_LowFpsActive{false};
static const int kLowFpsIntervalMs = 333;  // ~3fps
static void WINAPI ApplyLowFps(void*) {
    g_LowFpsLastCaptureMs = 0;  // force an immediate capture on first frame
    g_LowFpsActive.store(true);
}
// Releases the low-fps capture texture/surface without touching
// g_LowFpsActive -- shared by ClearLowFps and the device Reset() hook, same
// reasoning as ReleaseEchoTargets above.
static void ReleaseLowFpsTargets() {
    if (g_LowFpsSurf) {
        g_LowFpsSurf->Release();
        g_LowFpsSurf = nullptr;
    }
    if (g_LowFpsTex) {
        g_LowFpsTex->Release();
        g_LowFpsTex = nullptr;
    }
    g_LowFpsW = g_LowFpsH = 0;
    g_LowFpsLastCaptureMs = 0;
}
static void WINAPI ClearLowFps(void*) {
    g_LowFpsActive.store(false);
    ReleaseLowFpsTargets();
}
static void Render_LowFps(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t nowMs) {
    if (!g_LowFpsTex || static_cast<UINT>(vpW) != g_LowFpsW || static_cast<UINT>(vpH) != g_LowFpsH) {
        ReleaseLowFpsTargets();
        if (FAILED(dev->CreateTexture(vpW, vpH, 1, D3DUSAGE_RENDERTARGET, g_PingFmt, D3DPOOL_DEFAULT, &g_LowFpsTex,
                                       nullptr))) {
            return;
        }
        g_LowFpsTex->GetSurfaceLevel(0, &g_LowFpsSurf);
        g_LowFpsW = vpW;
        g_LowFpsH = vpH;
        g_LowFpsLastCaptureMs = 0;
    }

    if (nowMs - g_LowFpsLastCaptureMs >= static_cast<uint64_t>(kLowFpsIntervalMs)) {
        IDirect3DSurface9* srcSurf = nullptr;
        src->GetSurfaceLevel(0, &srcSurf);
        dev->StretchRect(srcSurf, nullptr, g_LowFpsSurf, nullptr, D3DTEXF_NONE);
        srcSurf->Release();
        g_LowFpsLastCaptureMs = nowMs;
    }

    SetPassthroughTextureState(dev, g_LowFpsTex, D3DTEXF_POINT);
    DrawQuad(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), 0, 0, 1, 1);
}

// -- ThermalInversion (zero 2 of 3 channels, cycling which survives -- see file header) --
static std::atomic<bool> g_ThermalInversionActive{false};
static uint64_t g_ThermalInversionStartMs = 0;
static const int kThermalCyclePeriodMs = 3000;
static void WINAPI ApplyThermalInversion(void*) {
    g_ThermalInversionStartMs = GetTickCount64();
    g_ThermalInversionActive.store(true);
}
static void WINAPI ClearThermalInversion(void*) { g_ThermalInversionActive.store(false); }
static void Render_ThermalInversion(IDirect3DDevice9* dev, IDirect3DTexture9* src, int vpW, int vpH, uint64_t nowMs) {
    static const DWORD kChannelMasks[3] = {D3DCOLORWRITEENABLE_RED, D3DCOLORWRITEENABLE_GREEN,
                                            D3DCOLORWRITEENABLE_BLUE};
    int surviving = static_cast<int>(((nowMs - g_ThermalInversionStartMs) / kThermalCyclePeriodMs) % 3);

    SetPassthroughTextureState(dev, src, D3DTEXF_POINT);
    DrawQuad(dev, 0, 0, static_cast<float>(vpW), static_cast<float>(vpH), 0, 0, 1, 1);

    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF000000);
    dev->SetFVF(D3DFVF_XYZRHW);
    struct ColorVertex {
        float x, y, z, rhw;
        DWORD color;
    };
    float w = static_cast<float>(vpW), h = static_cast<float>(vpH);
    ColorVertex verts[4] = {{0, 0, 0, 1, 0xFF000000}, {w, 0, 0, 1, 0xFF000000}, {0, h, 0, 1, 0xFF000000},
                             {w, h, 0, 1, 0xFF000000}};
    for (int ch = 0; ch < 3; ++ch) {
        if (ch == surviving) continue;
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, kChannelMasks[ch]);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ColorVertex));
    }
}

// ---------------------------------------------------------------------
// Pipeline orchestration.
// ---------------------------------------------------------------------

typedef void (*RenderFn)(IDirect3DDevice9*, IDirect3DTexture9*, int, int, uint64_t);

struct PipelineEffect {
    const char* name;
    std::atomic<bool>* active;
    RenderFn render;
};

// Registration order also fixes the pipeline's chaining order when
// multiple effects are simultaneously active -- not semantically load-
// bearing (each stage just transforms whatever the previous one produced),
// just deterministic.
static PipelineEffect g_Pipeline[] = {
    {"InvertColors", &g_InvertColorsActive, Render_InvertColors},
    {"Rotate", &g_RotateActive, Render_Rotate},
    {"Zoom", &g_ZoomActive, Render_Zoom},
    {"Pulse", &g_PulseActive, Render_Pulse},
    {"EliminateChannel", &g_EliminateChannelActive, Render_EliminateChannel},
    {"Pixelate", &g_PixelateActive, Render_Pixelate},
    {"InvertX", &g_InvertXActive, Render_InvertX},
    {"InvertY", &g_InvertYActive, Render_InvertY},
    {"SplitChannels", &g_SplitChannelsActive, Render_SplitChannels},
    {"DeepFry", &g_DeepFryActive, Render_DeepFry},
    {"Gameboy", &g_GameboyActive, Render_Gameboy},
    {"MixupRgb", &g_MixupRgbActive, Render_MixupRgb},
    {"Vignette", &g_VignetteActive, Render_Vignette},
    {"SineDisplace", &g_SineDisplaceActive, Render_SineDisplace},
    {"Kaleidoscope", &g_KaleidoscopeActive, Render_Kaleidoscope},
    {"Echo", &g_EchoActive, Render_Echo},
    {"LowFps", &g_LowFpsActive, Render_LowFps},
    {"ThermalInversion", &g_ThermalInversionActive, Render_ThermalInversion},
    {"Melt", &g_MeltActive, Render_Melt},
};

static bool g_EffectsRegistered = false;

static void RegisterEffect(const char* name, const char* displayName, const EffectTiming& t,
                            Mad2Effects_ApplyFn apply, Mad2Effects_ClearFn clear) {
    Mad2EffectDesc desc{};
    desc.name = name;
    desc.displayName = displayName;
    desc.defaultWeight = t.weight;
    desc.apply = apply;
    desc.clear = clear;
    desc.defaultMinDurationMs = t.minDurationMs;
    desc.defaultMaxDurationMs = t.maxDurationMs;
    BOOL ok = Mad2Effects_Resolve().Register(&desc);
    Log("Mad2Effects_Register(\"%s\") -> %d\n", name, ok);
}

static void EnsureEffectsRegistered() {
    if (g_EffectsRegistered) return;
    const auto& api = Mad2Effects_Resolve();
    if (!api.Register) return;  // retry next frame

    RegisterEffect("InvertColors", "Inverted Colors", g_Config.invertColors, ApplyInvertColors, ClearInvertColors);
    RegisterEffect("Rotate", "Screen Rotation", g_Config.rotate, ApplyRotate, ClearRotate);
    RegisterEffect("Zoom", "Screen Zoom", g_Config.zoom, ApplyZoom, ClearZoom);
    RegisterEffect("Pulse", "Screen Pulse", g_Config.pulse, ApplyPulse, ClearPulse);
    RegisterEffect("EliminateChannel", "Missing Color Channel", g_Config.eliminateChannel, ApplyEliminateChannel,
                    ClearEliminateChannel);
    RegisterEffect("Pixelate", "Pixelated", g_Config.pixelate, ApplyPixelate, ClearPixelate);
    RegisterEffect("InvertX", "Mirrored Horizontally", g_Config.invertX, ApplyInvertX, ClearInvertX);
    RegisterEffect("InvertY", "Mirrored Vertically", g_Config.invertY, ApplyInvertY, ClearInvertY);
    RegisterEffect("SplitChannels", "Chromatic Aberration", g_Config.splitChannels, ApplySplitChannels,
                    ClearSplitChannels);
    RegisterEffect("DeepFry", "Deep Fried", g_Config.deepFry, ApplyDeepFry, ClearDeepFry);
    RegisterEffect("Gameboy", "Gameboy Screen", g_Config.gameboy, ApplyGameboy, ClearGameboy);
    RegisterEffect("MixupRgb", "Mixed Up Colors", g_Config.mixupRgb, ApplyMixupRgb, ClearMixupRgb);
    RegisterEffect("Vignette", "Aggressive Vignette", g_Config.vignette, ApplyVignette, ClearVignette);
    RegisterEffect("SineDisplace", "Wavy Screen", g_Config.sineDisplace, ApplySineDisplace, ClearSineDisplace);
    RegisterEffect("Kaleidoscope", "Kaleidoscope", g_Config.kaleidoscope, ApplyKaleidoscope, ClearKaleidoscope);
    RegisterEffect("Echo", "Screen Echo", g_Config.echo, ApplyEcho, ClearEcho);
    RegisterEffect("LowFps", "Low Framerate", g_Config.lowFps, ApplyLowFps, ClearLowFps);
    RegisterEffect("ThermalInversion", "Thermal Inversion", g_Config.thermalInversion, ApplyThermalInversion,
                    ClearThermalInversion);
    RegisterEffect("Melt", "Screen Melt", g_Config.melt, ApplyMelt, ClearMelt);

    g_EffectsRegistered = true;
}

// ---------------------------------------------------------------------
// Direct3DCreate9 IAT hook -> IDirect3D9::CreateDevice vtable patch ->
// IDirect3DDevice9::EndScene vtable patch. Identical technique to
// mad2igttimer/mad2inputdisplay -- see their file headers for why vtable
// slots 16/42 are correct and why this composes with other D3D9-hooking
// mods regardless of mad2/mods/ load order.
// ---------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    EnsureConfigLoaded();
    EnsureEffectsRegistered();

    std::vector<const PipelineEffect*> active;
    for (const auto& e : g_Pipeline) {
        if (e.active->load()) active.push_back(&e);
    }

    if (!active.empty()) {
        IDirect3DSurface9* backSurf = nullptr;
        if (SUCCEEDED(This->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backSurf)) && backSurf) {
            D3DSURFACE_DESC desc;
            backSurf->GetDesc(&desc);
            D3DVIEWPORT9 vp{};
            This->GetViewport(&vp);
            int vpW = static_cast<int>(vp.Width), vpH = static_cast<int>(vp.Height);
            uint64_t nowMs = GetTickCount64();

            if (EnsurePingPong(This, desc)) {
                SavedState saved{};
                SaveState(This, saved);

                This->StretchRect(backSurf, nullptr, g_Ping[0].surf, nullptr, D3DTEXF_NONE);

                int srcIdx = 0;
                for (size_t i = 0; i < active.size(); ++i) {
                    bool isLast = (i + 1 == active.size());
                    IDirect3DSurface9* dstSurf = isLast ? backSurf : g_Ping[1 - srcIdx].surf;
                    This->SetRenderTarget(0, dstSurf);
                    active[i]->render(This, g_Ping[srcIdx].tex, vpW, vpH, nowMs);
                    if (!isLast) srcIdx = 1 - srcIdx;
                }

                RestoreState(This, saved);
            }
            backSurf->Release();
        }
    }

    return RealEndScene(This);
}

// IDirect3DDevice9::Reset (vtable slot 16 -- a different table/interface
// than IDirect3D9::CreateDevice below, which just happens to share the same
// slot number). Every D3DPOOL_DEFAULT resource this mod owns (ping-pong,
// echo, low-fps render targets) MUST be released before the real Reset()
// call and can only be safely recreated lazily afterward -- the pipeline's
// existing dimension-change checks in EnsurePingPong/Render_Echo/
// Render_LowFps already do that recreation, they just need these globals
// nulled out first so those checks fire again on the next frame. Not doing
// this at all was the likely cause of D3DERR_DEVICENOTRESET on alt-tab/
// resolution-change while a render-target-backed effect was active --
// Reset() legitimately fails if the application hasn't released every
// D3DPOOL_DEFAULT resource it's still holding.
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static Reset_t RealReset = nullptr;

static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    ReleasePingPong();
    ReleaseEchoTargets();
    ReleaseLowFpsTargets();
    Log("IDirect3DDevice9::Reset intercepted -- released ping-pong/echo/low-fps D3DPOOL_DEFAULT render "
        "targets before chaining through\n");

    if (!RealReset) {
        Log("ERROR: RealReset is null\n");
        return D3DERR_INVALIDCALL;
    }
    HRESULT hr = RealReset(This, pPresentationParameters);
    if (FAILED(hr)) {
        Log("Real Reset() failed: 0x%08lX\n", static_cast<unsigned long>(hr));
    }
    return hr;
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

        void* prevReset = nullptr;
        Mad2HookUtil_PatchVTableSlot(*ppReturnedDeviceInterface, 16, reinterpret_cast<void*>(HookReset), &prevReset);
        RealReset = reinterpret_cast<Reset_t>(prevReset);
        Log("Hooked IDirect3DDevice9::Reset (vtable[16], device=%p)\n", (void*)*ppReturnedDeviceInterface);
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
            srand(static_cast<unsigned>(time(nullptr)) ^ GetCurrentProcessId());
            InstallHooks();
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
