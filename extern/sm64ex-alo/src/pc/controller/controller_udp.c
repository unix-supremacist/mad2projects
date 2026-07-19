// Injected-controller-state backend for the mad2modloader SM64 chaos
// effect (see ../../../../mad2sm64mod/src/sm64mod.cpp, several directories
// up from this vendored tree). Listens on a loopback UDP port for
// Mad2Sm64InputPacket datagrams carrying mad2xinput's POST-override
// controller state -- i.e. exactly what Mad2 itself would receive, chaos
// effects like JoystickReversal already applied -- and, whenever a packet
// has arrived recently, overwrites whatever the real SDL2 gamepad backend
// already wrote into `pad` this frame.
//
// `pkt.buttons` arrives pre-translated into N64's own CONT_* button bits
// (sm64mod.cpp's XInputToN64Buttons) -- this file does no button
// translation of its own, unlike controller_sdl2.c's configurable bind
// table, and just copies it straight into pad->button below.
//
// Registered in controller_entry_point.c's controller_implementations[]
// AFTER controller_sdl, specifically so injected input overwrites (rather
// than gets overwritten by) the real controller's contribution when both
// are present -- and, just as importantly, so the real controller keeps
// working completely unmodified whenever no fresh injected packet has
// arrived (e.g. SM64 launched standalone outside the mod chain, or the
// mad2sm64mod DLL's process died) -- see kFreshnessWindowMs below.
//
// The packet struct here is a deliberately-DUPLICATED copy of
// mad2sm64mod/include/mad2sm64_protocol.h's Mad2Sm64InputPacket, not a
// cross-tree #include -- this is vendored, patched third-party source
// (see this tree's own README for upstream), and a local copy keeps it
// self-contained if this tree is ever re-vendored from a newer upstream
// independently of mad2modloader's own source layout. Keep the two
// definitions in sync by hand; the wire format is small and stable.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#include <SDL2/SDL.h>

#include "controller_api.h"
#include "controller_udp.h"

#define MAD2SM64_DEFAULT_INPUT_PORT 47064
#define MAD2SM64_INPUT_MAGIC 0x53344D32u

#pragma pack(push, 1)
struct Mad2Sm64InputPacket {
    uint32_t magic;
    uint32_t seq;
    uint16_t buttons;
    int16_t lx, ly;
    int16_t rx, ry;
};
#pragma pack(pop)

static SOCKET sUdpSocket = INVALID_SOCKET;
static struct Mad2Sm64InputPacket sLatest;
static uint32_t sLatestSeq;
static int sHavePacket;
static Uint32 sLastPacketTicksMs;

// How long a received packet stays "current" before this backend gives up
// and lets the real SDL2 gamepad reading (already written into `pad`
// before this backend's read() runs) stand -- must not get in the way when
// mad2sm64mod isn't actively streaming.
static const Uint32 kFreshnessWindowMs = 250;

static void controller_udp_init(void) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    int port = MAD2SM64_DEFAULT_INPUT_PORT;
    const char* portEnv = getenv("MAD2SM64_INPUT_PORT");
    if (portEnv && portEnv[0]) {
        int parsed = atoi(portEnv);
        if (parsed > 0 && parsed < 65536) port = parsed;
    }

    sUdpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sUdpSocket == INVALID_SOCKET) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(sUdpSocket, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(sUdpSocket);
        sUdpSocket = INVALID_SOCKET;
        return;
    }

#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(sUdpSocket, FIONBIO, &nonBlocking);
#else
    int flags = fcntl(sUdpSocket, F_GETFL, 0);
    fcntl(sUdpSocket, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void controller_udp_read(OSContPad* pad) {
    if (sUdpSocket == INVALID_SOCKET) return;

    // Drain every pending datagram, keeping only the newest by sequence
    // number -- staleness/reordering on loopback UDP is harmless since
    // only the latest sample matters.
    for (;;) {
        struct Mad2Sm64InputPacket pkt;
        int n = recv(sUdpSocket, (char*)&pkt, sizeof(pkt), 0);
        if (n != (int)sizeof(pkt)) break;
        if (pkt.magic != MAD2SM64_INPUT_MAGIC) continue;
        if (sHavePacket && pkt.seq <= sLatestSeq) continue;
        sLatest = pkt;
        sLatestSeq = pkt.seq;
        sHavePacket = 1;
        sLastPacketTicksMs = SDL_GetTicks();
    }

    if (!sHavePacket) return;
    if (SDL_GetTicks() - sLastPacketTicksMs > kFreshnessWindowMs) return;  // stale -- fall back to controller_sdl's read

    // Overwrite (not OR/merge) -- this backend runs after controller_sdl in
    // controller_implementations[], so a fresh injected packet should
    // fully replace the real pad's contribution for this frame.
    pad->button = sLatest.buttons;
    pad->stick_x = (s8)(sLatest.lx / 409);
    pad->stick_y = (s8)(sLatest.ly / 409);
    pad->ext_stick_x = (s8)(sLatest.rx / 409);
    pad->ext_stick_y = (s8)(sLatest.ry / 409);
}

static u32 controller_udp_rawkey(void) {
    return VK_INVALID;  // no bindable virtual keys -- injected state doesn't use the binding system
}

static void controller_udp_shutdown(void) {
    if (sUdpSocket != INVALID_SOCKET) {
        closesocket(sUdpSocket);
        sUdpSocket = INVALID_SOCKET;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

struct ControllerAPI controller_udp = {
    0,  // vkbase -- irrelevant, rawkey() always returns VK_INVALID
    controller_udp_init,
    controller_udp_read,
    controller_udp_rawkey,
    NULL,  // rumble_play
    NULL,  // rumble_stop
    NULL,  // reconfig
    controller_udp_shutdown
};
