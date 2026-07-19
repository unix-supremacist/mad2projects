// Now-playing music/podcast HUD for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. mad2music (a native Linux
// process, always running for the whole session -- see ../mad2music and
// ../mad2relauncher/music.go) broadcasts a Mad2PodcastStatusPacket once a
// second over loopback UDP (see
// ../../mad2podcastmod/include/mad2podcast_protocol.h) describing what it's
// currently playing on each of its two audio slots. This mod listens for
// those broadcasts on a background thread and draws up to two lines --
// "Music: <track>" / "Podcast: <track>" -- top-left, positioned below
// mad2playercoords' own overlay (X=20,Y=140, ~3 lines tall) so the two
// don't overlap. The other three screen corners are already claimed by
// mad2igttimer (top-left, above playercoords), mad2twitchchat (top-right),
// mad2inputdisplay (bottom-left), and mad2effectshud (bottom-right) -- see
// CLAUDE.md's per-mod defaults.
//
// D3D9 hook technique and GDI-rasterized-text-to-texture rendering are
// identical to mad2effectshud (own file header explains the no-D3DX
// reasoning) -- this mod is structurally the same shape, just fed by a UDP
// status broadcast instead of querying mad2effects' registry every frame.
//
// Unlike every other mod here, this one has no hard dependency on any other
// *.dll in mad2/mods/ -- mad2music is a separate native process, entirely
// optional (if -music-path wasn't passed to mad2relauncher, or mad2music
// isn't running for any other reason, no packets ever arrive and this HUD
// simply never draws anything, zero footprint, no [Dependency] log spam).
// A staleness check (no packet in 3s -> treat as not-playing) covers
// mad2music being mid-crash-relaunch or not running at all, rather than
// leaving a stale "Music: ..." line stuck on screen forever.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2podcastmod/include/mad2podcast_protocol.h"

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2musichud]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [MusicHud] section for display, [Podcast]
// StatusPort for the wire -- StatusPort lives in the same section
// mad2podcastmod's ControlPort does since both belong to the same protocol,
// even though mad2podcastmod itself never reads it -- this mod is the
// actual consumer, so it declares/seeds the default, same "whoever uses a
// key owns its default" convention as every other mod's config section.
// ---------------------------------------------------------------------

struct Config {
    bool enabled = true;
    int fontSize = 16;
    int x = 20;
    int y = 230;  // below mad2playercoords' Y=140 + ~3 lines
    int statusPort = MAD2PODCAST_DEFAULT_STATUS_PORT;
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

    g_Config.enabled = api.GetBool("MusicHud", "Enabled", TRUE,
                                    "Show the current music/podcast track, top-left (below the coordinate "
                                    "overlay).") != FALSE;
    g_Config.fontSize = api.GetInt("MusicHud", "FontSize", g_Config.fontSize, "Pixel height of the HUD font.");
    g_Config.x = api.GetInt("MusicHud", "X", g_Config.x, "Screen-space X position (pixels) of the overlay's top-left corner.");
    g_Config.y = api.GetInt("MusicHud", "Y", g_Config.y, "Screen-space Y position (pixels) of the overlay's top-left corner.");
    g_Config.statusPort = api.GetInt(
        "Podcast", "StatusPort", g_Config.statusPort,
        "Loopback UDP port mad2music broadcasts now-playing status to. Must match\n"
        "mad2music's [Podcast] StatusPort (config.cfg is read by both sides\n"
        "independently -- see mad2music/src/config.cpp for why).");

    Log("Config loaded: enabled=%d fontSize=%d x=%d y=%d statusPort=%d\n", g_Config.enabled, g_Config.fontSize,
        g_Config.x, g_Config.y, g_Config.statusPort);
}

// ---------------------------------------------------------------------
// Now-playing state, updated by the status-listener thread, read by the
// EndScene hook. A 3s staleness window (see file header) covers mad2music
// being mid-crash-relaunch or not running at all.
// ---------------------------------------------------------------------

static std::mutex g_StatusMutex;
static bool g_MusicPlaying = false;
static bool g_PodcastPlaying = false;
static std::string g_MusicTrack;
static std::string g_PodcastTrack;
static ULONGLONG g_LastPacketTickMs = 0;

static const ULONGLONG kStaleAfterMs = 3000;

static bool g_WinsockReady = false;

static DWORD WINAPI StatusListenerThreadFunc(LPVOID) {
    WSADATA wsaData;
    g_WinsockReady = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    if (!g_WinsockReady) {
        Log("WSAStartup failed -- now-playing status unavailable\n");
        return 0;
    }

    // Config isn't guaranteed loaded yet (mad2config may still be
    // resolving), so this thread polls until it can read StatusPort rather
    // than blocking DllMain -- same "retry until resolvable" shape as every
    // other mod's init thread.
    for (int attempts = 0; !Mad2Config_Resolve().GetInt && attempts < 50; ++attempts) Sleep(100);
    EnsureConfigLoaded();

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        Log("Failed to create status socket\n");
        return 0;
    }
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bindAddr.sin_port = htons(static_cast<u_short>(g_Config.statusPort));
    if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        Log("Failed to bind status socket on port %d (err=%d)\n", g_Config.statusPort, WSAGetLastError());
        closesocket(sock);
        return 0;
    }
    Log("Listening for now-playing status on loopback UDP port %d\n", g_Config.statusPort);

    for (;;) {
        Mad2PodcastStatusPacket pkt{};
        int n = recv(sock, reinterpret_cast<char*>(&pkt), sizeof(pkt), 0);
        if (n != static_cast<int>(sizeof(pkt))) continue;
        if (pkt.magic != MAD2PODCAST_STATUS_MAGIC) continue;

        // NUL-terminate defensively -- a malformed/truncated packet
        // shouldn't be able to run string ops past the buffer.
        pkt.musicTrack[MAD2PODCAST_TRACK_NAME_LEN - 1] = '\0';
        pkt.podcastTrack[MAD2PODCAST_TRACK_NAME_LEN - 1] = '\0';

        std::lock_guard<std::mutex> lock(g_StatusMutex);
        g_MusicPlaying = pkt.musicPlaying != 0;
        g_PodcastPlaying = pkt.podcastPlaying != 0;
        g_MusicTrack = pkt.musicTrack;
        g_PodcastTrack = pkt.podcastTrack;
        g_LastPacketTickMs = GetTickCount64();
    }
}

// Snapshot of what to draw right now, staleness-checked.
struct NowPlayingSnapshot {
    bool musicPlaying = false;
    bool podcastPlaying = false;
    std::string musicTrack;
    std::string podcastTrack;
};

static NowPlayingSnapshot GetNowPlayingSnapshot() {
    std::lock_guard<std::mutex> lock(g_StatusMutex);
    NowPlayingSnapshot snap;
    bool fresh = g_LastPacketTickMs != 0 && (GetTickCount64() - g_LastPacketTickMs) < kStaleAfterMs;
    if (!fresh) return snap;  // all false/empty -- mad2music hasn't reported in a while
    snap.musicPlaying = g_MusicPlaying;
    snap.podcastPlaying = g_PodcastPlaying;
    snap.musicTrack = g_MusicTrack;
    snap.podcastTrack = g_PodcastTrack;
    return snap;
}

// IAT/vtable hooking now provided by aa_mad2hookutil.dll -- see
// ../../mad2hookutil/include/mad2hookutil_api.h. Resolved synchronously
// (not lazily) since aa_mad2hookutil.dll is guaranteed already loaded by
// the time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Overlay rendering: GDI-rasterized-to-texture (no D3DX -- see file
// header). Identical technique to mad2effectshud -- see that file for the
// full explanation of the two-pass stroke+mask approach. Sized dynamically,
// rebuilt only when the displayed lines actually change.
// ---------------------------------------------------------------------

static const int kPadding = 6;
static const int kOutlineOffset = 2;

static HDC g_MemDC = nullptr;
static HBITMAP g_Bitmap = nullptr;
static void* g_Bits = nullptr;
static HFONT g_Font = nullptr;
static int g_LineHeight = 0;
static int g_TexW = 0, g_TexH = 0;
static IDirect3DTexture9* g_TextTexture = nullptr;
static std::vector<uint32_t> g_ColorBuf;

static bool EnsureFontResources() {
    if (g_Font) return true;
    HDC screenDC = GetDC(nullptr);
    g_MemDC = CreateCompatibleDC(screenDC);
    ReleaseDC(nullptr, screenDC);

    g_Font = CreateFontA(g_Config.fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SelectObject(g_MemDC, g_Font);
    SetBkMode(g_MemDC, TRANSPARENT);

    SIZE extent{};
    GetTextExtentPoint32A(g_MemDC, "Mg", 2, &extent);
    g_LineHeight = extent.cy + 6;
    return g_Font != nullptr;
}

static int MeasureTextWidth(const char* text) {
    SIZE extent{};
    GetTextExtentPoint32A(g_MemDC, text, static_cast<int>(strlen(text)), &extent);
    return extent.cx;
}

static bool EnsureTextTexture(IDirect3DDevice9* dev, int w, int h, bool* outRecreated) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (outRecreated) *outRecreated = false;
    if (g_TextTexture && g_TexW == w && g_TexH == h) return true;

    if (g_TextTexture) {
        g_TextTexture->Release();
        g_TextTexture = nullptr;
    }
    if (g_Bitmap) {
        DeleteObject(g_Bitmap);
        g_Bitmap = nullptr;
    }

    g_TexW = w;
    g_TexH = h;
    HRESULT hr = dev->CreateTexture(g_TexW, g_TexH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_TextTexture, nullptr);
    if (FAILED(hr)) {
        Log("CreateTexture (HUD text) failed: hr=0x%08X\n", hr);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_TexW;
    bmi.bmiHeader.biHeight = -g_TexH;  // top-down DIB, matches D3D texture row order
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_Bitmap = CreateDIBSection(g_MemDC, &bmi, DIB_RGB_COLORS, &g_Bits, nullptr, 0);
    SelectObject(g_MemDC, g_Bitmap);
    SelectObject(g_MemDC, g_Font);  // re-select: SelectObject(bitmap) can reset the DC's font slot
    SetBkMode(g_MemDC, TRANSPARENT);
    g_ColorBuf.resize(static_cast<size_t>(g_TexW) * g_TexH);

    if (outRecreated) *outRecreated = true;
    return true;
}

static const COLORREF kColorMusic = RGB(150, 220, 255);
static const COLORREF kColorPodcast = RGB(255, 200, 60);

static void DrawStrokedLines(const std::vector<std::string>& lines, const std::vector<COLORREF>& colors,
                              bool maskPass) {
    static const int offsets[4][2] = {{-kOutlineOffset, 0}, {kOutlineOffset, 0}, {0, -kOutlineOffset},
                                       {0, kOutlineOffset}};
    for (size_t li = 0; li < lines.size(); ++li) {
        RECT base{kPadding + kOutlineOffset, kPadding / 2 + kOutlineOffset + static_cast<int>(li) * g_LineHeight,
                   g_TexW, 0};
        base.bottom = base.top + g_LineHeight;

        COLORREF fill = maskPass ? RGB(255, 255, 255) : colors[li];
        COLORREF outline = maskPass ? RGB(255, 255, 255) : RGB(0, 0, 0);

        SetTextColor(g_MemDC, outline);
        for (auto& off : offsets) {
            RECT r = base;
            OffsetRect(&r, off[0], off[1]);
            DrawTextA(g_MemDC, lines[li].c_str(), -1, &r, DT_NOCLIP | DT_SINGLELINE);
        }
        SetTextColor(g_MemDC, fill);
        DrawTextA(g_MemDC, lines[li].c_str(), -1, &base, DT_NOCLIP | DT_SINGLELINE);
    }
}

static void RenderTextTexture(const std::vector<std::string>& lines, const std::vector<COLORREF>& colors) {
    size_t pixelCount = static_cast<size_t>(g_TexW) * g_TexH;

    memset(g_Bits, 0, pixelCount * 4);
    DrawStrokedLines(lines, colors, false);
    GdiFlush();
    memcpy(g_ColorBuf.data(), g_Bits, pixelCount * 4);

    memset(g_Bits, 0, pixelCount * 4);
    DrawStrokedLines(lines, colors, true);
    GdiFlush();

    uint32_t* mask = reinterpret_cast<uint32_t*>(g_Bits);
    for (size_t i = 0; i < pixelCount; ++i) {
        uint32_t maskPixel = mask[i];
        uint8_t a = std::max({static_cast<uint8_t>(maskPixel & 0xFF), static_cast<uint8_t>((maskPixel >> 8) & 0xFF),
                               static_cast<uint8_t>((maskPixel >> 16) & 0xFF)});
        uint32_t colorPixel = g_ColorBuf[i] & 0x00FFFFFF;
        mask[i] = (static_cast<uint32_t>(a) << 24) | colorPixel;
    }

    D3DLOCKED_RECT locked;
    if (SUCCEEDED(g_TextTexture->LockRect(0, &locked, nullptr, 0))) {
        for (int y = 0; y < g_TexH; ++y) {
            memcpy(reinterpret_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch,
                   reinterpret_cast<uint8_t*>(g_Bits) + static_cast<size_t>(y) * g_TexW * 4,
                   static_cast<size_t>(g_TexW) * 4);
        }
        g_TextTexture->UnlockRect(0);
    }
}

// ---------------------------------------------------------------------
// Drawing. Only the render states this draw actually touches are
// saved/restored (see mad2igttimer/mad2inputdisplay for why -- per-call
// full state blocks measurably affect frame time).
// ---------------------------------------------------------------------

struct SavedState {
    DWORD alphaBlend, srcBlend, destBlend, zEnable, alphaTest, cullMode, lighting, fogEnable;
    DWORD colorOp, colorArg1, alphaOp, alphaArg1;
    DWORD minFilter, magFilter, fvf;
    IDirect3DBaseTexture9* texture0 = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
};

static void SaveState(IDirect3DDevice9* dev, SavedState& s) {
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &s.alphaBlend);
    dev->GetRenderState(D3DRS_SRCBLEND, &s.srcBlend);
    dev->GetRenderState(D3DRS_DESTBLEND, &s.destBlend);
    dev->GetRenderState(D3DRS_ZENABLE, &s.zEnable);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &s.alphaTest);
    dev->GetRenderState(D3DRS_CULLMODE, &s.cullMode);
    dev->GetRenderState(D3DRS_LIGHTING, &s.lighting);
    dev->GetRenderState(D3DRS_FOGENABLE, &s.fogEnable);
    dev->GetTextureStageState(0, D3DTSS_COLOROP, &s.colorOp);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &s.colorArg1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &s.alphaOp);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &s.alphaArg1);
    dev->GetSamplerState(0, D3DSAMP_MINFILTER, &s.minFilter);
    dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &s.magFilter);
    dev->GetFVF(&s.fvf);
    dev->GetTexture(0, &s.texture0);
    dev->GetVertexShader(&s.vs);
    dev->GetPixelShader(&s.ps);
}

static void RestoreState(IDirect3DDevice9* dev, SavedState& s) {
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, s.alphaBlend);
    dev->SetRenderState(D3DRS_SRCBLEND, s.srcBlend);
    dev->SetRenderState(D3DRS_DESTBLEND, s.destBlend);
    dev->SetRenderState(D3DRS_ZENABLE, s.zEnable);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, s.alphaTest);
    dev->SetRenderState(D3DRS_CULLMODE, s.cullMode);
    dev->SetRenderState(D3DRS_LIGHTING, s.lighting);
    dev->SetRenderState(D3DRS_FOGENABLE, s.fogEnable);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, s.colorOp);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, s.colorArg1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, s.alphaOp);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, s.alphaArg1);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, s.minFilter);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, s.magFilter);
    dev->SetFVF(s.fvf);
    dev->SetTexture(0, s.texture0);
    if (s.texture0) s.texture0->Release();
    dev->SetVertexShader(s.vs);
    if (s.vs) s.vs->Release();
    dev->SetPixelShader(s.ps);
    if (s.ps) s.ps->Release();
}

struct ScreenVertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

static void DrawHudQuad(IDirect3DDevice9* dev) {
    float x = static_cast<float>(g_Config.x);
    float y = static_cast<float>(g_Config.y);

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
    dev->SetTexture(0, g_TextTexture);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);

    DWORD white = 0xFFFFFFFF;
    ScreenVertex verts[4] = {
        {x, y, 0, 1, white, 0, 0},
        {x + g_TexW, y, 0, 1, white, 1, 0},
        {x, y + g_TexH, 0, 1, white, 0, 1},
        {x + g_TexW, y + g_TexH, 0, 1, white, 1, 1},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ScreenVertex));

    RestoreState(dev, saved);
}

// ---------------------------------------------------------------------
// Direct3DCreate9 IAT hook -> IDirect3D9::CreateDevice vtable patch ->
// IDirect3DDevice9::EndScene vtable patch. Identical technique to
// mad2igttimer/mad2effectshud -- see either's file header for why vtable
// slots 16/42 are correct and why this composes with other D3D9-hooking
// mods regardless of mad2/mods/ load order.
// ---------------------------------------------------------------------

static std::vector<std::string> g_LastLines;

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    EnsureConfigLoaded();

    if (!g_Config.enabled || !EnsureFontResources()) return RealEndScene(This);

    NowPlayingSnapshot snap = GetNowPlayingSnapshot();
    if (!snap.musicPlaying && !snap.podcastPlaying) return RealEndScene(This);  // nothing to draw

    std::vector<std::string> lines;
    std::vector<COLORREF> colors;
    if (snap.musicPlaying) {
        lines.push_back("Music: " + (snap.musicTrack.empty() ? std::string("(unknown)") : snap.musicTrack));
        colors.push_back(kColorMusic);
    }
    if (snap.podcastPlaying) {
        lines.push_back("Podcast: " + (snap.podcastTrack.empty() ? std::string("(unknown)") : snap.podcastTrack));
        colors.push_back(kColorPodcast);
    }

    if (lines != g_LastLines) {
        int maxWidth = 0;
        for (auto& line : lines) maxWidth = std::max(maxWidth, MeasureTextWidth(line.c_str()));

        int neededW = maxWidth + kPadding * 2 + kOutlineOffset * 2;
        int neededH = static_cast<int>(lines.size()) * g_LineHeight + kPadding + kOutlineOffset * 2;

        bool recreated = false;
        if (EnsureTextTexture(This, neededW, neededH, &recreated)) {
            RenderTextTexture(lines, colors);
            g_LastLines = lines;
        }
    }

    if (g_TextTexture) DrawHudQuad(This);

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
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            InstallHooks();
            HANDLE h = CreateThread(nullptr, 0, StatusListenerThreadFunc, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
            break;
        }
        case DLL_PROCESS_DETACH:
            if (g_WinsockReady) WSACleanup();
            break;
        default:
            break;
    }
    return TRUE;
}
