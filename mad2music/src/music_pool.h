#pragma once

#include <string>

#include "config.h"

// Runs forever (call on its own thread) -- ported from
// ../mad2mod/src/main.cpp's UpdateMusicPool + MusicPlayerThreadFunc. Polls
// every 500ms; whenever slot 0 (music) is idle, rescans
// <audioDir>/music/{copyright,copyright_lyrics,no_copyright,no_copyright_lyrics,custom,custom_lyrics}
// (gated by cfg.musicAllow*, same rules as the original: no_copyright is
// always scanned, the others need their matching Allow* flag) and starts a
// random allowed track. Stops slot 0 outright if cfg.musicEnabled is false,
// and skips/stops lyric tracks while a podcast is playing or lyrics are
// disallowed -- same behavior as the original, so a podcast never plays
// over a singalong track.
void MusicPlayerLoop(const std::string& audioDir, const Config& cfg);

// Picks a random file from <audioDir>/ai/podcast (recursive, .wav/.ogg/.mp3
// only -- same extension filter as the original's scan_directory_for_audio).
// Returns an empty string if the directory is missing or empty.
std::string PickRandomPodcastFile(const std::string& audioDir);
