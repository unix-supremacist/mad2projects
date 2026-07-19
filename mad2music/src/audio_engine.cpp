#include "audio_engine.h"

#include <cstdio>

#include "log.h"

AudioChannel g_audioChannels[kAudioSlotCount];

bool PlayAudio(const std::string& filepath, int slot) {
    if (slot < 0 || slot >= kAudioSlotCount) {
        Log("[Audio] Invalid slot %d\n", slot);
        return false;
    }

    // Slot busy -- ask it to stop, caller retries later (same contract as
    // the original engine: this call does not block waiting for the old
    // track to finish).
    if (g_audioChannels[slot].active.load()) {
        g_audioChannels[slot].stopRequested.store(true);
        return false;
    }

    if (g_audioChannels[slot].thread.joinable()) {
        g_audioChannels[slot].thread.detach();
    }

    uint32_t gen = ++g_audioChannels[slot].generation;
    {
        std::lock_guard<std::mutex> lock(g_audioChannels[slot].filepathMutex);
        g_audioChannels[slot].filepath = filepath;
    }
    g_audioChannels[slot].active.store(true);
    g_audioChannels[slot].stopRequested.store(false);

    g_audioChannels[slot].thread = std::thread([slot, filepath, gen]() {
        SDL_AudioSpec srcSpec;
        srcSpec.format = SDL_AUDIO_S16LE;
        srcSpec.channels = 2;
        srcSpec.freq = 44100;

        SDL_AudioDeviceID dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (dev == 0) {
            Log("[Audio] Failed to open audio device: %s\n", SDL_GetError());
            if (g_audioChannels[slot].generation == gen) g_audioChannels[slot].active.store(false);
            return;
        }
        if (g_audioChannels[slot].generation == gen) g_audioChannels[slot].dev = dev;

        SDL_AudioStream* stream = SDL_CreateAudioStream(&srcSpec, nullptr);
        if (!stream) {
            Log("[Audio] Failed to create audio stream: %s\n", SDL_GetError());
            SDL_CloseAudioDevice(dev);
            if (g_audioChannels[slot].generation == gen) g_audioChannels[slot].active.store(false);
            return;
        }
        if (g_audioChannels[slot].generation == gen) g_audioChannels[slot].stream = stream;

        SDL_BindAudioStream(dev, stream);
        SDL_ResumeAudioDevice(dev);

        std::string cmd = "ffmpeg -y -loglevel quiet -i \"" + filepath + "\" -f s16le -ac 2 -ar 44100 -";
#ifdef _WIN32
        // Same ffmpeg-subprocess-piping-PCM technique as POSIX -- just the
        // MSVCRT-spelled names (identical semantics). Requires ffmpeg.exe on
        // PATH on Windows too, same as the Linux build's ffmpeg dependency.
        FILE* pipe = _popen(cmd.c_str(), "rb");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe) {
            Log("[Audio] Failed to open ffmpeg pipe for %s\n", filepath.c_str());
            SDL_DestroyAudioStream(stream);
            SDL_CloseAudioDevice(dev);
            if (g_audioChannels[slot].generation == gen) g_audioChannels[slot].active.store(false);
            return;
        }

        char buf[8192];
        while (!g_audioChannels[slot].stopRequested.load() && g_audioChannels[slot].generation == gen) {
            size_t n = fread(buf, 1, sizeof(buf), pipe);
            if (n == 0) break;

            // Duck music (slot 0) while a podcast (slot 1) is active -- same
            // fixed 0.25x factor as the original engine.
            if (slot == kAudioSlotMusic && g_audioChannels[kAudioSlotPodcast].active.load()) {
                int16_t* samples = reinterpret_cast<int16_t*>(buf);
                size_t numSamples = n / 2;
                for (size_t i = 0; i < numSamples; ++i) {
                    samples[i] = static_cast<int16_t>(samples[i] * 0.25f);
                }
            }

            SDL_PutAudioStreamData(stream, buf, static_cast<int>(n));

            while (SDL_GetAudioStreamQueued(stream) > 176400 && !g_audioChannels[slot].stopRequested.load() &&
                   g_audioChannels[slot].generation == gen) {
                SDL_Delay(50);
            }
        }

#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif

        SDL_FlushAudioStream(stream);
        while (SDL_GetAudioStreamQueued(stream) > 0 && !g_audioChannels[slot].stopRequested.load() &&
               g_audioChannels[slot].generation == gen) {
            SDL_Delay(50);
        }

        SDL_DestroyAudioStream(stream);
        SDL_CloseAudioDevice(dev);

        if (g_audioChannels[slot].generation == gen) {
            g_audioChannels[slot].stream = nullptr;
            g_audioChannels[slot].dev = 0;
            g_audioChannels[slot].active.store(false);
        }
    });

    g_audioChannels[slot].thread.detach();
    Log("[Audio] Playing %s on slot %d\n", filepath.c_str(), slot);
    return true;
}

void StopAudio(int slot) {
    if (slot < 0 || slot >= kAudioSlotCount) return;
    g_audioChannels[slot].stopRequested.store(true);
}

std::string GetChannelFilepath(int slot) {
    if (slot < 0 || slot >= kAudioSlotCount) return "";
    std::lock_guard<std::mutex> lock(g_audioChannels[slot].filepathMutex);
    return g_audioChannels[slot].filepath;
}
