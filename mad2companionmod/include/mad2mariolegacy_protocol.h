#ifndef MAD2MARIOLEGACY_PROTOCOL_H
#define MAD2MARIOLEGACY_PROTOCOL_H
// Wire protocol between mad2companionmod.dll (inside the Mad2 process, the
// "MarioLegacyChallenge" effect -- see ../src/mariolegacymod.cpp) and
// mad2relauncher (the native Linux process that actually launches/owns the
// vendored extern/mario-legacy `gk` process, see
// ../../mad2relauncher/mariolegacy.go). Sibling of mad2jak1_protocol.h --
// same two-packet-family/two-loopback-UDP-port shape, and (see below)
// actually wire-*identical* to it, but kept as its own header/magic/ports
// rather than literally reusing Jak1's so a stray packet from one effect can
// never be misread by the other's listener, and so the two effects can in
// principle run/launch independently without any shared state.
//
// Why wire-identical: extern/mario-legacy is itself a fork of jak-project
// (see this repo's extern/mario-legacy/data/goal_src/jak1/engine/ps2/pad.gc)
// -- Mario replaces Jak's *rendering and move logic* (mario.gc reads
// buttons via the same cpad-hold?/cpad-pressed? predicates against the same
// cpad-info struct every stock Jak1 process/target reads), but the
// underlying PS2 hw-cpad button/stick wire format service-cpads populates
// is completely untouched -- diffed byte-for-byte against this repo's own
// extern/jak-project/goal_src/jak1/engine/ps2/pad.gc and confirmed identical
// except for the injection patch itself. So the same
// XInputToPs2Pad-produced byte layout Jak1 uses is already exactly what
// Mario's moveset (including its SM64-derived extras like crouch/long-jump,
// which are just button *combinations* over the same face/shoulder buttons
// -- see mario.gc's own button-bitmask comment) expects; no new *mapping*
// was needed, only a second small C++ function (mariolegacymod.cpp's own
// XInputToPs2Pad, byte-for-byte the same logic as jak1mod.cpp's -- see that
// file's own comment for why duplicating it, rather than sharing one across
// translation units, matches this DLL's existing "SM64/Jak1 duplicate small
// platform shims rather than share" precedent).
//
// gk (this fork's runtime, statically linking libsm64 for Mario's actual
// SM64-derived movement/rendering -- confirmed via `strings gk` finding
// libsm64 log format strings) has no socket primitive of its own, same as
// stock jak-project's gk -- so the same "mad2relauncher is the actual UDP
// receiving endpoint, bridges into a polled file a GOAL patch reads every
// frame" design as Jak1 applies unchanged. See mad2jak1_protocol.h's file
// header for the fuller version of this explanation; only the two port
// numbers and magic values differ here.

#include <stdint.h>

#define MAD2MARIOLEGACY_DEFAULT_INPUT_PORT 47068
#define MAD2MARIOLEGACY_DEFAULT_CONTROL_PORT 47069

#define MAD2MARIOLEGACY_INPUT_MAGIC 0x4D314D32u    // "M2 1M" (Mad2 Mario1), input family
#define MAD2MARIOLEGACY_CONTROL_MAGIC 0x4D324D32u  // "M2 2M" variant, control family

// Identical bit layout to mad2jak1_protocol.h's MAD2JAK1_PAD_* -- see that
// header's comment (engine/ps2/pad.gc's pad-buttons bitfield, unmodified in
// this fork). Duplicated here (not #included from that header) so this file
// stays self-contained and MarioLegacyChallenge doesn't take a build
// dependency on JakChallenge's header for what's conceptually a shared PS2
// pad convention, not a Jak-specific one.
#define MAD2MARIOLEGACY_PAD_SELECT (1u << 0)
#define MAD2MARIOLEGACY_PAD_L3 (1u << 1)
#define MAD2MARIOLEGACY_PAD_R3 (1u << 2)
#define MAD2MARIOLEGACY_PAD_START (1u << 3)
#define MAD2MARIOLEGACY_PAD_UP (1u << 4)
#define MAD2MARIOLEGACY_PAD_RIGHT (1u << 5)
#define MAD2MARIOLEGACY_PAD_DOWN (1u << 6)
#define MAD2MARIOLEGACY_PAD_LEFT (1u << 7)
#define MAD2MARIOLEGACY_PAD_L2 (1u << 8)
#define MAD2MARIOLEGACY_PAD_R2 (1u << 9)
#define MAD2MARIOLEGACY_PAD_L1 (1u << 10)
#define MAD2MARIOLEGACY_PAD_R1 (1u << 11)
#define MAD2MARIOLEGACY_PAD_TRIANGLE (1u << 12)
#define MAD2MARIOLEGACY_PAD_CIRCLE (1u << 13)
#define MAD2MARIOLEGACY_PAD_X (1u << 14)
#define MAD2MARIOLEGACY_PAD_SQUARE (1u << 15)

#pragma pack(push, 1)
// Byte-identical layout to Mad2Jak1InputPacket -- see file header for why.
struct Mad2MarioLegacyInputPacket {
    uint32_t magic;  // MAD2MARIOLEGACY_INPUT_MAGIC
    uint32_t seq;    // monotonically increasing; receiver keeps only the newest
    uint16_t button0;
    uint8_t leftx, lefty;
    uint8_t rightx, righty;
};

enum Mad2MarioLegacyControlType {
    MAD2MARIOLEGACY_CONTROL_LAUNCH = 1,  // mod -> relauncher: start Mario Legacy, block Mad2 input, begin streaming
    MAD2MARIOLEGACY_CONTROL_STOP = 2,    // mod -> relauncher: kill it now (effect cleared/overridden away)
    MAD2MARIOLEGACY_CONTROL_EXITED = 3,  // relauncher -> mod: process exited on its own (player quit)
};

struct Mad2MarioLegacyControlPacket {
    uint32_t magic;  // MAD2MARIOLEGACY_CONTROL_MAGIC
    uint32_t type;   // one of Mad2MarioLegacyControlType
};
#pragma pack(pop)

#endif  // MAD2MARIOLEGACY_PROTOCOL_H
