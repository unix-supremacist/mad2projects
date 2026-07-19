// Controller input display overlay for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Ports the Xbox-layout
// portion of ../mad2mod/src/main.cpp's controller HUD (dpad, start,
// A/B/X/Y, LB) into an in-process D3D9 overlay, using icons sourced from
// the Kenney Input Prompts pack (see tools/gen_embedded_assets.py and
// src/embedded_assets.h). The original also drew LS/RS as numeric text;
// here they're drawn as little stick-position indicators instead (filled
// dot inside a box outline) so this mod doesn't need to duplicate
// mad2igttimer's whole GDI-text-to-texture pipeline just for two numbers.
// RB/LT/RT/Back and LS/RS-click badges are available too (see [InputDisplay]
// in config.cfg) but default off, matching the original fixed icon set's
// visuals until explicitly opted into.
//
// Controller state comes from mad2xinput (mad2/mods/mad2xinput.dll) via
// its exported API rather than polling XInputGetState directly, so this
// mod doesn't need its own XInput hook -- see
// ../../mad2xinput/include/mad2xinput_api.h. Settings come from
// mad2config.dll (mad2/mods/mad2config.dll) the same way -- see
// ../../mad2config/include/mad2config_api.h.
//
// Hooking strategy: identical to mad2igttimer -- Direct3DCreate9/Ex
// hooked at the IAT level, then IDirect3D9::CreateDevice and
// IDirect3DDevice9::EndScene vtable-patched (slots 16/42). See
// mad2igttimer/src/igttimer.cpp's file header for why this composes with
// mad2shadowfix (and now mad2igttimer, and any other D3D9-hooking mod)
// regardless of mad2/mods/ load order.
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <xinput.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2xinput/include/mad2xinput_api.h"
#include "embedded_assets.h"

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2textrenderer/include/mad2textrenderer_api.h"

// ---------------------------------------------------------------------
// Logging -- writes into the shared logs\mad2.log (see mad2sharedlog.h),
// tagged "[mad2inputdisplay]" so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2inputdisplay]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [InputDisplay] section, resolved lazily on the
// first EndScene call -- see mad2config/include/mad2config_api.h for why
// this can't happen from DllMain: mad2/mods/*.dll load order isn't
// guaranteed, so mad2config.dll might not be loaded yet this early.)
// ---------------------------------------------------------------------

struct Config {
    DWORD userIndex = 0;  // which controller (XInput user index) to display
    int marginX = 20;
    int marginY = 20;  // from the bottom-left corner, matching mad2mod's HUD placement
    // Green direction-dot position, relative to the dpad icon's geometric
    // center. Measured (bias-corrected for bounding-box discretization,
    // see DrawHud) from screenshots pressing Up and pressing Down, which
    // agreed exactly: true dot center sits 2px right/down of the icon's
    // true center before the +-10 per-direction offset is applied.
    float dpadOffsetX = 2.0f;
    float dpadOffsetY = 2.0f;
    bool debugDpadCrosshair = false;  // draws a fixed red crosshair at the dpad's true geometric center

    // Original fixed icon set -- default on, matching the mod's behavior
    // before per-icon toggles existed.
    bool showDpad = true;
    bool showStart = true;
    bool showA = true;
    bool showB = true;
    bool showX = true;
    bool showY = true;
    bool showLB = true;

    // Expanded icon set -- default off, so visuals don't change until
    // explicitly opted into.
    bool showRB = false;
    bool showLT = false;
    bool showRT = false;
    bool showBack = false;
    bool showLSClick = false;
    bool showRSClick = false;
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

    g_Config.userIndex = static_cast<DWORD>(api.GetInt("InputDisplay", "UserIndex",
                                                         static_cast<int>(g_Config.userIndex),
                                                         "Which controller (XInput user index, 0-3) to display."));
    g_Config.marginX = api.GetInt("InputDisplay", "MarginX", g_Config.marginX,
                                   "Horizontal margin (pixels) from the left edge of the screen.");
    g_Config.marginY = api.GetInt("InputDisplay", "MarginY", g_Config.marginY,
                                   "Vertical margin (pixels) from the bottom edge of the screen.");
    g_Config.dpadOffsetX = api.GetFloat("InputDisplay", "DpadOffsetX", g_Config.dpadOffsetX,
                                         "Fine X-offset (pixels) correction for the dpad's pressed-direction dot.");
    g_Config.dpadOffsetY = api.GetFloat("InputDisplay", "DpadOffsetY", g_Config.dpadOffsetY,
                                         "Fine Y-offset (pixels) correction for the dpad's pressed-direction dot.");
    g_Config.debugDpadCrosshair =
        api.GetBool("InputDisplay", "DebugDpadCrosshair", FALSE,
                     "Draw a fixed red crosshair at the dpad icon's true geometric center, for tuning "
                     "DpadOffsetX/DpadOffsetY.") != FALSE;

    g_Config.showDpad = api.GetBool("InputDisplay", "ShowDpad", TRUE, "Show the dpad icon.") != FALSE;
    g_Config.showStart = api.GetBool("InputDisplay", "ShowStart", TRUE, "Show the Start button icon.") != FALSE;
    g_Config.showA = api.GetBool("InputDisplay", "ShowA", TRUE, "Show the A button icon.") != FALSE;
    g_Config.showB = api.GetBool("InputDisplay", "ShowB", TRUE, "Show the B button icon.") != FALSE;
    g_Config.showX = api.GetBool("InputDisplay", "ShowX", TRUE, "Show the X button icon.") != FALSE;
    g_Config.showY = api.GetBool("InputDisplay", "ShowY", TRUE, "Show the Y button icon.") != FALSE;
    g_Config.showLB = api.GetBool("InputDisplay", "ShowLB", TRUE, "Show the left shoulder (LB) icon.") != FALSE;

    g_Config.showRB = api.GetBool("InputDisplay", "ShowRB", FALSE, "Show the right shoulder (RB) icon.") != FALSE;
    g_Config.showLT = api.GetBool("InputDisplay", "ShowLT", FALSE, "Show the left trigger (LT) icon.") != FALSE;
    g_Config.showRT = api.GetBool("InputDisplay", "ShowRT", FALSE, "Show the right trigger (RT) icon.") != FALSE;
    g_Config.showBack = api.GetBool("InputDisplay", "ShowBack", FALSE, "Show the Back button icon.") != FALSE;
    g_Config.showLSClick = api.GetBool("InputDisplay", "ShowLSClick", FALSE,
                                        "Show a small badge when the left stick is clicked in (L3).") != FALSE;
    g_Config.showRSClick = api.GetBool("InputDisplay", "ShowRSClick", FALSE,
                                        "Show a small badge when the right stick is clicked in (R3).") != FALSE;

    Log("Config loaded: userIndex=%lu marginX=%d marginY=%d dpadOffset=(%.2f,%.2f) debugDpadCrosshair=%d "
        "show[dpad=%d start=%d a=%d b=%d x=%d y=%d lb=%d rb=%d lt=%d rt=%d back=%d lsClick=%d rsClick=%d]\n",
        g_Config.userIndex, g_Config.marginX, g_Config.marginY, g_Config.dpadOffsetX, g_Config.dpadOffsetY,
        g_Config.debugDpadCrosshair, g_Config.showDpad, g_Config.showStart, g_Config.showA, g_Config.showB,
        g_Config.showX, g_Config.showY, g_Config.showLB, g_Config.showRB, g_Config.showLT, g_Config.showRT,
        g_Config.showBack, g_Config.showLSClick, g_Config.showRSClick);
}

// IAT/vtable hooking now provided by aa_mad2hookutil.dll -- see
// ../../mad2hookutil/include/mad2hookutil_api.h. Resolved synchronously
// (not lazily) since aa_mad2hookutil.dll is guaranteed already loaded by
// the time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Embedded BMP -> D3D9 texture.
//
// The embedded assets are 32bpp BI_RGB .bmp files with real per-pixel
// alpha packed into the nominally-unused 4th byte (confirmed by
// inspection -- corner pixels are (0,0,0,0), opaque interior pixels are
// (b,g,r,255)). BMP pixel byte order is B,G,R,A, which is exactly
// D3DFMT_A8R8G8B8's in-memory byte order on a little-endian target, so
// each row can be copied verbatim; only the bottom-up row order needs
// flipping to match D3D's top-down texture layout. No D3DX dependency
// (per mad2igttimer's precedent, D3DX isn't reliably present under
// Proton/Wine) and no padding handling (all embedded assets are 64px
// wide at 32bpp, i.e. already 4-byte-aligned rows).
// ---------------------------------------------------------------------

static IDirect3DTexture9* CreateTextureFromBmp(IDirect3DDevice9* dev, const uint8_t* data, size_t len) {
    if (len < 54 || data[0] != 'B' || data[1] != 'M') return nullptr;
    uint32_t dataOffset;
    int32_t width, height;
    uint16_t bpp;
    memcpy(&dataOffset, data + 10, 4);
    memcpy(&width, data + 18, 4);
    memcpy(&height, data + 22, 4);
    memcpy(&bpp, data + 28, 2);
    if (bpp != 32 || height <= 0 || width <= 0) return nullptr;
    size_t rowBytes = static_cast<size_t>(width) * 4;
    if (dataOffset + rowBytes * static_cast<size_t>(height) > len) return nullptr;

    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
        return nullptr;
    }
    D3DLOCKED_RECT locked;
    if (FAILED(tex->LockRect(0, &locked, nullptr, 0))) {
        tex->Release();
        return nullptr;
    }
    const uint8_t* pixels = data + dataOffset;
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = pixels + static_cast<size_t>(height - 1 - y) * rowBytes;
        uint8_t* dstRow = reinterpret_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
        memcpy(dstRow, srcRow, rowBytes);
    }
    tex->UnlockRect(0);
    return tex;
}

struct HudTextures {
    IDirect3DTexture9* dpad = nullptr;
    IDirect3DTexture9* start = nullptr;
    IDirect3DTexture9* a = nullptr;
    IDirect3DTexture9* b = nullptr;
    IDirect3DTexture9* x = nullptr;
    IDirect3DTexture9* y = nullptr;
    IDirect3DTexture9* lb = nullptr;
    IDirect3DTexture9* rb = nullptr;
    IDirect3DTexture9* lt = nullptr;
    IDirect3DTexture9* rt = nullptr;
    IDirect3DTexture9* back = nullptr;
    IDirect3DTexture9* ls = nullptr;
    IDirect3DTexture9* rs = nullptr;
};

static HudTextures g_Tex;
static bool g_TexturesReady = false;

// Only requires textures for the icons actually enabled in config to have
// loaded -- a load failure on a disabled/unused icon shouldn't take down
// the whole HUD.
static bool EnsureTextures(IDirect3DDevice9* dev) {
    if (g_TexturesReady) return true;
    g_Tex.dpad = CreateTextureFromBmp(dev, kBmpDpad, kBmpDpadLen);
    g_Tex.start = CreateTextureFromBmp(dev, kBmpStart, kBmpStartLen);
    g_Tex.a = CreateTextureFromBmp(dev, kBmpA, kBmpALen);
    g_Tex.b = CreateTextureFromBmp(dev, kBmpB, kBmpBLen);
    g_Tex.x = CreateTextureFromBmp(dev, kBmpX, kBmpXLen);
    g_Tex.y = CreateTextureFromBmp(dev, kBmpY, kBmpYLen);
    g_Tex.lb = CreateTextureFromBmp(dev, kBmpLb, kBmpLbLen);
    g_Tex.rb = CreateTextureFromBmp(dev, kBmpRb, kBmpRbLen);
    g_Tex.lt = CreateTextureFromBmp(dev, kBmpLt, kBmpLtLen);
    g_Tex.rt = CreateTextureFromBmp(dev, kBmpRt, kBmpRtLen);
    g_Tex.back = CreateTextureFromBmp(dev, kBmpBack, kBmpBackLen);
    g_Tex.ls = CreateTextureFromBmp(dev, kBmpLs, kBmpLsLen);
    g_Tex.rs = CreateTextureFromBmp(dev, kBmpRs, kBmpRsLen);

    g_TexturesReady = (!g_Config.showDpad || g_Tex.dpad) && (!g_Config.showStart || g_Tex.start) &&
                       (!g_Config.showA || g_Tex.a) && (!g_Config.showB || g_Tex.b) &&
                       (!g_Config.showX || g_Tex.x) && (!g_Config.showY || g_Tex.y) &&
                       (!g_Config.showLB || g_Tex.lb) && (!g_Config.showRB || g_Tex.rb) &&
                       (!g_Config.showLT || g_Tex.lt) && (!g_Config.showRT || g_Tex.rt) &&
                       (!g_Config.showBack || g_Tex.back) && (!g_Config.showLSClick || g_Tex.ls) &&
                       (!g_Config.showRSClick || g_Tex.rs);
    Log("Textures created: %s\n", g_TexturesReady ? "ok" : "FAILED");
    return g_TexturesReady;
}

// ---------------------------------------------------------------------
// Drawing. Only the render states these draws actually touch are saved
// once per frame and restored at the end (see mad2igttimer/mad2shadowfix
// for why -- per-call full state blocks measurably affect frame time).
// ---------------------------------------------------------------------

struct ColorVertex {
    float x, y, z, rhw;
    DWORD color;
};

// State save/restore + tinted-textured-quad draw now provided by
// mad2textrenderer.dll -- see
// ../../mad2textrenderer/include/mad2textrenderer_api.h. Its
// Mad2TextRendererSavedState/DrawTexturedQuad(..., tint) generalize this
// mod's own MODULATE-against-diffuse dimming (255,255,255,255 = full
// brightness matching SDL_SetTextureColorMod/AlphaMod(255) in the
// original; a dimmer tint for the "not currently pressed" state, matching
// its (100,100,100,100)) -- see Mad2TextRenderer_DrawTexturedQuad's own
// comment for why tint 0xFFFFFFFF is visually identical to a plain
// texture-only sample.
static void DrawTexturedQuad(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float x, float y, float w, float h,
                              DWORD tint) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (!tex || !api.DrawTexturedQuad) return;
    api.DrawTexturedQuad(dev, tex, x, y, w, h, 0, 0, 1, 1, tint);
}

// Thin local aliases so DrawHud's single save/restore around the whole
// multi-quad HUD draw doesn't need to change shape.
using SavedState = Mad2TextRendererSavedState;
static void SaveState(IDirect3DDevice9* dev, SavedState& s) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (api.SaveState) api.SaveState(dev, &s);
}
static void RestoreState(IDirect3DDevice9* dev, SavedState& s) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (api.RestoreState) api.RestoreState(dev, &s);
}

static void SetUntexturedState(IDirect3DDevice9* dev) {
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}

static void DrawFilledQuad(IDirect3DDevice9* dev, float x, float y, float w, float h, DWORD color) {
    ColorVertex verts[4] = {
        {x, y, 0, 1, color},
        {x + w, y, 0, 1, color},
        {x, y + h, 0, 1, color},
        {x + w, y + h, 0, 1, color},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ColorVertex));
}

// 1px-thick box outline via four thin filled quads (avoids relying on
// D3DPT_LINESTRIP width support, which some backends handle inconsistently).
static void DrawBoxOutline(IDirect3DDevice9* dev, float x, float y, float w, float h, DWORD color) {
    DrawFilledQuad(dev, x, y, w, 1, color);
    DrawFilledQuad(dev, x, y + h - 1, w, 1, color);
    DrawFilledQuad(dev, x, y, 1, h, color);
    DrawFilledQuad(dev, x + w - 1, y, 1, h, color);
}

static const DWORD kTintActive = 0xFFFFFFFF;
static const DWORD kTintInactive = 0x64646464;  // (100,100,100,100), matching the original's dimmed state

static void DrawStickIndicator(IDirect3DDevice9* dev, float centerX, float centerY, float boxSize, SHORT stickX,
                                SHORT stickY) {
    float half = boxSize / 2.0f;
    DrawBoxOutline(dev, centerX - half, centerY - half, boxSize, boxSize, 0xFFFFFFFF);

    // XInput sticks are signed 16-bit, +Y is up; screen space +Y is down.
    float nx = static_cast<float>(stickX) / 32768.0f;
    float ny = static_cast<float>(stickY) / 32768.0f;
    float radius = half - 4.0f;
    float dotX = centerX + nx * radius;
    float dotY = centerY - ny * radius;
    DrawFilledQuad(dev, dotX - 3, dotY - 3, 6, 6, 0xFF00FF00);
}

static void DrawHud(IDirect3DDevice9* dev, const XINPUT_STATE& state, int viewportW, int viewportH) {
    if (!EnsureTextures(dev)) return;

    SavedState saved{};
    SaveState(dev, saved);

    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);

    const WORD buttons = state.Gamepad.wButtons;
    bool pressedA = (buttons & XINPUT_GAMEPAD_A) != 0;
    bool pressedB = (buttons & XINPUT_GAMEPAD_B) != 0;
    bool pressedX = (buttons & XINPUT_GAMEPAD_X) != 0;
    bool pressedY = (buttons & XINPUT_GAMEPAD_Y) != 0;
    bool pressedStart = (buttons & XINPUT_GAMEPAD_START) != 0;
    bool pressedUp = (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0;
    bool pressedDown = (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    bool pressedLeft = (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    bool pressedRight = (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    bool pressedLB = (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    bool pressedRB = (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    bool pressedBack = (buttons & XINPUT_GAMEPAD_BACK) != 0;
    bool pressedLThumb = (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    bool pressedRThumb = (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    bool pressedLT = state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    bool pressedRT = state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

    const float hudW = 300.0f;
    const float hudH = 170.0f;
    const float hudX = static_cast<float>(g_Config.marginX);
    const float hudY = static_cast<float>(viewportH) - hudH - static_cast<float>(g_Config.marginY);
    (void)hudW;
    (void)viewportW;

    if (g_Config.showDpad) {
        DrawTexturedQuad(dev, g_Tex.dpad, hudX + 70, hudY + 20, 36, 36,
                          (pressedUp || pressedDown || pressedLeft || pressedRight) ? kTintActive : kTintInactive);
    }
    if (g_Config.showStart) {
        DrawTexturedQuad(dev, g_Tex.start, hudX + 130, hudY + 26, 24, 24, pressedStart ? kTintActive : kTintInactive);
    }
    if (g_Config.showX) {
        DrawTexturedQuad(dev, g_Tex.x, hudX + 175, hudY + 26, 24, 24, pressedX ? kTintActive : kTintInactive);
    }
    if (g_Config.showY) {
        DrawTexturedQuad(dev, g_Tex.y, hudX + 200, hudY + 2, 24, 24, pressedY ? kTintActive : kTintInactive);
    }
    if (g_Config.showA) {
        DrawTexturedQuad(dev, g_Tex.a, hudX + 200, hudY + 50, 24, 24, pressedA ? kTintActive : kTintInactive);
    }
    if (g_Config.showB) {
        DrawTexturedQuad(dev, g_Tex.b, hudX + 225, hudY + 26, 24, 24, pressedB ? kTintActive : kTintInactive);
    }
    if (g_Config.showLB) {
        DrawTexturedQuad(dev, g_Tex.lb, hudX + 20, hudY + 26, 36, 24, pressedLB ? kTintActive : kTintInactive);
    }
    if (g_Config.showRB) {
        DrawTexturedQuad(dev, g_Tex.rb, hudX + 255, hudY + 26, 36, 24, pressedRB ? kTintActive : kTintInactive);
    }
    // Extra row above the main button cluster -- only occupied when one of
    // these is enabled (all default off, so the HUD is unchanged unless
    // opted into).
    if (g_Config.showLT) {
        DrawTexturedQuad(dev, g_Tex.lt, hudX + 20, hudY - 30, 36, 24, pressedLT ? kTintActive : kTintInactive);
    }
    if (g_Config.showBack) {
        DrawTexturedQuad(dev, g_Tex.back, hudX + 130, hudY - 30, 24, 24, pressedBack ? kTintActive : kTintInactive);
    }
    if (g_Config.showRT) {
        DrawTexturedQuad(dev, g_Tex.rt, hudX + 255, hudY - 30, 36, 24, pressedRT ? kTintActive : kTintInactive);
    }

    SetUntexturedState(dev);
    // Geometric center of the dpad quad above (hudX+70, hudY+20, 36x36).
    // (An earlier version of this nudged these by +0.5 based on a
    // screenshot pixel measurement that looked off -- that was a
    // measurement artifact: comparing a bounding-box center computed as
    // (min+max)/2 between an odd-width span (the 27px-wide lit icon,
    // unbiased) and an even-width span (the 16px-wide crosshair arm,
    // which undercounts its true center by exactly 0.5px in that
    // formula) makes an already-correct value look off by 0.5px. Bias-
    // corrected, the crosshair was already exactly on the icon's true
    // center -- see dpadOffsetX/Y below for where the real offset was.)
    const float dpadCenterX = hudX + 88.0f, dpadCenterY = hudY + 38.0f;
    if (g_Config.showDpad && g_Config.debugDpadCrosshair) {
        // Fixed reference mark at the exact math-computed center, always
        // drawn (independent of dpadOffsetX/Y below) so it's possible to
        // read off exactly how far off the tuned green dot still is instead
        // of guessing blindly -- see INPUTDISPLAY_DEBUG_DPAD_CROSSHAIR.
        DrawFilledQuad(dev, dpadCenterX - 1, dpadCenterY - 8, 2, 16, 0xFFFF0000);
        DrawFilledQuad(dev, dpadCenterX - 8, dpadCenterY - 1, 16, 2, 0xFFFF0000);
    }
    if (g_Config.showDpad && (pressedUp || pressedDown || pressedLeft || pressedRight)) {
        float dotX = dpadCenterX + g_Config.dpadOffsetX, dotY = dpadCenterY + g_Config.dpadOffsetY;
        if (pressedUp) dotY -= 10;
        if (pressedDown) dotY += 10;
        if (pressedLeft) dotX -= 10;
        if (pressedRight) dotX += 10;
        DrawFilledQuad(dev, dotX - 3, dotY - 3, 6, 6, 0xFF00FF00);
    }
    DrawStickIndicator(dev, hudX + 55, hudY + 130, 50, state.Gamepad.sThumbLX, state.Gamepad.sThumbLY);
    DrawStickIndicator(dev, hudX + 185, hudY + 130, 50, state.Gamepad.sThumbRX, state.Gamepad.sThumbRY);

    // Small click (L3/R3) badges at the top-right corner of each stick
    // indicator's box, only drawn when explicitly enabled -- the
    // indicators themselves have no press/click state of their own.
    if (g_Config.showLSClick) {
        DrawFilledQuad(dev, hudX + 55 + 25 - 4, hudY + 130 - 25 - 4, 8, 8, pressedLThumb ? 0xFF00FF00 : 0x64646464);
    }
    if (g_Config.showRSClick) {
        DrawFilledQuad(dev, hudX + 185 + 25 - 4, hudY + 130 - 25 - 4, 8, 8, pressedRThumb ? 0xFF00FF00 : 0x64646464);
    }

    RestoreState(dev, saved);
}

// ---------------------------------------------------------------------
// Direct3DCreate9 IAT hook -> IDirect3D9::CreateDevice vtable patch ->
// IDirect3DDevice9::EndScene vtable patch. Identical technique to
// mad2igttimer -- see its file header for why the vtable slot indices
// (16/42) are correct and why this composes with other D3D9-hooking mods
// regardless of mad2/mods/ load order.
// ---------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    EnsureConfigLoaded();
    if (const auto& api = Mad2XInput_Resolve(); api.GetState) {
        XINPUT_STATE state{};
        if (api.GetState(g_Config.userIndex, &state)) {
            D3DVIEWPORT9 vp{};
            This->GetViewport(&vp);
            DrawHud(This, state, static_cast<int>(vp.Width), static_cast<int>(vp.Height));
        }
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("[Dependency] mad2xinput.dll not found or missing exports -- input display HUD will not draw "
                "(controller state unavailable; place mad2xinput.dll in mad2/mods/)\n");
        }
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
