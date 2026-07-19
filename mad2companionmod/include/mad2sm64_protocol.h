#ifndef MAD2SM64_PROTOCOL_H
#define MAD2SM64_PROTOCOL_H
// Wire protocol between mad2sm64mod.dll (inside the Mad2 process) and
// whichever process ends up actually running SM64 -- either the Go
// relauncher (mad2relauncher, which forks a native Linux sm64ex-alo binary
// when Mad2 is running under Wine) or, on native Windows, a Windows
// sm64ex-alo build launched directly by mad2sm64mod itself via
// CreateProcess. Plain C, POD-only (no C++ features) so this same header
// can be included both by mad2sm64mod's C++ source and by the new
// controller_udp.c backend added to the vendored extern/sm64ex-alo/ (a
// plain C codebase) -- see extern/sm64ex-alo/src/pc/controller/controller_udp.c.
//
// Two independent packet families, on two independent loopback UDP ports
// (both configurable, see mad2config's [SM64] section / mad2relauncher's
// own flags -- the two processes don't share a config file, see
// mad2sm64mod's file header for why):
//
//   - Mad2Sm64InputPacket, sent one-way and continuously (mad2sm64mod's
//     streaming thread -> SM64's controller_udp backend) on the *input*
//     port, carrying mad2xinput's post-override controller state so chaos
//     effects like JoystickReversal apply inside SM64 too. Fire-and-forget:
//     no ack, no retry -- only the newest `seq` the receiver has seen
//     matters, so a dropped or reordered packet is a non-event.
//
//   - Mad2Sm64ControlPacket, sent on the *control* port. Only meaningful
//     between mad2sm64mod and mad2relauncher (the Wine branch) -- native
//     Windows has no separate launcher process and never uses this family.
//     mad2sm64mod sends LAUNCH/STOP from an ephemeral local port;
//     mad2relauncher replies with EXITED to that same source address once
//     the SM64 child it launched exits, so mad2sm64mod knows to drop its
//     input-block override and stop streaming without having to poll a
//     process handle it doesn't own.
//
// Both packet structs start with `magic` as a cheap sanity check against
// stray traffic on localhost; receivers must drop anything that doesn't
// match before touching the rest of the packet.

#include <stdint.h>

#define MAD2SM64_DEFAULT_INPUT_PORT 47064
#define MAD2SM64_DEFAULT_CONTROL_PORT 47065

#define MAD2SM64_INPUT_MAGIC 0x53344D32u    // "M2S4" (Mad2 SM64), input family
#define MAD2SM64_CONTROL_MAGIC 0x43344D32u  // "M2S4" variant, control family

// Injected controller state has priority over SM64's own SDL2 gamepad
// reading (see controller_udp.c), but must fall back to it cleanly if this
// stops arriving -- e.g. SM64 launched standalone outside the mod chain, or
// the mod process died. The receiver enforces this via a freshness window
// on the last-received packet's arrival time, not via anything in the wire
// format itself.
#pragma pack(push, 1)
struct Mad2Sm64InputPacket {
    uint32_t magic;   // MAD2SM64_INPUT_MAGIC
    uint32_t seq;     // monotonically increasing; receiver keeps only the newest
    // Already translated to PR/os_cont.h's N64 CONT_* bitmask by
    // sm64mod.cpp's XInputToN64Buttons -- NOT a raw XINPUT_GAMEPAD wButtons
    // passthrough (an earlier version of this packet was, which was a real
    // bug: XInput's bit positions don't line up with N64's at almost any
    // bit, so every button was effectively scrambled). controller_udp.c's
    // receiving side just copies this straight into pad->button with no
    // translation of its own -- see XInputToN64Buttons's own comment for
    // the full mapping and why it lives on the sending side.
    uint16_t buttons;
    int16_t lx, ly;   // post-override left stick, XInput range (-32768..32767)
    int16_t rx, ry;   // post-override right stick, XInput range
};

enum Mad2Sm64ControlType {
    MAD2SM64_CONTROL_LAUNCH = 1,  // mod -> relauncher: start SM64, block Mad2 input, begin streaming
    MAD2SM64_CONTROL_STOP = 2,    // mod -> relauncher: kill SM64 now (effect cleared/overridden away)
    MAD2SM64_CONTROL_EXITED = 3,  // relauncher -> mod: SM64's process exited on its own
};

struct Mad2Sm64ControlPacket {
    uint32_t magic;  // MAD2SM64_CONTROL_MAGIC
    uint32_t type;   // one of Mad2Sm64ControlType
};
#pragma pack(pop)

#endif  // MAD2SM64_PROTOCOL_H
