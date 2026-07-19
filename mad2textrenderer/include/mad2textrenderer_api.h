#pragma once
// Public client interface for mad2textrenderer.dll (mad2/mods/mad2textrenderer.dll).
//
// Shared GDI mask/outline text-to-texture rasterizer + D3D9 state save/
// restore + textured-quad drawing, factored out of 5 mods that each
// carried their own near-identical copy: mad2igttimer, mad2playercoords,
// mad2inputdisplay, mad2twitchchat, mad2effectshud. See each mod's
// original RenderOverlayTexture/DrawStrokedLines(-Runs)/SaveState/
// RestoreState/DrawOverlayQuad for the technique this generalizes --
// mad2igttimer's was the most-commented version.
//
// Two real feature differences existed across the 5 originals, both
// preserved here rather than flattened to a lowest common denominator:
//   - mad2twitchchat needs per-run (not just per-surface) fill color, for
//     username/message/system-message coloring within one texture --
//     Mad2TextRun carries its own fillColor, and its own (x, y) position,
//     rather than being a single auto-laid-out line.
//   - mad2effectshud needs its texture resized every frame to fit however
//     many effects are currently active -- Mad2TextRenderer_EnsureSize is
//     a cheap no-op when the requested size matches the current one, so
//     it's safe to call unconditionally every frame (that's exactly what
//     the dynamic-resize consumers do); fixed-size consumers just call it
//     once.
// mad2graphicseffectmod does NOT use GDI text at all -- not a consumer.
//
// Resolved lazily (e.g. the first EndScene call, same as mad2xinput
// consumers already do) -- no load-order concern, unlike aa_mad2hookutil,
// since nothing here runs synchronously from another mod's DllMain.
#include <windows.h>
#include <d3d9.h>

#define MAD2TEXTRENDERER_DLL_NAME "mad2textrenderer.dll"

// Opaque handle for one texture-backed text rendering surface. A mod
// creates one per distinct overlay it draws (every consumer today only
// ever needs one).
typedef void* Mad2TextSurface;

// One run of text to bake into a surface's texture: position (top-left,
// texture-local pixel coordinates), the text itself, and this run's own
// fill color (outline is always black, matching every existing
// consumer's convention -- see Mad2TextRenderer_Render_t).
struct Mad2TextRun {
    int x, y;
    const char* text;
    COLORREF fillColor;
};

// The small subset of D3D9 render/texture-stage/sampler state every
// consumer's own quad-drawing touches -- saved before, restored after, so
// drawing a HUD element doesn't perturb whatever state the engine (or an
// earlier mod's overlay this frame) left active. A concrete POD struct
// (not a truly opaque blob) so it can be stack-allocated by the caller
// with no additional allocation API.
struct Mad2TextRendererSavedState {
    DWORD alphaBlend, srcBlend, destBlend, zEnable, alphaTest, cullMode, lighting, fogEnable;
    DWORD colorOp, colorArg1, colorArg2, alphaOp, alphaArg1, alphaArg2;
    DWORD minFilter, magFilter, fvf;
    IDirect3DBaseTexture9* texture0;
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
};

// Creates a new rendering surface using a bold "Consolas" font at
// fontSize (matching every existing consumer's font choice). Returns
// nullptr on failure (e.g. GDI resource exhaustion).
typedef Mad2TextSurface(WINAPI* Mad2TextRenderer_CreateSurface_t)(int fontSize);

// Destroys a surface and all of its GDI/D3D9 resources (safe to call with
// nullptr).
typedef void(WINAPI* Mad2TextRenderer_DestroySurface_t)(Mad2TextSurface surface);

// (Re)creates the surface's backing D3D9 texture at exactly width x
// height if it isn't already that size; a cheap no-op otherwise, so
// dynamic-size consumers (mad2effectshud) can call this unconditionally
// every frame. Must be called (at least once, with a size big enough for
// whatever Render will draw) before Render/DrawQuad. Returns FALSE on
// failure (e.g. CreateTexture failed) -- surface keeps its previous size
// in that case.
typedef BOOL(WINAPI* Mad2TextRenderer_EnsureSize_t)(Mad2TextSurface surface, IDirect3DDevice9* dev, int width,
                                                     int height);

// Renders `runs` (runCount entries) into the surface's current texture via
// the mask-pass + color-pass GDI outline-text technique: an all-white
// mask pass gives true per-pixel glyph+outline coverage (used as alpha),
// composited with a black-outline/per-run-fill-color pass (used as RGB) --
// this is what makes the text read correctly antialiased against any
// background, which a single direct-color GDI draw cannot do (a black
// outline pixel can't self-report its own alpha). Overwrites the whole
// texture (any area not covered by a run is left transparent).
typedef void(WINAPI* Mad2TextRenderer_Render_t)(Mad2TextSurface surface, const Mad2TextRun* runs, int runCount);

// Draws the surface's entire current texture as one alpha-blended,
// pixel-exact (no scaling) screen-space quad at (x, y). Saves/restores
// state around the draw internally (see Mad2TextRendererSavedState).
typedef void(WINAPI* Mad2TextRenderer_DrawQuad_t)(Mad2TextSurface surface, IDirect3DDevice9* dev, float x, float y);

// Current texture size (e.g. for a consumer's own on-screen positioning
// math, like bottom/right-anchored overlays) and this surface's font's
// single-line height (extent.cy + 6, matching every existing consumer's
// padding convention).
typedef void(WINAPI* Mad2TextRenderer_GetSize_t)(Mad2TextSurface surface, int* outWidth, int* outHeight);
typedef int(WINAPI* Mad2TextRenderer_GetLineHeight_t)(Mad2TextSurface surface);

// Raw D3D9 texture backing this surface (not AddRef'd -- valid only until
// the next EnsureSize/DestroySurface call). For a consumer that needs to
// draw the surface's texture alongside OTHER textures under one shared
// SaveState/RestoreState block (e.g. mad2twitchchat drawing its chat text
// plus several inline emote quads in a single batch) rather than going
// through DrawQuad's own self-contained save+draw+restore.
typedef IDirect3DTexture9*(WINAPI* Mad2TextRenderer_GetTexture_t)(Mad2TextSurface surface);

// Measures the pixel width `text` would render at in this surface's font
// -- every consumer needs this before it can decide what width/positions
// to pass to EnsureSize/Render.
typedef int(WINAPI* Mad2TextRenderer_MeasureTextWidth_t)(Mad2TextSurface surface, const char* text);

// General-purpose D3D9 state save/restore + raw textured-quad draw, for
// any consumer that draws its own textures/quads outside of a text
// surface (e.g. mad2inputdisplay's button-icon quads, mad2twitchchat's
// inline emote quads) -- the same save/restore/draw shape every consumer
// used to inline for its own DrawOverlayQuad, generalized to take an
// arbitrary texture, UV rect, and per-draw diffuse tint (COLOROP/ALPHAOP
// MODULATE against D3DTA_DIFFUSE, not just SELECTARG1 -- a strict
// generalization: tint 0xFFFFFFFF is visually identical to plain
// texture-only sampling, while mad2inputdisplay also needs a dimmed tint
// for its "not currently pressed" button state).
typedef void(WINAPI* Mad2TextRenderer_SaveState_t)(IDirect3DDevice9* dev, Mad2TextRendererSavedState* outState);
typedef void(WINAPI* Mad2TextRenderer_RestoreState_t)(IDirect3DDevice9* dev, Mad2TextRendererSavedState* state);
typedef void(WINAPI* Mad2TextRenderer_DrawTexturedQuad_t)(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float x,
                                                           float y, float w, float h, float u0, float v0, float u1,
                                                           float v1, DWORD tint);

#define MAD2TEXTRENDERER_CREATESURFACE_PROC "Mad2TextRenderer_CreateSurface"
#define MAD2TEXTRENDERER_DESTROYSURFACE_PROC "Mad2TextRenderer_DestroySurface"
#define MAD2TEXTRENDERER_ENSURESIZE_PROC "Mad2TextRenderer_EnsureSize"
#define MAD2TEXTRENDERER_RENDER_PROC "Mad2TextRenderer_Render"
#define MAD2TEXTRENDERER_DRAWQUAD_PROC "Mad2TextRenderer_DrawQuad"
#define MAD2TEXTRENDERER_GETSIZE_PROC "Mad2TextRenderer_GetSize"
#define MAD2TEXTRENDERER_GETLINEHEIGHT_PROC "Mad2TextRenderer_GetLineHeight"
#define MAD2TEXTRENDERER_GETTEXTURE_PROC "Mad2TextRenderer_GetTexture"
#define MAD2TEXTRENDERER_MEASURETEXTWIDTH_PROC "Mad2TextRenderer_MeasureTextWidth"
#define MAD2TEXTRENDERER_SAVESTATE_PROC "Mad2TextRenderer_SaveState"
#define MAD2TEXTRENDERER_RESTORESTATE_PROC "Mad2TextRenderer_RestoreState"
#define MAD2TEXTRENDERER_DRAWTEXTUREDQUAD_PROC "Mad2TextRenderer_DrawTexturedQuad"

struct Mad2TextRendererApi {
    Mad2TextRenderer_CreateSurface_t CreateSurface;
    Mad2TextRenderer_DestroySurface_t DestroySurface;
    Mad2TextRenderer_EnsureSize_t EnsureSize;
    Mad2TextRenderer_Render_t Render;
    Mad2TextRenderer_DrawQuad_t DrawQuad;
    Mad2TextRenderer_GetSize_t GetSize;
    Mad2TextRenderer_GetLineHeight_t GetLineHeight;
    Mad2TextRenderer_GetTexture_t GetTexture;
    Mad2TextRenderer_MeasureTextWidth_t MeasureTextWidth;
    Mad2TextRenderer_SaveState_t SaveState;
    Mad2TextRenderer_RestoreState_t RestoreState;
    Mad2TextRenderer_DrawTexturedQuad_t DrawTexturedQuad;
};

// Resolves mad2textrenderer.dll's exports by name. Cheap to call every
// frame (resolves once, then returns the cached result). If
// mad2textrenderer.dll isn't loaded, every member is null -- callers must
// null-check before use, same convention as every other shared-API mod.
inline const Mad2TextRendererApi& Mad2TextRenderer_Resolve() {
    static Mad2TextRendererApi api{};
    static bool resolved = false;
    if (!resolved) {
        if (HMODULE h = GetModuleHandleA(MAD2TEXTRENDERER_DLL_NAME)) {
            api.CreateSurface = reinterpret_cast<Mad2TextRenderer_CreateSurface_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_CREATESURFACE_PROC)));
            api.DestroySurface = reinterpret_cast<Mad2TextRenderer_DestroySurface_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_DESTROYSURFACE_PROC)));
            api.EnsureSize = reinterpret_cast<Mad2TextRenderer_EnsureSize_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_ENSURESIZE_PROC)));
            api.Render = reinterpret_cast<Mad2TextRenderer_Render_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_RENDER_PROC)));
            api.DrawQuad = reinterpret_cast<Mad2TextRenderer_DrawQuad_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_DRAWQUAD_PROC)));
            api.GetSize = reinterpret_cast<Mad2TextRenderer_GetSize_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_GETSIZE_PROC)));
            api.GetLineHeight = reinterpret_cast<Mad2TextRenderer_GetLineHeight_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_GETLINEHEIGHT_PROC)));
            api.GetTexture = reinterpret_cast<Mad2TextRenderer_GetTexture_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_GETTEXTURE_PROC)));
            api.MeasureTextWidth = reinterpret_cast<Mad2TextRenderer_MeasureTextWidth_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_MEASURETEXTWIDTH_PROC)));
            api.SaveState = reinterpret_cast<Mad2TextRenderer_SaveState_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_SAVESTATE_PROC)));
            api.RestoreState = reinterpret_cast<Mad2TextRenderer_RestoreState_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_RESTORESTATE_PROC)));
            api.DrawTexturedQuad = reinterpret_cast<Mad2TextRenderer_DrawTexturedQuad_t>(
                reinterpret_cast<void*>(GetProcAddress(h, MAD2TEXTRENDERER_DRAWTEXTUREDQUAD_PROC)));
            resolved = api.CreateSurface != nullptr;
        }
    }
    return api;
}
