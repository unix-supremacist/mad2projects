// Twitch chat overlay for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Ports the Twitch-chat
// portion of ../mad2mod/src/main.cpp (an external SDL overlay tool) into an
// in-process D3D9 overlay, using Winsock2 instead of POSIX sockets and a
// vendored copy of stb_image.h instead of SDL_image/libwebp for emote
// decoding. Settings come from mad2config.dll the same way every other
// D3D9-hooking mod's config does -- see ../../mad2config/include/mad2config_api.h.
//
// Differences from the ../mad2mod original (found while porting -- these
// were real bugs there, not stylistic choices):
//   - SO_RCVTIMEO takes a DWORD milliseconds value on Windows, not a
//     `struct timeval` like POSIX; and a timed-out recv() fails with
//     WSAGetLastError() == WSAETIMEDOUT, not EAGAIN/EWOULDBLOCK. Getting
//     either of these wrong makes the "just re-check the loop condition on
//     timeout" path either never fire (spinning the CPU) or misclassify a
//     real error as a timeout (silently stalling instead of reconnecting).
//   - Twitch can proactively tell a client to reconnect
//     (":tmi.twitch.tv RECONNECT", sent ahead of planned server maintenance);
//     the original didn't look for it, so it would either miss the request
//     or only recover once Twitch actually dropped the TCP connection.
//   - Connect/disconnect/retry state is now pushed into the chat feed
//     itself (as "* ..." system messages, see PushMessage) as well as
//     twitchchat.log, since a silent reconnect loop is unverifiable from
//     gameplay.
// The outer connect loop in NetworkThreadFunc never gives up -- any
// disconnect (clean close, socket error, or a Twitch-initiated RECONNECT)
// leads back to another connection attempt after a short delay.
//
// Messages are also dropped from the overlay after
// [TwitchChat] MessageLifetimeSeconds (default 120s) regardless of the
// MaxMessages cap, so a channel that goes quiet doesn't leave stale chat
// sitting on screen indefinitely -- see PruneExpiredMessages.
//
// Emotes are matched to chat words by filename (case-insensitive) in
// [TwitchChat] EmotesDir, which ships empty -- this mod includes no emote
// images itself. Supports static .png and static/animated .gif via
// third_party/stb_image.h (vendored, public domain, PNG+GIF decoders only
// -- see CMakeLists.txt). .webp is intentionally not supported: it would
// need libwebp, which isn't worth cross-compiling for i686-w64-mingw32 just
// for this.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#include "stb_image.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"
#include "../../mad2textrenderer/include/mad2textrenderer_api.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------
// Logging -- writes into the shared logs\mad2.log (see mad2sharedlog.h),
// tagged "[mad2twitchchat]" so it stays attributable there.
// ---------------------------------------------------------------------

// Mad2Log_Write's own cross-DLL named mutex already serializes every
// caller (this mod's background network thread included), so no separate
// std::mutex is needed here anymore.
static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2twitchchat]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [TwitchChat] section, resolved lazily on the
// first EndScene call -- see mad2config/include/mad2config_api.h for why
// this can't happen from DllMain).
// ---------------------------------------------------------------------

struct Config {
    std::string channel;               // empty disables the overlay entirely
    std::string emotesDir = "emotes";  // relative to the game's working directory
    int maxMessages = 15;
    int fontSize = 16;
    int width = 350;
    int marginRight = 20;
    int marginTop = 20;
    int emoteSize = 16;
    int messageLifetimeSeconds = 120;
} g_Config;

static bool g_ConfigLoaded = false;

static void EnsureConfigLoaded() {
    if (g_ConfigLoaded) return;
    g_ConfigLoaded = true;

    const auto& api = Mad2Config_Resolve();
    if (!api.GetString) {
        Log("[Dependency] mad2config.dll not found or missing exports -- chat overlay disabled (no default "
            "channel to join without config.cfg; place mad2config.dll in mad2/mods/ and set [TwitchChat] "
            "Channel)\n");
        return;
    }

    char buf[256];
    api.GetString("TwitchChat", "Channel", "",
                   "Twitch channel name to join (no leading #, lowercase).\n"
                   "Leave empty to disable the chat overlay entirely.",
                   buf, sizeof(buf));
    g_Config.channel = buf;
    for (char& c : g_Config.channel) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    api.GetString("TwitchChat", "EmotesDir", g_Config.emotesDir.c_str(),
                   "Directory (relative to the game's working directory) scanned for custom emote images.\n"
                   "Chat words are matched to files here by filename, case-insensitive (e.g. Kappa.png,\n"
                   "PogChamp.gif). Supports .png (static) and .gif (static or animated); .webp is not\n"
                   "supported. Ships empty -- this mod includes no emote images of its own.",
                   buf, sizeof(buf));
    g_Config.emotesDir = buf;

    g_Config.maxMessages = api.GetInt("TwitchChat", "MaxMessages", g_Config.maxMessages,
                                       "Maximum number of chat messages kept on screen at once.");
    g_Config.fontSize = api.GetInt("TwitchChat", "FontSize", g_Config.fontSize, "Pixel height of the chat font.");
    g_Config.width = api.GetInt("TwitchChat", "Width", g_Config.width,
                                 "Pixel width of the chat panel (used for word-wrap).");
    g_Config.marginRight = api.GetInt("TwitchChat", "MarginRight", g_Config.marginRight,
                                       "Margin (pixels) from the right edge of the screen.");
    g_Config.marginTop = api.GetInt("TwitchChat", "MarginTop", g_Config.marginTop,
                                     "Margin (pixels) from the top edge of the screen.");
    g_Config.emoteSize = api.GetInt("TwitchChat", "EmoteSize", g_Config.emoteSize,
                                     "Pixel size (width and height) inline emotes are drawn at.");
    g_Config.messageLifetimeSeconds =
        api.GetInt("TwitchChat", "MessageLifetimeSeconds", g_Config.messageLifetimeSeconds,
                    "Chat messages (including connection status messages) are removed from the overlay\n"
                    "after this many seconds, regardless of MaxMessages.");

    Log("Config loaded: channel='%s' emotesDir='%s' maxMessages=%d fontSize=%d width=%d marginRight=%d "
        "marginTop=%d emoteSize=%d messageLifetimeSeconds=%d\n",
        g_Config.channel.c_str(), g_Config.emotesDir.c_str(), g_Config.maxMessages, g_Config.fontSize,
        g_Config.width, g_Config.marginRight, g_Config.marginTop, g_Config.emoteSize,
        g_Config.messageLifetimeSeconds);

    if (!g_Config.channel.empty()) {
        std::error_code ec;
        fs::create_directories(g_Config.emotesDir, ec);
    }
}

// IAT/vtable hooking now provided by aa_mad2hookutil.dll -- see
// ../../mad2hookutil/include/mad2hookutil_api.h. Resolved synchronously
// (not lazily) since aa_mad2hookutil.dll is guaranteed already loaded by
// the time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Chat message feed: shared between the network thread (producer) and the
// EndScene render thread (consumer). Only g_Messages/g_Revision are
// touched from both threads, so only they need the lock.
// ---------------------------------------------------------------------

struct TwitchMessage {
    std::string user;  // empty for system (connection status) messages
    std::string text;
    std::vector<std::string> words;  // pre-tokenized, so the render thread never re-splits
    bool isSystem;
    uint64_t receivedMs;
};

static std::vector<TwitchMessage> g_Messages;
static std::mutex g_MessagesMutex;
static uint64_t g_Revision = 0;  // bumped on any g_Messages mutation, so the render thread knows to rebuild

static std::vector<std::string> SplitWords(const std::string& s) {
    std::vector<std::string> words;
    std::istringstream iss(s);
    std::string w;
    while (iss >> w) words.push_back(w);
    return words;
}

static void PushMessage(const std::string& user, const std::string& text, bool isSystem) {
    TwitchMessage msg;
    msg.user = user;
    msg.text = text;
    msg.words = SplitWords(text);
    msg.isSystem = isSystem;
    msg.receivedMs = GetTickCount64();

    {
        std::lock_guard<std::mutex> lock(g_MessagesMutex);
        g_Messages.push_back(std::move(msg));
        while (static_cast<int>(g_Messages.size()) > g_Config.maxMessages) g_Messages.erase(g_Messages.begin());
        ++g_Revision;
    }

    if (isSystem) {
        Log("[Chat] * %s\n", text.c_str());
    } else {
        Log("[Chat] %s: %s\n", user.c_str(), text.c_str());
    }
}

// Called every frame from the render thread -- expiry is time-based, so it
// has to happen even when no new message arrives to trigger it (a channel
// that goes quiet should still clear itself out after MessageLifetimeSeconds).
static void PruneExpiredMessages() {
    uint64_t nowMs = GetTickCount64();
    uint64_t lifetimeMs = static_cast<uint64_t>(g_Config.messageLifetimeSeconds) * 1000;

    std::lock_guard<std::mutex> lock(g_MessagesMutex);
    size_t before = g_Messages.size();
    g_Messages.erase(std::remove_if(g_Messages.begin(), g_Messages.end(),
                                     [&](const TwitchMessage& m) { return nowMs - m.receivedMs > lifetimeMs; }),
                      g_Messages.end());
    if (g_Messages.size() != before) ++g_Revision;
}

// ---------------------------------------------------------------------
// Twitch IRC client. Anonymous read-only login (no OAuth token needed for
// "justinfanNNNNNN" names), plain (unencrypted) port 6667 -- Twitch's
// documented anonymous chat-read endpoint. Runs until the process exits;
// any disconnect (error, clean close, or a server-initiated RECONNECT)
// loops back into another connection attempt rather than giving up.
// ---------------------------------------------------------------------

static SOCKET ConnectToTwitch() {
    struct addrinfo hints{};
    struct addrinfo* res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo("irc.chat.twitch.tv", "6667", &hints, &res) != 0) {
        Log("getaddrinfo(irc.chat.twitch.tv) failed: %d\n", WSAGetLastError());
        return INVALID_SOCKET;
    }

    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        Log("socket() failed: %d\n", WSAGetLastError());
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }

    // Windows' SO_RCVTIMEO wants a DWORD milliseconds value, not a
    // `struct timeval` (that's the POSIX shape the ../mad2mod original
    // used) -- getting this wrong silently breaks the timeout.
    DWORD timeoutMs = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    if (connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
        Log("connect() failed: %d\n", WSAGetLastError());
        closesocket(sock);
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return sock;
}

static DWORD WINAPI NetworkThreadFunc(LPVOID) {
    if (g_Config.channel.empty()) {
        Log("No channel configured; chat network thread exiting.\n");
        return 0;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log("WSAStartup failed\n");
        return 0;
    }

    const std::string channel = g_Config.channel;  // config is never reloaded, safe to snapshot once
    Log("Chat thread starting for #%s\n", channel.c_str());

    for (;;) {
        PushMessage("", "Connecting to #" + channel + "...", true);

        SOCKET sock = ConnectToTwitch();
        if (sock == INVALID_SOCKET) {
            PushMessage("", "Connection failed, retrying in 5s...", true);
            Sleep(5000);
            continue;
        }

        std::string nick = "justinfan" + std::to_string(100000 + (rand() % 900000));
        std::string passCmd = "PASS oauth:justinfan\r\n";
        std::string nickCmd = "NICK " + nick + "\r\n";
        std::string joinCmd = "JOIN #" + channel + "\r\n";
        send(sock, passCmd.c_str(), static_cast<int>(passCmd.size()), 0);
        send(sock, nickCmd.c_str(), static_cast<int>(nickCmd.size()), 0);
        send(sock, joinCmd.c_str(), static_cast<int>(joinCmd.size()), 0);

        PushMessage("", "Connected to #" + channel, true);
        Log("Connected and joined #%s as %s\n", channel.c_str(), nick.c_str());

        bool reconnectRequested = false;
        std::string currentLine;
        char recvBuf[4096];

        while (!reconnectRequested) {
            int n = recv(sock, recvBuf, sizeof(recvBuf) - 1, 0);
            if (n == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT) continue;  // just the poll interval; connection is fine
                Log("recv() error: %d\n", err);
                break;
            }
            if (n == 0) {
                Log("Server closed the connection\n");
                break;
            }
            recvBuf[n] = '\0';

            for (int i = 0; i < n; ++i) {
                char c = recvBuf[i];
                if (c != '\n') {
                    currentLine += c;
                    continue;
                }
                if (!currentLine.empty() && currentLine.back() == '\r') currentLine.pop_back();

                if (currentLine.rfind("PING", 0) == 0) {
                    std::string pong = "PONG " + currentLine.substr(5) + "\r\n";
                    send(sock, pong.c_str(), static_cast<int>(pong.size()), 0);
                } else if (currentLine.rfind(":tmi.twitch.tv RECONNECT", 0) == 0) {
                    Log("Twitch requested a reconnect\n");
                    reconnectRequested = true;
                } else {
                    size_t exclamation = currentLine.find('!');
                    size_t privmsg = currentLine.find(" PRIVMSG #");
                    if (!currentLine.empty() && currentLine[0] == ':' && exclamation != std::string::npos &&
                        privmsg != std::string::npos && exclamation < privmsg) {
                        std::string user = currentLine.substr(1, exclamation - 1);
                        size_t colon = currentLine.find(':', privmsg);
                        if (colon != std::string::npos) {
                            PushMessage(user, currentLine.substr(colon + 1), false);
                        }
                    }
                }
                currentLine.clear();

                if (reconnectRequested) break;
            }
        }

        closesocket(sock);
        if (reconnectRequested) {
            PushMessage("", "Reconnecting (requested by Twitch)...", true);
            Sleep(1000);
        } else {
            PushMessage("", "Disconnected, reconnecting in 2s...", true);
            Sleep(2000);
        }
    }
}

static bool g_NetworkThreadStarted = false;

static void EnsureNetworkThread() {
    if (g_NetworkThreadStarted) return;
    g_NetworkThreadStarted = true;
    HANDLE h = CreateThread(nullptr, 0, NetworkThreadFunc, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        Log("CreateThread for chat network thread failed: %lu\n", GetLastError());
    }
}

// ---------------------------------------------------------------------
// Emote loading: PNG (static) and GIF (static or animated) via
// third_party/stb_image.h, decoded from an in-memory buffer (STBI_NO_STDIO
// -- avoids stb's fopen-based path entirely). Only ever touched from the
// render thread (EndScene), so g_Emotes needs no locking.
// ---------------------------------------------------------------------

struct EmoteFrame {
    IDirect3DTexture9* texture;
    int delayMs;
};

struct Emote {
    bool isAnimated = false;
    int totalDurationMs = 0;
    std::vector<EmoteFrame> frames;
    IDirect3DTexture9* staticTexture = nullptr;  // == frames[0].texture; null means "failed to load"
};

static std::map<std::string, Emote> g_Emotes;
static bool g_WarnedWebp = false;

static std::string ToLowerStr(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return r;
}

static bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size <= 0) return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), size);
    return true;
}

// stb_image gives RGBA byte order; D3DFMT_A8R8G8B8 wants BGRA in memory on
// a little-endian target (see mad2inputdisplay's CreateTextureFromBmp for
// the same observation about BMP's native byte order) -- swap R/B per pixel.
static IDirect3DTexture9* CreateTextureFromRGBA(IDirect3DDevice9* dev, const uint8_t* rgba, int w, int h) {
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) return nullptr;
    D3DLOCKED_RECT locked;
    if (FAILED(tex->LockRect(0, &locked, nullptr, 0))) {
        tex->Release();
        return nullptr;
    }
    for (int y = 0; y < h; ++y) {
        const uint8_t* srcRow = rgba + static_cast<size_t>(y) * w * 4;
        uint8_t* dstRow = reinterpret_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
        for (int x = 0; x < w; ++x) {
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
        }
    }
    tex->UnlockRect(0);
    return tex;
}

static bool LoadEmoteFromFile(IDirect3DDevice9* dev, const std::string& path, const std::string& ext, Emote& out) {
    std::vector<uint8_t> buf;
    if (!ReadFileBytes(path, buf)) return false;

    if (ext == ".gif") {
        int w = 0, h = 0, frameCount = 0, channels = 0;
        int* delays = nullptr;
        uint8_t* pixels = stbi_load_gif_from_memory(buf.data(), static_cast<int>(buf.size()), &delays, &w, &h,
                                                      &frameCount, &channels, 4);
        if (!pixels) return false;
        out.isAnimated = frameCount > 1;
        for (int i = 0; i < frameCount; ++i) {
            IDirect3DTexture9* tex = CreateTextureFromRGBA(dev, pixels + static_cast<size_t>(i) * w * h * 4, w, h);
            if (!tex) continue;
            int delay = (delays && delays[i] > 0) ? delays[i] : 100;
            out.frames.push_back({tex, delay});
            out.totalDurationMs += delay;
        }
        if (delays) stbi_image_free(delays);
        stbi_image_free(pixels);
    } else {
        int w = 0, h = 0, channels = 0;
        uint8_t* pixels = stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()), &w, &h, &channels, 4);
        if (!pixels) return false;
        IDirect3DTexture9* tex = CreateTextureFromRGBA(dev, pixels, w, h);
        stbi_image_free(pixels);
        if (!tex) return false;
        out.frames.push_back({tex, 0});
    }

    if (out.frames.empty()) return false;
    out.staticTexture = out.frames[0].texture;
    return true;
}

static const Emote* GetEmote(IDirect3DDevice9* dev, const std::string& word) {
    auto it = g_Emotes.find(word);
    if (it != g_Emotes.end()) return it->second.staticTexture ? &it->second : nullptr;

    Emote loaded;
    bool ok = false;
    std::string lowerWord = ToLowerStr(word);

    std::error_code ec;
    if (fs::exists(g_Config.emotesDir, ec)) {
        for (const auto& entry : fs::directory_iterator(g_Config.emotesDir, ec)) {
            if (ec || !entry.is_regular_file(ec)) continue;
            std::string ext = ToLowerStr(entry.path().extension().string());
            if (ext != ".png" && ext != ".gif") {
                if (ext == ".webp" && !g_WarnedWebp) {
                    g_WarnedWebp = true;
                    Log("[Emotes] .webp files (e.g. %s) are not supported -- convert to .png or .gif\n",
                        entry.path().filename().string().c_str());
                }
                continue;
            }
            if (ToLowerStr(entry.path().stem().string()) != lowerWord) continue;
            ok = LoadEmoteFromFile(dev, entry.path().string(), ext, loaded);
            break;
        }
    }

    g_Emotes[word] = loaded;  // caches failure too, so a missing emote isn't rescanned every frame
    return ok ? &g_Emotes[word] : nullptr;
}

// ---------------------------------------------------------------------
// Text rendering: GDI-rasterized-to-texture, same technique as
// mad2igttimer (no D3DX dependency), generalized from "N fixed lines" to
// "a list of positioned, individually-colored runs" since chat needs
// per-username/per-message coloring and dynamic line counts (word-wrap +
// growing/shrinking message list).
// ---------------------------------------------------------------------

// Layout representation stays local (x, y, text, color per positioned
// run) -- only the GDI rasterization/quad-draw moved to
// mad2textrenderer.dll (see ../../mad2textrenderer/include/mad2textrenderer_api.h).
// This mod is the per-run-colored/positioned consumer that header's
// Mad2TextRun generalizes from (vs. mad2igttimer/mad2playercoords'
// N-fixed-lines case) -- converted to Mad2TextRun only at the Render()
// call site (see RenderTextTexture below), so the surrounding layout code
// (username/message/system-message coloring, word-wrap) doesn't need to
// change shape.
struct TextRun {
    int x, y;
    std::string text;
    COLORREF color;
};

static const int kPadding = 6;
static const int kOutlineOffset = 2;

static Mad2TextSurface g_Surface = nullptr;

static bool EnsureFontResources() {
    if (g_Surface) return true;
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.CreateSurface) return false;
    g_Surface = api.CreateSurface(g_Config.fontSize);
    return g_Surface != nullptr;
}

static int MeasureTextWidth(const std::string& text) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.MeasureTextWidth) return 0;
    return api.MeasureTextWidth(g_Surface, text.c_str());
}

// Resizes the surface's backing texture only when the required size
// actually changes (texture width is fixed at g_Config.width; height
// grows/shrinks with the message list) -- *outRecreated tells the caller
// a re-render is needed even if the message content's revision hasn't
// changed (e.g. very first frame).
static bool EnsureTextTexture(IDirect3DDevice9* dev, int w, int h, bool* outRecreated) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.EnsureSize) return false;
    int prevW = 0, prevH = 0;
    if (api.GetSize) api.GetSize(g_Surface, &prevW, &prevH);
    bool ok = api.EnsureSize(g_Surface, dev, w, h) != FALSE;
    if (outRecreated) *outRecreated = ok && (prevW != w || prevH != h);
    if (!ok) Log("EnsureSize (chat text, %dx%d) failed\n", w, h);
    return ok;
}

static int LineHeight() {
    const auto& api = Mad2TextRenderer_Resolve();
    return api.GetLineHeight ? api.GetLineHeight(g_Surface) : 0;
}

static void RenderTextTexture(const std::vector<TextRun>& runs) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (!api.Render) return;
    std::vector<Mad2TextRun> converted(runs.size());
    for (size_t i = 0; i < runs.size(); ++i) {
        converted[i] = {runs[i].x, runs[i].y, runs[i].text.c_str(), runs[i].color};
    }
    api.Render(g_Surface, converted.data(), static_cast<int>(converted.size()));
}

// ---------------------------------------------------------------------
// Layout: walks the (already pruned) message list once, producing the text
// runs to bake into the GDI texture plus the screen positions of any
// inline emotes (drawn as separate D3D quads on top, so animated emotes
// don't require rebuilding the text texture every frame -- see HookEndScene).
// ---------------------------------------------------------------------

struct EmoteDraw {
    int x, y;
    const Emote* emote;
};

struct ChatLayout {
    std::vector<TextRun> textRuns;
    std::vector<EmoteDraw> emoteDraws;
    int width = 0;
    int height = 0;
};

static const COLORREF kColorTwitchPurple = RGB(169, 112, 255);
static const COLORREF kColorMessageText = RGB(255, 255, 255);
static const COLORREF kColorSystem = RGB(255, 215, 0);

static ChatLayout BuildChatLayout(IDirect3DDevice9* dev, const std::vector<TwitchMessage>& messages,
                                   const std::string& channel) {
    ChatLayout layout;
    layout.width = g_Config.width;

    int innerWidth = g_Config.width - kPadding * 2 - kOutlineOffset * 2;
    if (innerWidth < 20) innerWidth = 20;

    const int baseX = kPadding + kOutlineOffset;
    int y = kPadding + kOutlineOffset;

    char headerBuf[128];
    snprintf(headerBuf, sizeof(headerBuf), "Twitch Chat (#%s):", channel.c_str());
    layout.textRuns.push_back({baseX, y, headerBuf, kColorTwitchPurple});
    y += LineHeight();

    const int emoteSize = g_Config.emoteSize;

    for (const auto& m : messages) {
        int x = baseX;
        COLORREF msgColor = m.isSystem ? kColorSystem : kColorMessageText;

        std::string prefix = m.isSystem ? "* " : (m.user + ": ");
        layout.textRuns.push_back({x, y, prefix, m.isSystem ? kColorSystem : kColorTwitchPurple});
        x += MeasureTextWidth(prefix);

        for (size_t wi = 0; wi < m.words.size(); ++wi) {
            const std::string& w = m.words[wi];
            const Emote* emote = m.isSystem ? nullptr : GetEmote(dev, w);
            int pieceWidth = emote ? (emoteSize + 4) : MeasureTextWidth(w + " ");

            if (x - baseX + pieceWidth > innerWidth && x > baseX) {
                y += LineHeight();
                x = baseX + 16;  // indent wrapped continuation lines
            }

            if (emote) {
                layout.emoteDraws.push_back({x, y - 2, emote});
                x += emoteSize + 4;
            } else {
                std::string piece = w;
                if (wi + 1 < m.words.size()) piece += " ";
                layout.textRuns.push_back({x, y, piece, msgColor});
                x += MeasureTextWidth(piece);
            }
        }
        y += LineHeight();
    }

    layout.height = y + kPadding;
    return layout;
}

// ---------------------------------------------------------------------
// Drawing. Only the render states these draws actually touch are
// saved/restored once per frame (see mad2igttimer/mad2inputdisplay for why
// full state blocks aren't used here -- per-call overhead in this D3D9
// hook chain measurably affects frame time).
// ---------------------------------------------------------------------

// Save/restore + tinted quad draw now provided by mad2textrenderer.dll --
// see ../../mad2textrenderer/include/mad2textrenderer_api.h. Unlike
// mad2igttimer/mad2playercoords/mad2effectshud (which each draw exactly
// one quad via the surface's own self-contained DrawQuad), this mod draws
// the chat text texture AND every visible emote quad under ONE shared
// SaveState/RestoreState block, matching the original's structure -- so
// it uses the lower-level SaveState/RestoreState/DrawTexturedQuad(...,
// tint=0xFFFFFFFF) primitives directly, plus GetTexture to pull the raw
// D3D9 texture out of the (otherwise opaque) text surface for the first
// of those quads.
using SavedState = Mad2TextRendererSavedState;

static void DrawTexQuad(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float x, float y, float w, float h) {
    const auto& api = Mad2TextRenderer_Resolve();
    if (!tex || !api.DrawTexturedQuad) return;
    api.DrawTexturedQuad(dev, tex, x, y, w, h, 0, 0, 1, 1, 0xFFFFFFFF);
}

static void DrawChatOverlay(IDirect3DDevice9* dev, const ChatLayout& layout) {
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    float x = static_cast<float>(vp.Width) - static_cast<float>(g_Config.width) -
              static_cast<float>(g_Config.marginRight);
    float y = static_cast<float>(g_Config.marginTop);

    const auto& api = Mad2TextRenderer_Resolve();
    SavedState saved{};
    if (api.SaveState) api.SaveState(dev, &saved);

    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);

    IDirect3DTexture9* textTexture = api.GetTexture ? api.GetTexture(g_Surface) : nullptr;
    DrawTexQuad(dev, textTexture, x, y, static_cast<float>(layout.width), static_cast<float>(layout.height));

    uint64_t nowMs = GetTickCount64();
    for (const auto& ed : layout.emoteDraws) {
        IDirect3DTexture9* frameTex = ed.emote->staticTexture;
        if (ed.emote->isAnimated && ed.emote->totalDurationMs > 0) {
            int t = static_cast<int>(nowMs % static_cast<uint64_t>(ed.emote->totalDurationMs));
            int elapsed = 0;
            for (const auto& f : ed.emote->frames) {
                elapsed += f.delayMs;
                if (t < elapsed) {
                    frameTex = f.texture;
                    break;
                }
            }
        }
        DrawTexQuad(dev, frameTex, x + ed.x, y + ed.y, static_cast<float>(g_Config.emoteSize),
                    static_cast<float>(g_Config.emoteSize));
    }

    if (api.RestoreState) api.RestoreState(dev, &saved);
}

// ---------------------------------------------------------------------
// Direct3DCreate9 IAT hook -> IDirect3D9::CreateDevice vtable patch ->
// IDirect3DDevice9::EndScene vtable patch. Identical technique to
// mad2igttimer -- see its file header for why vtable slots 16/42 are
// correct and why this composes with other D3D9-hooking mods regardless of
// mad2/mods/ load order.
// ---------------------------------------------------------------------

static uint64_t g_LastRenderedRevision = ~static_cast<uint64_t>(0);
static ChatLayout g_CachedLayout;

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
static EndScene_t RealEndScene = nullptr;

static HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* This) {
    EnsureConfigLoaded();
    EnsureNetworkThread();

    if (g_Config.channel.empty() || !EnsureFontResources()) return RealEndScene(This);

    PruneExpiredMessages();

    // BuildChatLayout does a GetTextExtentPoint32A call per word (for
    // word-wrap) -- worth skipping on frames where the message list hasn't
    // actually changed, same "don't pay per-frame cost for content that
    // rarely changes" reasoning as igttimer/shadowfix's per-call overhead
    // comments. Emote animation still updates every frame regardless (see
    // DrawChatOverlay), since that only needs the cached quad positions
    // plus the current tick, not a re-layout.
    uint64_t revision;
    std::vector<TwitchMessage> snapshot;
    bool contentChanged;
    {
        std::lock_guard<std::mutex> lock(g_MessagesMutex);
        revision = g_Revision;
        contentChanged = (revision != g_LastRenderedRevision);
        if (contentChanged) snapshot = g_Messages;
    }
    if (contentChanged) {
        g_CachedLayout = BuildChatLayout(This, snapshot, g_Config.channel);
    }

    bool recreated = false;
    if (EnsureTextTexture(This, g_CachedLayout.width, g_CachedLayout.height, &recreated)) {
        if (recreated || contentChanged) {
            RenderTextTexture(g_CachedLayout.textRuns);
            g_LastRenderedRevision = revision;
        }
        DrawChatOverlay(This, g_CachedLayout);
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
            srand(static_cast<unsigned>(time(nullptr)));
            InstallHooks();
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
