#ifndef MAD2PODCAST_PROTOCOL_H
#define MAD2PODCAST_PROTOCOL_H
// Wire protocol between mad2podcastmod.dll (inside the Mad2 process, under
// Wine) and mad2music (a native Linux process, forked/supervised by
// mad2relauncher -- see ../mad2relauncher/music.go -- for the whole play
// session, not just while this effect is active). Unlike mad2sm64mod's
// protocol, there's no launch/exit handshake to model: mad2music is always
// running and already owns the whole continuous background-music loop
// (ported from ../mad2mod/src/main.cpp's MusicPlayerThreadFunc) on its own;
// this header only covers the one-shot "play a podcast clip now" trigger
// mad2podcastmod's chaos effect layers on top of that.
//
// Plain C, POD-only (no C++ features), same reasoning as mad2sm64_protocol.h
// even though both sides happen to be C++ here: keeps the header safe to
// widen later if a non-C++ side ever needs it.
//
// One packet family on one loopback UDP port (configurable -- see
// mad2config's [Podcast] section / mad2music's own -control-port flag, kept
// in sync by hand since the two processes don't share a config-loading
// mechanism):
//
//   - mad2podcastmod sends TRIGGER from an ephemeral local port when its
//     "PodcastBreak" effect fires. Fire-and-forget: mad2music picks a random
//     file from <audio-dir>/ai/podcast, plays it on audio slot 1 (ducking
//     the continuous music on slot 0, exactly like the original engine's
//     PlayAudio ducking logic), and once playback actually finishes, sends
//     FINISHED back to whatever address TRIGGER arrived from, so
//     mad2podcastmod knows to call Mad2Effects_ClearByName instead of
//     guessing a duration -- same "let the real side effect report back"
//     shape as mad2sm64mod's LAUNCH/EXITED handshake.
//
// No STOP message in v1: podcast clips are short (see ../mad2mod's existing
// clips under game/audio/ai/podcast, all well under a minute), so an early-
// cancel path wasn't worth the extra state. If that changes, add
// MAD2PODCAST_CONTROL_STOP the same way mad2sm64mod's protocol did.
//
// A second, independent packet family on a second loopback UDP port covers
// "now playing" status: mad2music broadcasts a Mad2PodcastStatusPacket once
// a second, fire-and-forget, to whoever's listening (today: mad2musichud,
// see ../../mad2musichud/src/musichud.cpp) so a D3D9 overlay inside the Wine
// process can show the current music/podcast track without mad2music having
// to know who's listening or how many listeners there are -- a plain
// broadcast is simpler than a request/reply here since "what's playing" is
// continuously-valid state, not a one-shot event like TRIGGER/FINISHED.

#include <stdint.h>

#define MAD2PODCAST_DEFAULT_CONTROL_PORT 47070
#define MAD2PODCAST_DEFAULT_STATUS_PORT 47071

#define MAD2PODCAST_CONTROL_MAGIC 0x50344D32u  // "M2P4" (Mad2 Podcast), control family
#define MAD2PODCAST_STATUS_MAGIC 0x53344D32u   // "M2S4"-ish, status family (distinct port, distinct magic)

#define MAD2PODCAST_TRACK_NAME_LEN 128

enum Mad2PodcastControlType {
    MAD2PODCAST_CONTROL_TRIGGER = 1,   // mod -> mad2music: play a random podcast clip now
    MAD2PODCAST_CONTROL_FINISHED = 2,  // mad2music -> mod: the triggered clip finished playing
};

#pragma pack(push, 1)
struct Mad2PodcastControlPacket {
    uint32_t magic;  // MAD2PODCAST_CONTROL_MAGIC
    uint32_t type;   // one of Mad2PodcastControlType
};

// musicTrack/podcastTrack are NUL-terminated basenames (no directory, no
// extension), truncated to fit -- empty string when that slot isn't
// playing. musicPlaying/podcastPlaying are the authoritative "should this
// line be shown at all" flags (a track name can still be present briefly
// after playback stops, since AudioChannel::filepath isn't cleared on
// stop -- see mad2music/src/audio_engine.cpp).
struct Mad2PodcastStatusPacket {
    uint32_t magic;  // MAD2PODCAST_STATUS_MAGIC
    uint8_t musicPlaying;
    uint8_t podcastPlaying;
    char musicTrack[MAD2PODCAST_TRACK_NAME_LEN];
    char podcastTrack[MAD2PODCAST_TRACK_NAME_LEN];
};
#pragma pack(pop)

#endif  // MAD2PODCAST_PROTOCOL_H
