#pragma once

// Thin Winsock-shaped shim so the lobby server builds for Linux without the
// body of the server changing. The server is a plain UDP forwarder, so the only
// Windows-isms it relies on are the socket type/teardown spellings, a
// millisecond clock, a sleep, and a Ctrl-C hook -- all of which have direct
// POSIX equivalents.
//
// Hosting a Windows VPS purely to run a UDP relay costs several times what a
// Linux box does, which is the whole reason this file exists.

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef WSAPOLLFD PollEntry;
const short POLL_READABLE = POLLRDNORM;
inline int PollSockets(PollEntry* entries, unsigned long count, int timeoutMs) {
	return WSAPoll(entries, count, timeoutMs);
}

#else

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>

// Winsock spells a socket handle SOCKET and an invalid one INVALID_SOCKET; on
// POSIX it is just a file descriptor, and failure is negative.
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)

typedef uint64_t ULONGLONG;
typedef unsigned long u_long;

inline int closesocket(SOCKET s) {
	return ::close(s);
}

// Winsock needs explicit startup/teardown; POSIX sockets do not.
inline int WSAStartup(uint16_t, void*) { return 0; }
inline int WSACleanup() { return 0; }

// Only ever used here to set non-blocking mode (FIONBIO), which POSIX does
// through fcntl. Anything else is not supported on purpose rather than being
// silently mistranslated.
#ifndef FIONBIO
#define FIONBIO 0x5421
#endif
inline int ioctlsocket(SOCKET s, long cmd, u_long* argp) {
	if (cmd != FIONBIO || argp == nullptr) {
		errno = EINVAL;
		return SOCKET_ERROR;
	}
	int flags = ::fcntl(s, F_GETFL, 0);
	if (flags < 0) {
		return SOCKET_ERROR;
	}
	flags = (*argp != 0) ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return ::fcntl(s, F_SETFL, flags) < 0 ? SOCKET_ERROR : 0;
}

// Milliseconds since an arbitrary fixed point. MONOTONIC so that an NTP step
// or a DST change cannot make a timeout fire early, late, or never -- this
// clock drives the relay and lobby timeouts.
inline ULONGLONG GetTickCount64() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)(ts.tv_nsec / 1000000);
}

typedef struct pollfd PollEntry;
const short POLL_READABLE = POLLIN;
inline int PollSockets(PollEntry* entries, unsigned long count, int timeoutMs) {
	return ::poll(entries, count, timeoutMs);
}

inline void Sleep(unsigned int ms) {
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, nullptr);
}

#endif // _WIN32
