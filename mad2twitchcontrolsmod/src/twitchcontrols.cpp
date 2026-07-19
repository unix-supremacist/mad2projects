// Twitch channel-points-redemption -> effect trigger for Madagascar 2.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Separate from
// mad2twitchchat (which is a read-only, anonymous-login chat overlay --
// anonymous IRC structurally cannot see channel-point redemptions, only
// chat messages). This mod instead speaks Twitch's authenticated EventSub
// WebSocket protocol and maps configured reward titles to mad2effects
// (mad2/mods/mad2effects.dll) effect triggers.
//
// Credentials (OAuth token, Client ID, Broadcaster User ID) are pasted
// manually into config.cfg by the user, obtained from an externally
// registered Twitch application with the channel:read:redemptions scope
// (e.g. via a token generator site) -- there is no in-mod OAuth flow.
//
// Transport is WinHTTP (winhttp.h), not Winsock -- unlike mad2twitchchat's
// plaintext IRC connection, this needs TLS (wss://) for both the
// WebSocket and the plain HTTPS subscription-creation call, and WinHTTP
// handles both transparently with the same API, no hand-rolled TLS.
// Verified working under this repo's actual Proton/Wine target via a
// standalone spike (opened a real WSS connection to
// eventsub.wss.twitch.tv/ws and received a genuine session_welcome frame)
// before this mod was written, per the plan's "validate the riskiest
// transport choice first" guidance.
//
// Protocol flow (see https://dev.twitch.tv/docs/eventsub/handling-websocket-events/):
//   1. Connect wss://eventsub.wss.twitch.tv/ws.
//   2. First message is always "session_welcome"; its payload.session.id
//      is the session id.
//   3. Separately, POST https://api.twitch.tv/helix/eventsub/subscriptions
//      (Authorization: Bearer <token>, Client-Id: <clientId>) subscribing
//      "channel.channel_points_custom_reward_redemption.add" with
//      transport {method:"websocket", session_id:<from welcome>}. Expect
//      202.
//   4. "notification" messages thereafter carry the redemption
//      (payload.event.reward.title etc).
//   5. "session_keepalive" is informational only.
//   6. "session_reconnect" is a real protocol requirement: Twitch's fully
//      correct behavior is a seamless dual-socket handover via the
//      provided reconnect_url. This mod takes the documented v1
//      simplification instead: on session_reconnect, close everything and
//      rerun the whole outer connect loop from scratch (new /ws connect,
//      new session_welcome, redo the subscription POST against the new
//      session id) -- simpler, but may drop events during the brief gap.
//   7. The outer loop never gives up on any error (mirrors
//      mad2twitchchat's NetworkThreadFunc philosophy exactly): close
//      everything, sleep, retry from step 1.
//
// JSON handling: no JSON library is vendored anywhere in this repo (only
// stb_image.h, for mad2twitchchat's emotes) -- consistent with that,
// this hand-rolls small substring scans for the handful of fixed fields
// this mod actually needs (message_type, session id, reward title,
// reconnect_url). This is fragile to any change in Twitch's payload
// shape/field ordering/escaping -- accepted here for the same reason
// mad2config's own file format avoids a third-party parser: not worth
// pulling in a library for something this small on a static-runtime MinGW
// cross-compile target.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../mad2config/include/mad2config_api.h"
#include "../../mad2effects/include/mad2effects_api.h"

#include "../../mad2sharedlog/mad2sharedlog.h"

// ---------------------------------------------------------------------
// Logging -- this mod's Log() wrapper writes into the shared logs\mad2.log file (see mad2sharedlog.h), tagged so it stays attributable there.
// ---------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2twitchcontrolsmod]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (mad2config.dll's [TwitchControls] section) -- numbered discrete
// keys for the reward->effect mapping, not a packed/delimited value:
// mad2config's file format is strictly single-line/single-value per key
// (see config_dll.cpp's FindKey/GetOrSeedString), no list support.
// ---------------------------------------------------------------------

struct RewardMapping {
    std::string reward;  // Twitch reward title, matched trimmed/case-insensitive
    std::string effect;  // mad2effects effect name to trigger
};

struct Config {
    bool enabled = false;
    std::string oauthToken;
    std::string clientId;
    std::string broadcasterUserId;
    std::vector<RewardMapping> mappings;
} g_Config;

static std::string Trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

static void LoadConfig() {
    const auto& api = Mad2Config_Resolve();
    if (!api.GetString) {
        Log("[Dependency] mad2config.dll not found or missing exports -- Twitch controls disabled (no config to "
            "read credentials from; place mad2config.dll in mad2/mods/)\n");
        return;
    }

    g_Config.enabled = api.GetBool("TwitchControls", "Enabled", FALSE,
                                    "Master switch for Twitch channel-points controls. Requires OAuthToken/"
                                    "ClientId/BroadcasterUserId below to be set. Off by default.") != FALSE;

    char buf[512];
    api.GetString("TwitchControls", "OAuthToken", "",
                   "OAuth user access token with the channel:read:redemptions scope, obtained from an\n"
                   "externally registered Twitch application (e.g. via a token generator site). Pasted\n"
                   "manually -- this mod does not perform its own OAuth flow.",
                   buf, sizeof(buf));
    g_Config.oauthToken = buf;

    api.GetString("TwitchControls", "ClientId", "", "Client ID of the Twitch application the token above belongs to.",
                   buf, sizeof(buf));
    g_Config.clientId = buf;

    api.GetString("TwitchControls", "BroadcasterUserId", "",
                   "Numeric Twitch user id of the broadcaster whose channel-point redemptions to watch.", buf,
                   sizeof(buf));
    g_Config.broadcasterUserId = buf;

    int mappingCount = api.GetInt("TwitchControls", "MappingCount", 1,
                                   "Number of Reward<N>/Effect<N> pairs below (N from 1 to MappingCount).\n"
                                   "Reward<N> is the exact channel-points reward title as configured on your\n"
                                   "Twitch dashboard; Effect<N> is a registered mad2effects effect name (e.g.\n"
                                   "JoystickReversal, InvertColors).\n"
                                   "The trailing @indexed(...) line is a machine-readable spec mad2launcher's\n"
                                   "generic config.cfg editor parses to render an 'Add mapping' control that\n"
                                   "manages this count plus the matching Reward<N>/Effect<N> pairs, instead of\n"
                                   "a plain number field -- see mad2launcher/iniconfig.go.\n"
                                   "@indexed(Reward,Effect)");
    if (mappingCount < 0) mappingCount = 0;

    for (int i = 1; i <= mappingCount; ++i) {
        char rewardKey[32], effectKey[32];
        snprintf(rewardKey, sizeof(rewardKey), "Reward%d", i);
        snprintf(effectKey, sizeof(effectKey), "Effect%d", i);

        const char* defaultReward = (i == 1) ? "Reverse My Controls" : "";
        const char* defaultEffect = (i == 1) ? "JoystickReversal" : "";

        api.GetString("TwitchControls", rewardKey, defaultReward, "", buf, sizeof(buf));
        std::string reward = Trim(buf);
        api.GetString("TwitchControls", effectKey, defaultEffect, "", buf, sizeof(buf));
        std::string effect = Trim(buf);

        if (!reward.empty() && !effect.empty()) {
            g_Config.mappings.push_back({reward, effect});
        }
    }

    Log("Config loaded: enabled=%d clientId set=%d token set=%d broadcasterUserId=%s mappings=%zu\n",
        g_Config.enabled, !g_Config.clientId.empty(), !g_Config.oauthToken.empty(),
        g_Config.broadcasterUserId.c_str(), g_Config.mappings.size());
    for (const auto& m : g_Config.mappings) {
        Log("  Reward '%s' -> Effect '%s'\n", m.reward.c_str(), m.effect.c_str());
    }
}

// ---------------------------------------------------------------------
// Hand-rolled JSON substring scanning (see file header for why). Only
// handles the flat, unescaped-value cases this mod's fixed set of fields
// actually needs -- not a general JSON parser.
// ---------------------------------------------------------------------

// Finds "<key>":"<value>" and returns value (unescaped as-is, no \" or
// \uXXXX handling). Returns false if the key isn't present as a string
// value (e.g. it's null, a number, or absent).
static bool ExtractJsonString(const std::string& json, const char* key, std::string& outValue) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    size_t start = pos + needle.size();
    size_t end = json.find('"', start);
    if (end == std::string::npos) return false;
    outValue = json.substr(start, end - start);
    return true;
}

// ---------------------------------------------------------------------
// UTF-8 -> UTF-16 conversion (WinHTTP's wide-string APIs).
// ---------------------------------------------------------------------

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    w.resize(static_cast<size_t>(len) - 1);  // drop the counted NUL terminator
    return w;
}

// ---------------------------------------------------------------------
// EventSub subscription creation: plain HTTPS POST to api.twitch.tv,
// reusing the same WinHTTP primitives as the WebSocket connection (just
// without the upgrade). A fresh WinHttpOpen/Connect per call -- this is
// called once per successful session, not per frame, so the overhead
// doesn't matter.
// ---------------------------------------------------------------------

static bool CreateEventSubSubscription(const Config& cfg, const std::string& sessionId) {
    HINTERNET hSession =
        WinHttpOpen(L"Mad2TwitchControls/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        Log("CreateEventSubSubscription: WinHttpOpen failed: %lu\n", GetLastError());
        return false;
    }
    HINTERNET hConnect = WinHttpConnect(hSession, L"api.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        Log("CreateEventSubSubscription: WinHttpConnect failed: %lu\n", GetLastError());
        WinHttpCloseHandle(hSession);
        return false;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/helix/eventsub/subscriptions", NULL,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        Log("CreateEventSubSubscription: WinHttpOpenRequest failed: %lu\n", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::string body = "{\"type\":\"channel.channel_points_custom_reward_redemption.add\",\"version\":\"1\","
                        "\"condition\":{\"broadcaster_user_id\":\"" +
                        cfg.broadcasterUserId +
                        "\"},"
                        "\"transport\":{\"method\":\"websocket\",\"session_id\":\"" +
                        sessionId + "\"}}";

    std::wstring headers = L"Authorization: Bearer " + Utf8ToWide(cfg.oauthToken) + L"\r\nClient-Id: " +
                            Utf8ToWide(cfg.clientId) + L"\r\nContent-Type: application/json\r\n";

    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1),
                                    const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                    static_cast<DWORD>(body.size()), 0);
    bool ok = false;
    if (!sent) {
        Log("CreateEventSubSubscription: WinHttpSendRequest failed: %lu\n", GetLastError());
    } else if (!WinHttpReceiveResponse(hRequest, NULL)) {
        Log("CreateEventSubSubscription: WinHttpReceiveResponse failed: %lu\n", GetLastError());
    } else {
        DWORD statusCode = 0, size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &statusCode,
                             &size, NULL);

        std::string responseBody;
        BYTE readBuf[1024];
        DWORD bytesRead = 0;
        do {
            if (!WinHttpReadData(hRequest, readBuf, sizeof(readBuf), &bytesRead)) break;
            responseBody.append(reinterpret_cast<char*>(readBuf), bytesRead);
        } while (bytesRead > 0);

        ok = (statusCode == 202);
        Log("CreateEventSubSubscription: HTTP %lu%s\n", statusCode, ok ? "" : " -- response body follows");
        if (!ok) Log("  %s\n", responseBody.c_str());
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// ---------------------------------------------------------------------
// WebSocket connection lifecycle.
// ---------------------------------------------------------------------

struct WsHandles {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET websocket = nullptr;

    void Close() {
        if (websocket) {
            WinHttpWebSocketClose(websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
            WinHttpCloseHandle(websocket);
            websocket = nullptr;
        }
        if (connect) {
            WinHttpCloseHandle(connect);
            connect = nullptr;
        }
        if (session) {
            WinHttpCloseHandle(session);
            session = nullptr;
        }
    }
};

static bool ConnectEventSubWebSocket(WsHandles& ws) {
    ws.session = WinHttpOpen(L"Mad2TwitchControls/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ws.session) {
        Log("WinHttpOpen failed: %lu\n", GetLastError());
        return false;
    }
    ws.connect = WinHttpConnect(ws.session, L"eventsub.wss.twitch.tv", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!ws.connect) {
        Log("WinHttpConnect failed: %lu\n", GetLastError());
        ws.Close();
        return false;
    }
    HINTERNET hRequest = WinHttpOpenRequest(ws.connect, L"GET", L"/ws", NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        Log("WinHttpOpenRequest failed: %lu\n", GetLastError());
        ws.Close();
        return false;
    }
    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
        !WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL)) {
        Log("WebSocket upgrade request failed: %lu\n", GetLastError());
        WinHttpCloseHandle(hRequest);
        ws.Close();
        return false;
    }

    ws.websocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    WinHttpCloseHandle(hRequest);
    if (!ws.websocket) {
        Log("WinHttpWebSocketCompleteUpgrade failed: %lu\n", GetLastError());
        ws.Close();
        return false;
    }
    return true;
}

// Reads one full text message, accumulating fragments (WinHTTP delivers
// large messages as a sequence of FRAGMENT buffers followed by a final
// MESSAGE buffer) -- Twitch's EventSub payloads are small, but this
// handles the general case correctly either way. Returns NO_ERROR on a
// complete message, ERROR_HANDLE_EOF if the peer sent a close frame, or
// the WinHTTP error code on failure.
static DWORD ReceiveTextMessage(HINTERNET hWebSocket, std::string& outMessage) {
    outMessage.clear();
    BYTE buf[8192];
    for (;;) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;
        DWORD result = WinHttpWebSocketReceive(hWebSocket, buf, sizeof(buf), &bytesRead, &bufType);
        if (result != NO_ERROR) return result;
        if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return ERROR_HANDLE_EOF;

        outMessage.append(reinterpret_cast<char*>(buf), bytesRead);
        if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            return NO_ERROR;
        }
        // Otherwise a FRAGMENT buffer type -- keep looping to accumulate the rest.
    }
}

// ---------------------------------------------------------------------
// Reward -> effect dispatch.
// ---------------------------------------------------------------------

static bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

static void HandleRewardRedemption(const std::string& rewardTitle) {
    std::string trimmed = Trim(rewardTitle);
    for (const auto& mapping : g_Config.mappings) {
        if (!IEquals(mapping.reward, trimmed)) continue;

        const auto& effects = Mad2Effects_Resolve();
        if (!effects.Trigger) {
            Log("[Dependency] mad2effects.dll not found or missing exports -- cannot trigger '%s' for reward '%s'\n",
                mapping.effect.c_str(), trimmed.c_str());
            return;
        }
        BOOL ok = effects.Trigger(mapping.effect.c_str(), 0);
        Log("Reward '%s' -> Mad2Effects_Trigger(\"%s\") -> %d\n", trimmed.c_str(), mapping.effect.c_str(), ok);
        return;
    }
    Log("Redeemed reward '%s' has no configured effect mapping -- ignoring\n", trimmed.c_str());
}

// ---------------------------------------------------------------------
// Main network thread. Never gives up -- any failure at any stage closes
// everything and retries from the top after a short delay, mirroring
// mad2twitchchat's NetworkThreadFunc reconnect philosophy.
// ---------------------------------------------------------------------

static DWORD WINAPI NetworkThreadFunc(LPVOID) {
    LoadConfig();
    if (!g_Config.enabled) {
        Log("Twitch controls disabled (TwitchControls.Enabled=false); network thread exiting.\n");
        return 0;
    }
    if (g_Config.oauthToken.empty() || g_Config.clientId.empty() || g_Config.broadcasterUserId.empty()) {
        Log("TwitchControls.Enabled=true but OAuthToken/ClientId/BroadcasterUserId aren't all set -- "
            "network thread exiting. Fill in [TwitchControls] in config.cfg.\n");
        return 0;
    }

    // Resolve-with-retry for mad2effects, same reasoning as mad2chaosmod:
    // mad2/mods/*.dll load order isn't guaranteed, so this mod's DllMain-
    // spawned thread can start running before mad2effects.dll has loaded.
    const int kRetryDelayMs = 200;
    const int kWarnAfterAttempts = 25;  // ~5s
    int attempts = 0;
    bool warned = false;
    for (;;) {
        if (Mad2Effects_Resolve().Trigger != nullptr) break;
        ++attempts;
        if (!warned && attempts >= kWarnAfterAttempts) {
            warned = true;
            Log("[Dependency] still waiting on mad2effects (retrying indefinitely)\n");
        }
        Sleep(kRetryDelayMs);
    }

    for (;;) {
        Log("Connecting to Twitch EventSub...\n");
        WsHandles ws;
        if (!ConnectEventSubWebSocket(ws)) {
            Log("Connect failed, retrying in 5s...\n");
            Sleep(5000);
            continue;
        }

        std::string welcomeMsg;
        DWORD recvResult = ReceiveTextMessage(ws.websocket, welcomeMsg);
        if (recvResult != NO_ERROR) {
            Log("Failed to receive session_welcome: error=%lu\n", recvResult);
            ws.Close();
            Sleep(5000);
            continue;
        }

        std::string messageType, sessionId;
        if (!ExtractJsonString(welcomeMsg, "message_type", messageType) || messageType != "session_welcome" ||
            !ExtractJsonString(welcomeMsg, "id", sessionId)) {
            Log("Unexpected first message (expected session_welcome): %s\n", welcomeMsg.c_str());
            ws.Close();
            Sleep(5000);
            continue;
        }
        Log("Session established: %s\n", sessionId.c_str());

        bool subOk = CreateEventSubSubscription(g_Config, sessionId);
        if (!subOk) {
            Log("Subscription creation failed -- staying connected anyway (keepalives will still arrive; fix "
                "credentials/broadcaster id in config.cfg and this will retry on the next reconnect).\n");
        }

        bool reconnectRequested = false;
        for (;;) {
            std::string msg;
            DWORD result = ReceiveTextMessage(ws.websocket, msg);
            if (result != NO_ERROR) {
                Log("WebSocket receive error/closed: %lu\n", result);
                break;
            }

            std::string msgType;
            if (!ExtractJsonString(msg, "message_type", msgType)) continue;

            if (msgType == "session_reconnect") {
                // v1 simplification -- see file header: reconnect from scratch
                // rather than Twitch's fully seamless dual-socket handover.
                Log("Twitch requested a reconnect\n");
                reconnectRequested = true;
                break;
            } else if (msgType == "notification") {
                std::string title;
                if (ExtractJsonString(msg, "title", title)) {
                    HandleRewardRedemption(title);
                } else {
                    Log("notification message had no reward title field: %s\n", msg.c_str());
                }
            }
            // session_keepalive and anything else: no action needed.
        }

        ws.Close();
        Sleep(reconnectRequested ? 1000 : 2000);
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            HANDLE h = CreateThread(nullptr, 0, NetworkThreadFunc, nullptr, 0, nullptr);
            if (h) {
                CloseHandle(h);
            } else {
                Log("CreateThread for network thread failed: %lu\n", GetLastError());
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
