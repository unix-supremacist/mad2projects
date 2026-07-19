#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Two-channel audio engine, ported from ../mad2mod/src/main.cpp's
// AudioChannel/PlayAudio (that file used 3 slots -- music/podcast/SFX; this
// port only needs the two mad2music actually uses). Decode is via an exec'd
// `ffmpeg` subprocess piping raw PCM back over stdout -- same technique the
// original engine used, not a new dependency, since ffmpeg handles both the
// pool's Ogg/mp3/wav files and anything a user drops into `custom/`
// uniformly without mad2music needing its own per-format decoder.
enum AudioSlot {
    kAudioSlotMusic = 0,
    kAudioSlotPodcast = 1,
    kAudioSlotCount = 2,
};

struct AudioChannel {
    // Guards `filepath` -- written once from the main/caller thread right
    // before a decode thread starts, read from that decode thread AND from
    // StatusBroadcastLoop's polling thread (see main.cpp), so a plain
    // std::string here would be a data race between the two readers/writer.
    std::mutex filepathMutex;
    std::string filepath;
    SDL_AudioStream* stream = nullptr;
    SDL_AudioDeviceID dev = 0;
    std::thread thread;
    std::atomic<bool> active{false};
    std::atomic<bool> stopRequested{false};
    uint32_t generation = 0;
};

extern AudioChannel g_audioChannels[kAudioSlotCount];

// Thread-safe accessor for AudioChannel::filepath -- see its comment above.
std::string GetChannelFilepath(int slot);

// Starts playback of `filepath` on `slot`, detached. Returns false (and
// requests the current track to stop instead of starting a new one) if the
// slot is already playing -- callers should retry once the slot goes
// inactive, mirroring the original engine's polling-based callers
// (MusicPlayerThreadFunc's 500ms loop, the podcast trigger handler's
// completion watcher).
//
// While slot 0 (music) is playing, if slot 1 (podcast) is simultaneously
// active, slot 0's samples are ducked to 25% volume -- same fixed ducking
// factor as the original engine, applied per-chunk inside the decode loop
// rather than via a separate mixer stage.
bool PlayAudio(const std::string& filepath, int slot);

// Requests the given slot stop as soon as its decode loop next checks
// (non-blocking -- the slot's `active` flag goes false once its thread
// actually exits and drains).
void StopAudio(int slot);
