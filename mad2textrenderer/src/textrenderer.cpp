// Shared GDI text-to-texture rasterizer + D3D9 state save/restore + quad
// drawing for Madagascar 2 (mad2/mods/mad2textrenderer.dll).
//
// See include/mad2textrenderer_api.h for the full client contract. This is
// the canonical, generalized implementation of what used to be 5 mods'
// worth of near-identical GDI mask/outline text rendering
// (mad2igttimer/mad2playercoords/mad2inputdisplay's fixed-size single-line
// lists, mad2effectshud's dynamically-resized list, mad2twitchchat's
// per-run-colored/positioned runs) plus the D3D9 save-state/quad-draw
// boilerplate every one of them also inlined separately.
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../include/mad2textrenderer_api.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2textrenderer]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Surface: one GDI DIB + D3D9 texture pair, plus the font it was created
// with. Consumers hold this only as an opaque Mad2TextSurface handle.
// ---------------------------------------------------------------------

namespace {

static const int kPadding = 6;
static const int kOutlineOffset = 2;

struct Surface {
    HDC memDC = nullptr;
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    HFONT font = nullptr;
    int lineHeight = 0;

    int texW = 0, texH = 0;
    IDirect3DTexture9* texture = nullptr;
    std::vector<uint32_t> colorBuf;  // scratch buffer for the color pass, see Render()
};

void ReleaseTexture(Surface* s) {
    if (s->texture) {
        s->texture->Release();
        s->texture = nullptr;
    }
    if (s->bitmap) {
        DeleteObject(s->bitmap);
        s->bitmap = nullptr;
        s->bits = nullptr;
    }
    s->texW = s->texH = 0;
}

}  // namespace

extern "C" __declspec(dllexport) Mad2TextSurface WINAPI Mad2TextRenderer_CreateSurface(int fontSize) {
    Surface* s = new Surface();

    HDC screenDC = GetDC(nullptr);
    s->memDC = CreateCompatibleDC(screenDC);
    ReleaseDC(nullptr, screenDC);

    s->font = CreateFontA(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SelectObject(s->memDC, s->font);
    SetBkMode(s->memDC, TRANSPARENT);

    SIZE extent{};
    GetTextExtentPoint32A(s->memDC, "Mg", 2, &extent);
    s->lineHeight = extent.cy + 6;

    Log("Surface created (fontSize=%d, lineHeight=%d)\n", fontSize, s->lineHeight);
    return s;
}

extern "C" __declspec(dllexport) void WINAPI Mad2TextRenderer_DestroySurface(Mad2TextSurface surface) {
    Surface* s = reinterpret_cast<Surface*>(surface);
    if (!s) return;
    ReleaseTexture(s);
    if (s->font) DeleteObject(s->font);
    if (s->memDC) DeleteDC(s->memDC);
    delete s;
}

extern "C" __declspec(dllexport) BOOL WINAPI Mad2TextRenderer_EnsureSize(Mad2TextSurface surface,
                                                                          IDirect3DDevice9* dev, int width,
                                                                          int height) {
    Surface* s = reinterpret_cast<Surface*>(surface);
    if (!s || !dev) return FALSE;
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;
    if (s->texture && s->texW == width && s->texH == height) return TRUE;

    ReleaseTexture(s);

    HRESULT hr = dev->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s->texture, nullptr);
    if (FAILED(hr)) {
        Log("CreateTexture (%dx%d) failed: hr=0x%08lX\n", width, height, static_cast<unsigned long>(hr));
        return FALSE;
    }
    s->texW = width;
    s->texH = height;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = s->texW;
    bmi.bmiHeader.biHeight = -s->texH;  // top-down DIB, matches D3D texture row order
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s->bitmap = CreateDIBSection(s->memDC, &bmi, DIB_RGB_COLORS, &s->bits, nullptr, 0);
    SelectObject(s->memDC, s->bitmap);
    SelectObject(s->memDC, s->font);  // re-select: SelectObject(bitmap) can reset the DC's font slot
    SetBkMode(s->memDC, TRANSPARENT);
    s->colorBuf.resize(static_cast<size_t>(s->texW) * s->texH);

    Log("Texture (re)created: %dx%d\n", s->texW, s->texH);
    return TRUE;
}

namespace {

// Draws every run once, stroked (4 offset copies + a center fill), all in
// one color per pass -- called once for the all-white "mask" pass (true
// glyph+outline coverage) and once for the "color" pass (black outline,
// each run's own fill color).
void DrawStrokedRuns(Surface* s, const Mad2TextRun* runs, int runCount, bool maskPass) {
    static const int offsets[4][2] = {{-kOutlineOffset, 0}, {kOutlineOffset, 0}, {0, -kOutlineOffset},
                                       {0, kOutlineOffset}};
    for (int i = 0; i < runCount; ++i) {
        const Mad2TextRun& run = runs[i];
        RECT base{run.x, run.y, s->texW, run.y + s->lineHeight};
        COLORREF fill = maskPass ? RGB(255, 255, 255) : run.fillColor;
        COLORREF outline = maskPass ? RGB(255, 255, 255) : RGB(0, 0, 0);

        SetTextColor(s->memDC, outline);
        for (auto& off : offsets) {
            RECT r = base;
            OffsetRect(&r, off[0], off[1]);
            DrawTextA(s->memDC, run.text, -1, &r, DT_NOCLIP | DT_SINGLELINE);
        }
        SetTextColor(s->memDC, fill);
        DrawTextA(s->memDC, run.text, -1, &base, DT_NOCLIP | DT_SINGLELINE);
    }
}

}  // namespace

extern "C" __declspec(dllexport) void WINAPI Mad2TextRenderer_Render(Mad2TextSurface surface,
                                                                       const Mad2TextRun* runs, int runCount) {
    Surface* s = reinterpret_cast<Surface*>(surface);
    if (!s || !s->texture || !s->bits) return;

    size_t pixelCount = static_cast<size_t>(s->texW) * s->texH;

    memset(s->bits, 0, pixelCount * 4);
    DrawStrokedRuns(s, runs, runCount, /*maskPass=*/false);
    GdiFlush();
    memcpy(s->colorBuf.data(), s->bits, pixelCount * 4);

    memset(s->bits, 0, pixelCount * 4);
    DrawStrokedRuns(s, runs, runCount, /*maskPass=*/true);
    GdiFlush();

    uint32_t* mask = reinterpret_cast<uint32_t*>(s->bits);
    for (size_t i = 0; i < pixelCount; ++i) {
        uint32_t maskPixel = mask[i];
        uint8_t a = std::max({static_cast<uint8_t>(maskPixel & 0xFF), static_cast<uint8_t>((maskPixel >> 8) & 0xFF),
                               static_cast<uint8_t>((maskPixel >> 16) & 0xFF)});
        uint32_t colorPixel = s->colorBuf[i] & 0x00FFFFFF;
        mask[i] = (static_cast<uint32_t>(a) << 24) | colorPixel;
    }

    D3DLOCKED_RECT locked;
    if (SUCCEEDED(s->texture->LockRect(0, &locked, nullptr, 0))) {
        for (int y = 0; y < s->texH; ++y) {
            memcpy(reinterpret_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch,
                   reinterpret_cast<uint8_t*>(s->bits) + static_cast<size_t>(y) * s->texW * 4,
                   static_cast<size_t>(s->texW) * 4);
        }
        s->texture->UnlockRect(0);
    }
}

extern "C" __declspec(dllexport) void WINAPI Mad2TextRenderer_GetSize(Mad2TextSurface surface, int* outWidth,
                                                                        int* outHeight) {
    Surface* s = reinterpret_cast<Surface*>(surface);
    if (outWidth) *outWidth = s ? s->texW : 0;
    if (outHeight) *outHeight = s ? s->texH : 0;
}

extern "C" __declspec(dllexport) int WINAPI Mad2TextRenderer_GetLineHeight(Mad2TextSurface surface) {
    Surface* s = reinterpret_cast<Surface*>(surface);
    return s ? s->lineHeight : 0;
}

extern "C" __declspec(dllexport) IDirect3DTexture9* WINAPI Mad2TextRenderer_GetTexture(Mad2TextSurface surface) {
    Surface* s = reinterpret_cast<Surface*>(surface);
    return s ? s->texture : nullptr;
}

extern "C" __declspec(dllexport) int WINAPI Mad2TextRenderer_MeasureTextWidth(Mad2TextSurface surface,
                                                                                const char* text) {
    Surface* s = reinterpret_cast<Surface*>(surface);
    if (!s || !text) return 0;
    SIZE extent{};
    GetTextExtentPoint32A(s->memDC, text, static_cast<int>(strlen(text)), &extent);
    return extent.cx;
}

// ---------------------------------------------------------------------
// D3D9 state save/restore + textured quad draw -- only the render/
// texture-stage/sampler state every consumer's own overlay draw touches
// (not a full D3DSBT_ALL state block, since this runs every frame and
// per-call overhead in this hook chain measurably affects frame time --
// same reasoning shadowfix's own comments give).
// ---------------------------------------------------------------------

extern "C" __declspec(dllexport) void WINAPI Mad2TextRenderer_SaveState(IDirect3DDevice9* dev,
                                                                          Mad2TextRendererSavedState* out) {
    if (!dev || !out) return;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &out->alphaBlend);
    dev->GetRenderState(D3DRS_SRCBLEND, &out->srcBlend);
    dev->GetRenderState(D3DRS_DESTBLEND, &out->destBlend);
    dev->GetRenderState(D3DRS_ZENABLE, &out->zEnable);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &out->alphaTest);
    dev->GetRenderState(D3DRS_CULLMODE, &out->cullMode);
    dev->GetRenderState(D3DRS_LIGHTING, &out->lighting);
    dev->GetRenderState(D3DRS_FOGENABLE, &out->fogEnable);
    dev->GetTextureStageState(0, D3DTSS_COLOROP, &out->colorOp);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &out->colorArg1);
    dev->GetTextureStageState(0, D3DTSS_COLORARG2, &out->colorArg2);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &out->alphaOp);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &out->alphaArg1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG2, &out->alphaArg2);
    dev->GetSamplerState(0, D3DSAMP_MINFILTER, &out->minFilter);
    dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &out->magFilter);
    dev->GetFVF(&out->fvf);
    dev->GetTexture(0, &out->texture0);
    dev->GetVertexShader(&out->vs);
    dev->GetPixelShader(&out->ps);
}

extern "C" __declspec(dllexport) void WINAPI Mad2TextRenderer_RestoreState(IDirect3DDevice9* dev,
                                                                             Mad2TextRendererSavedState* s) {
    if (!dev || !s) return;
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, s->alphaBlend);
    dev->SetRenderState(D3DRS_SRCBLEND, s->srcBlend);
    dev->SetRenderState(D3DRS_DESTBLEND, s->destBlend);
    dev->SetRenderState(D3DRS_ZENABLE, s->zEnable);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, s->alphaTest);
    dev->SetRenderState(D3DRS_CULLMODE, s->cullMode);
    dev->SetRenderState(D3DRS_LIGHTING, s->lighting);
    dev->SetRenderState(D3DRS_FOGENABLE, s->fogEnable);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, s->colorOp);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, s->colorArg1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, s->colorArg2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, s->alphaOp);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, s->alphaArg1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, s->alphaArg2);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, s->minFilter);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, s->magFilter);
    dev->SetFVF(s->fvf);
    dev->SetTexture(0, s->texture0);
    if (s->texture0) s->texture0->Release();  // GetTexture/GetXxxShader AddRef
    dev->SetVertexShader(s->vs);
    if (s->vs) s->vs->Release();
    dev->SetPixelShader(s->ps);
    if (s->ps) s->ps->Release();
}

namespace {
struct ScreenVertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
}  // namespace

extern "C" __declspec(dllexport) void WINAPI Mad2TextRenderer_DrawTexturedQuad(IDirect3DDevice9* dev,
                                                                                 IDirect3DTexture9* tex, float x,
                                                                                 float y, float w, float h, float u0,
                                                                                 float v0, float u1, float v1,
                                                                                 DWORD tint) {
    if (!dev) return;

    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetTexture(0, tex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    // MODULATE against the vertex diffuse color (not a plain SELECTARG1
    // texture sample) -- a strict generalization: tint 0xFFFFFFFF is
    // visually identical to texture-only sampling, while a dimmer tint
    // (e.g. mad2inputdisplay's "not currently pressed" button state) also
    // works through this same single code path.
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);

    ScreenVertex verts[4] = {
        {x, y, 0, 1, tint, u0, v0},
        {x + w, y, 0, 1, tint, u1, v0},
        {x, y + h, 0, 1, tint, u0, v1},
        {x + w, y + h, 0, 1, tint, u1, v1},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ScreenVertex));
}

extern "C" __declspec(dllexport) void WINAPI Mad2TextRenderer_DrawQuad(Mad2TextSurface surface, IDirect3DDevice9* dev,
                                                                         float x, float y) {
    Surface* s = reinterpret_cast<Surface*>(surface);
    if (!s || !s->texture || !dev) return;

    Mad2TextRendererSavedState saved{};
    Mad2TextRenderer_SaveState(dev, &saved);
    Mad2TextRenderer_DrawTexturedQuad(dev, s->texture, x, y, static_cast<float>(s->texW), static_cast<float>(s->texH),
                                       0, 0, 1, 1, 0xFFFFFFFF);
    Mad2TextRenderer_RestoreState(dev, &saved);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            Log("mad2textrenderer loaded\n");
            break;
        default:
            break;
    }
    return TRUE;
}
