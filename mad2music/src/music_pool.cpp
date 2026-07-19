#include "music_pool.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "audio_engine.h"
#include "log.h"

namespace fs = std::filesystem;

namespace {

bool IsAudioExt(const std::string& ext) {
    return ext == ".ogg" || ext == ".mp3" || ext == ".wav";
}

std::string LowerExt(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

struct Track {
    std::string path;
    bool hasLyrics;
};

void ScanDir(const std::string& path, bool hasLyrics, std::vector<Track>& out) {
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) return;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && IsAudioExt(LowerExt(entry.path()))) {
            out.push_back({entry.path().string(), hasLyrics});
        }
    }
}

std::vector<Track> BuildMusicPool(const std::string& audioDir, const Config& cfg) {
    std::vector<Track> pool;
    std::string musicDir = audioDir + "/music";

    if (cfg.musicAllowCopyright) {
        ScanDir(musicDir + "/copyright", false, pool);
        if (cfg.musicAllowLyrics) ScanDir(musicDir + "/copyright_lyrics", true, pool);
    }
    // Always scanned, same as the original engine -- game-owned/no-copyright
    // score is always in the rotation regardless of the copyright toggle.
    ScanDir(musicDir + "/no_copyright", false, pool);
    if (cfg.musicAllowLyrics) ScanDir(musicDir + "/no_copyright_lyrics", true, pool);

    if (cfg.musicAllowCustom) {
        ScanDir(musicDir + "/custom", false, pool);
        if (cfg.musicAllowLyrics) ScanDir(musicDir + "/custom_lyrics", true, pool);
    }

    return pool;
}

}  // namespace

std::string PickRandomPodcastFile(const std::string& audioDir) {
    std::vector<std::string> files;
    std::string podcastDir = audioDir + "/ai/podcast";

    std::error_code ec;
    if (!fs::exists(podcastDir, ec) || !fs::is_directory(podcastDir, ec)) return "";
    for (const auto& entry : fs::recursive_directory_iterator(podcastDir, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && IsAudioExt(LowerExt(entry.path()))) {
            files.push_back(entry.path().string());
        }
    }
    if (files.empty()) return "";
    return files[static_cast<size_t>(rand()) % files.size()];
}

void MusicPlayerLoop(const std::string& audioDir, const Config& cfg) {
    bool currentHasLyrics = false;

    for (;;) {
        SDL_Delay(500);

        if (!cfg.musicEnabled) {
            StopAudio(kAudioSlotMusic);
            continue;
        }

        bool podcastPlaying = g_audioChannels[kAudioSlotPodcast].active.load();

        if (g_audioChannels[kAudioSlotMusic].active.load()) {
            if (podcastPlaying && currentHasLyrics) {
                Log("[Music] Podcast playing and current track has lyrics -- stopping\n");
                StopAudio(kAudioSlotMusic);
            } else if (!cfg.musicAllowLyrics && currentHasLyrics) {
                Log("[Music] Lyrics disabled in config -- stopping current track\n");
                StopAudio(kAudioSlotMusic);
            }
            continue;
        }

        std::vector<Track> pool = BuildMusicPool(audioDir, cfg);
        if (pool.empty()) continue;

        std::vector<Track> allowed;
        for (const auto& track : pool) {
            if (podcastPlaying && track.hasLyrics) continue;  // skip lyrics during podcasts
            allowed.push_back(track);
        }
        if (allowed.empty()) continue;

        const Track& picked = allowed[static_cast<size_t>(rand()) % allowed.size()];
        currentHasLyrics = picked.hasLyrics;
        Log("[Music] Playing track: %s (lyrics: %s)\n", picked.path.c_str(), currentHasLyrics ? "yes" : "no");
        PlayAudio(picked.path, kAudioSlotMusic);
    }
}
