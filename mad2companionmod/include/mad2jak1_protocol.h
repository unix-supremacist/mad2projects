#ifndef MAD2JAK1_PROTOCOL_H
#define MAD2JAK1_PROTOCOL_H
// Wire protocol between mad2sm64mod.dll (inside the Mad2 process, the
// "JakChallenge" effect -- see ../src/jak1mod.cpp) and mad2relauncher (the
// native Linux process that actually launches/owns the Jak & Daxter
// (OpenGOAL "gk") process, see ../../mad2relauncher/jak1.go). Sibling of
// mad2sm64_protocol.h, same two-packet-family/two-loopback-UDP-port shape,
// but NOT wire-compatible with it -- distinct magic values and ports so a
// stray packet from one effect can never be misread by the other's
// listener, even though both mad2sm64mod effects can in principle be
// triggered around the same time.
//
// The one real difference from the SM64 side: SM64Challenge streams straight
// through to the game process itself, because extern/sm64ex-alo is vendored
// source we patched (controller_udp.c) to receive UDP directly. There is no
// equivalent jak-project C++ source here -- ~/src/jak1nolauncher/gk is a
// prebuilt binary -- so gk cannot be a UDP endpoint. Only its GOAL *script*
// source (data/goal_src/jak1, compiled by goalc) is available to us, and
// GOAL has no socket primitive exposed by the kernel's symbol table. So the
// last inch into gk is a small file mad2relauncher overwrites (atomically:
// write-temp-then-rename, so a GOAL-side reader never sees a torn write)
// every time it receives a Mad2Jak1InputPacket, which a hand-written patch
// in engine/ps2/pad.gc's service-cpads polls once per frame and copies
// directly onto the raw cpad-info fields -- see that file's patch comment
// for the receiving half of this contract.
//
// Because that GOAL patch is meant to stay a handful of `set!` lines (no
// XInput-to-PS2-pad translation logic in GOAL), Mad2Jak1InputPacket is
// **already** in the PS2 hw-cpad wire format (engine/ps2/pad.gc's
// `pad-buttons` bit layout, stick bytes 0-255 with 128 = center) rather than
// raw XInput fields -- mad2sm64mod's jak1mod.cpp does that translation in
// C++ before sending. This is the opposite of controller_udp.c's approach
// (which receives raw XInput and decodes it in C on the receiving side),
// forced by not owning the receiving side's source here.
//
// Both packet structs start with `magic` as a cheap sanity check against
// stray localhost traffic; receivers must drop anything that doesn't match
// before touching the rest of the packet.

#include <stdint.h>

#define MAD2JAK1_DEFAULT_INPUT_PORT 47066
#define MAD2JAK1_DEFAULT_CONTROL_PORT 47067

#define MAD2JAK1_INPUT_MAGIC 0x4A314D32u    // "M2 1J" (Mad2 Jak1), input family
#define MAD2JAK1_CONTROL_MAGIC 0x4A324D32u  // "M2 2J" variant, control family

// engine/ps2/pad.gc's (defenum pad-buttons :bitfield #t :type uint32 ...)
// bit indices -- kept as a hand-written duplicate here (like every other
// wire-format constant in this repo, see mad2sm64_protocol.h's own header
// comment) since one side is C++ and the other is GOAL script; there is no
// shared-source mechanism across that boundary. jak1mod.cpp sets these bits
// directly when translating from XINPUT_GAMEPAD's wButtons/trigger bytes.
#define MAD2JAK1_PAD_SELECT (1u << 0)
#define MAD2JAK1_PAD_L3 (1u << 1)
#define MAD2JAK1_PAD_R3 (1u << 2)
#define MAD2JAK1_PAD_START (1u << 3)
#define MAD2JAK1_PAD_UP (1u << 4)
#define MAD2JAK1_PAD_RIGHT (1u << 5)
#define MAD2JAK1_PAD_DOWN (1u << 6)
#define MAD2JAK1_PAD_LEFT (1u << 7)
#define MAD2JAK1_PAD_L2 (1u << 8)
#define MAD2JAK1_PAD_R2 (1u << 9)
#define MAD2JAK1_PAD_L1 (1u << 10)
#define MAD2JAK1_PAD_R1 (1u << 11)
#define MAD2JAK1_PAD_TRIANGLE (1u << 12)
#define MAD2JAK1_PAD_CIRCLE (1u << 13)
#define MAD2JAK1_PAD_X (1u << 14)
#define MAD2JAK1_PAD_SQUARE (1u << 15)

#pragma pack(push, 1)
// Already PS2-native (see file header): button0 is a pad-buttons bitmask
// (MAD2JAK1_PAD_* above, bit SET = pressed -- note hw-cpad's raw button0 on
// real hardware is active-LOW, but engine/ps2/pad.gc's service-cpads never
// sees that polarity directly on PC -- cpad-get-data's PC-port
// implementation already normalizes it to active-HIGH before GOAL code
// touches it, which is what our injection point downstream of
// (cpad-get-data pad) must match). leftx/lefty/rightx/righty are 0-255,
// 128 = centered, matching hw-cpad's stick byte convention exactly.
struct Mad2Jak1InputPacket {
    uint32_t magic;  // MAD2JAK1_INPUT_MAGIC
    uint32_t seq;    // monotonically increasing; receiver keeps only the newest
    uint16_t button0;
    uint8_t leftx, lefty;
    uint8_t rightx, righty;
};

enum Mad2Jak1ControlType {
    MAD2JAK1_CONTROL_LAUNCH = 1,  // mod -> relauncher: start Jak1, block Mad2 input, begin streaming
    MAD2JAK1_CONTROL_STOP = 2,    // mod -> relauncher: kill Jak1 now (effect cleared/overridden away)
    MAD2JAK1_CONTROL_EXITED = 3,  // relauncher -> mod: Jak1's process exited on its own (either the
                                   // player quit, or engine/game/game-info.gc's own -level/*kernel-boot-level*
                                   // auto-(kernel-shutdown)-on-power-cell patch fired)
};

struct Mad2Jak1ControlPacket {
    uint32_t magic;  // MAD2JAK1_CONTROL_MAGIC
    uint32_t type;   // one of Mad2Jak1ControlType
};
#pragma pack(pop)

#endif  // MAD2JAK1_PROTOCOL_H
