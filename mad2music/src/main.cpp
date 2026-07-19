// mad2music: native Linux background-music + podcast playback engine,
// ported from ../mad2mod/src/main.cpp's audio subsystem. Forked and
// supervised (crash-relaunched) by mad2relauncher for the whole play
// session -- see ../mad2relauncher/music.go -- exactly like the game itself,
// not effect-gated. Background music (slot 0) loops continuously from the
// pool in audio_engine.cpp/music_pool.cpp; the only thing driven externally
// is the podcast interruption (slot 1), triggered by mad2podcastmod.dll
// sending a TRIGGER packet over loopback UDP -- see
// ../mad2podcastmod/include/mad2podcast_protocol.h for the wire format.
//
// Runs with cwd = the game install directory (mad2relauncher forks it
// without changing cwd -- see justfile's `_play` recipe), so `./config.cfg`
// and `mad2music.log` land in the same place every other mod's do. The
// audio pool itself lives outside any single game install (it's shared
// across mad2/mad2russia/mad2demo and would otherwise triple ~1.7GB of
// already-downloaded tracks) -- passed in via -audio-dir.
#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

#include "audio_engine.h"
#include "config.h"
#include "log.h"
#include "music_pool.h"
#include "platform_compat.h"

#include "../../mad2podcastmod/include/mad2podcast_protocol.h"

namespace {

// Copies `path`'s basename, extension stripped, into `outBuf` (NUL-
// terminated, truncated to fit) -- e.g. "/foo/hulelam.ogg" -> "hulelam".
// Empty `path` (slot never played anything yet) produces an empty string.
void CopyTrackName(char* outBuf, size_t outBufSize, const std::string& path) {
    std::string name = path.empty() ? "" : std::filesystem::path(path).stem().string();
    std::strncpy(outBuf, name.c_str(), outBufSize - 1);
    outBuf[outBufSize - 1] = '\0';
}

// Broadcasts a Mad2PodcastStatusPacket once a second, fire-and-forget, to
// whoever's listening on loopback (mad2musichud.dll, inside the Wine
// process) -- see mad2podcast_protocol.h's file header for why this is a
// broadcast rather than a request/reply. Runs forever -- call on its own
// thread.
void StatusBroadcastLoop(int statusPort) {
    int sock = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (sock < 0) {
        Log("[Status] Failed to create status broadcast socket\n");
        return;
    }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons(static_cast<uint16_t>(statusPort));

    for (;;) {
        Mad2PodcastStatusPacket pkt{};
        pkt.magic = MAD2PODCAST_STATUS_MAGIC;
        pkt.musicPlaying = g_audioChannels[kAudioSlotMusic].active.load() ? 1 : 0;
        pkt.podcastPlaying = g_audioChannels[kAudioSlotPodcast].active.load() ? 1 : 0;
        CopyTrackName(pkt.musicTrack, sizeof(pkt.musicTrack), GetChannelFilepath(kAudioSlotMusic));
        CopyTrackName(pkt.podcastTrack, sizeof(pkt.podcastTrack), GetChannelFilepath(kAudioSlotPodcast));
        sendto(sock, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        SDL_Delay(1000);
    }
}

// Watches slot 1 (podcast) until it goes idle, then replies FINISHED to
// whoever sent the TRIGGER that started it. Runs detached -- one per
// trigger, matched to that trigger's playback via `generation` so a
// re-trigger while one watcher is still waiting doesn't send a stray
// FINISHED for the wrong clip.
void WatchPodcastAndReply(uint32_t generation, sockaddr_in replyAddr) {
    while (g_audioChannels[kAudioSlotPodcast].active.load() &&
           g_audioChannels[kAudioSlotPodcast].generation == generation) {
        SDL_Delay(100);
    }
    if (g_audioChannels[kAudioSlotPodcast].generation != generation) {
        // Superseded by a newer trigger -- that watcher owns the reply now.
        return;
    }

    int sock = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (sock < 0) return;
    Mad2PodcastControlPacket pkt{MAD2PODCAST_CONTROL_MAGIC, MAD2PODCAST_CONTROL_FINISHED};
    sendto(sock, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&replyAddr), sizeof(replyAddr));
    PlatformCloseSocket(sock);
    Log("[Podcast] Playback finished, notified requester\n");
}

void HandleTrigger(const std::string& audioDir, sockaddr_in fromAddr) {
    std::string clip = PickRandomPodcastFile(audioDir);
    if (clip.empty()) {
        Log("[Podcast] TRIGGER received but no podcast clips found under %s/ai/podcast\n", audioDir.c_str());
        return;
    }
    if (!PlayAudio(clip, kAudioSlotPodcast)) {
        Log("[Podcast] TRIGGER received but slot busy -- asked current clip to stop, ignoring this trigger\n");
        return;
    }
    uint32_t gen = g_audioChannels[kAudioSlotPodcast].generation;
    std::thread(WatchPodcastAndReply, gen, fromAddr).detach();
}

std::string ArgValue(int argc, char** argv, const std::string& flag, const std::string& def) {
    for (int i = 1; i < argc - 1; ++i) {
        if (flag == argv[i]) return argv[i + 1];
    }
    return def;
}

}  // namespace

int main(int argc, char** argv) {
    InitLog();
    if (!PlatformSocketInit()) {
        Log("Platform socket init (WSAStartup) failed\n");
        return 1;
    }
    srand(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(PlatformGetPid()));

#ifdef _WIN32
    // Named kernel object, held for this process's entire lifetime (never
    // closed -- see below), so mad2podcastmod's native-Windows branch (see
    // ../../mad2podcastmod/src/podcastmod.cpp's
    // StartMad2MusicIfNeeded_NativeWindows) can tell "is mad2music currently
    // running" via a plain OpenMutexA rather than needing a PID file or
    // extra IPC round-trip. Also gives us free single-instance enforcement:
    // if the mutex already exists, another instance is already running
    // (and would otherwise fight this one for the same control/status UDP
    // ports), so this one exits immediately instead.
    HANDLE singleInstanceMutex = CreateMutexA(nullptr, FALSE, "Mad2Music_SingleInstanceMutex");
    if (singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("Another mad2music instance is already running -- exiting\n");
        return 0;
    }
#endif

    std::string audioDir = ArgValue(argc, argv, "-audio-dir", "./audio");
    Config cfg = LoadConfig();
    Log("mad2music starting (audioDir=%s, controlPort=%d, statusPort=%d, musicEnabled=%d)\n", audioDir.c_str(),
        cfg.controlPort, cfg.statusPort, cfg.musicEnabled);

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        Log("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    std::thread(MusicPlayerLoop, audioDir, cfg).detach();
    std::thread(StatusBroadcastLoop, cfg.statusPort).detach();

    int sock = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (sock < 0) {
        Log("Failed to create control socket\n");
        return 1;
    }
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bindAddr.sin_port = htons(static_cast<uint16_t>(cfg.controlPort));
    if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0) {
        Log("Failed to bind control socket on port %d\n", cfg.controlPort);
        return 1;
    }
    Log("Listening for podcast triggers on loopback UDP port %d\n", cfg.controlPort);

    for (;;) {
        Mad2PodcastControlPacket pkt{};
        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        // int, not ssize_t: recvfrom returns int on Windows and ssize_t on
        // POSIX -- these tiny control packets never approach INT_MAX either
        // way, so int is portable and sufficient (see platform_compat.h).
        int n = static_cast<int>(recvfrom(sock, reinterpret_cast<char*>(&pkt), sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&fromAddr), &fromLen));
        if (n != static_cast<int>(sizeof(pkt))) continue;
        if (pkt.magic != MAD2PODCAST_CONTROL_MAGIC) continue;

        if (pkt.type == MAD2PODCAST_CONTROL_TRIGGER) {
            Log("[Podcast] TRIGGER received\n");
            HandleTrigger(audioDir, fromAddr);
        }
    }
}
