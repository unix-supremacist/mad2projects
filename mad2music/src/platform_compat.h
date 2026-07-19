#pragma once

// Small POSIX/Windows shim, isolated to one header, so main.cpp/log.cpp/
// audio_engine.cpp's actual logic doesn't get cluttered with #ifdefs.
// mad2music is native Linux by default (see CMakeLists.txt); the Windows
// branch below is for `just build-music-windows`'s i686-w64-mingw32
// cross-build, used on native-Windows Mad2 installs where there is no
// mad2relauncher to fork/supervise this process -- see mad2podcastmod's
// native-Windows CreateProcess-if-not-running startup step, and CLAUDE.md's
// "mad2music on native Windows" section.
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>  // _getpid

// ws2tcpip.h already provides socklen_t.
using PlatformPid = int;

inline bool PlatformSocketInit() {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
}
inline void PlatformSocketCleanup() { WSACleanup(); }
inline void PlatformCloseSocket(int sock) { closesocket(static_cast<SOCKET>(sock)); }
inline PlatformPid PlatformGetPid() { return _getpid(); }

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using PlatformPid = pid_t;

inline bool PlatformSocketInit() { return true; }  // no-op -- POSIX sockets need no global init
inline void PlatformSocketCleanup() {}
inline void PlatformCloseSocket(int sock) { close(sock); }
inline PlatformPid PlatformGetPid() { return getpid(); }

#endif
