// Host sockets for the game's own network stack (runtime/services/bsd.c, ssl.c): the few things POSIX and Winsock
// spell differently, and errno numbers as the game expects them (FreeBSD's, as Horizon's bsd service reports them).
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET hsock_t;
#define HSOCK_BAD INVALID_SOCKET
#define hsock_close closesocket
typedef WSAPOLLFD hpollfd;
static inline int hsock_poll(hpollfd *f, unsigned n, int ms) { return WSAPoll(f, n, ms); }
static inline int hsock_errno(void) { return WSAGetLastError(); }
static inline void hsock_startup(void) {
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
}
static inline void hsock_nonblock(hsock_t s) { u_long v = 1; ioctlsocket(s, FIONBIO, &v); }
typedef int hsocklen_t;
#define HSOCK_IOLEN(n) ((int)(n))
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int hsock_t;
#define HSOCK_BAD (-1)
#define hsock_close close
typedef struct pollfd hpollfd;
static inline int hsock_poll(hpollfd *f, unsigned n, int ms) { return poll(f, (nfds_t)n, ms); }
static inline int hsock_errno(void) { return errno; }
static inline void hsock_startup(void) {}
static inline void hsock_nonblock(hsock_t s) {
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}
typedef socklen_t hsocklen_t;
#define HSOCK_IOLEN(n) (n)
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// The errno numbers the game's socket library expects. They are Linux's, not FreeBSD's: the Switch's socket errno
// enum follows Linux (EAGAIN 11, EINPROGRESS 115, EISCONN 106 ...). SM35's own socket wrapper tests the value after
// a failed connect against 115 for "in progress" and 11 / 100 / 9 after a close: with FreeBSD's 36 it never saw
// its relay connection as pending, and connected to Nextendo's match relay again every frame until it gave up.
enum {
    BSD_EPERM = 1, BSD_ENOENT = 2, BSD_EINTR = 4, BSD_EIO = 5, BSD_EBADF = 9, BSD_EAGAIN = 11, BSD_ENOMEM = 12,
    BSD_EACCES = 13, BSD_EFAULT = 14, BSD_EBUSY = 16, BSD_EINVAL = 22, BSD_EMFILE = 24, BSD_EPIPE = 32,
    BSD_ENOTSOCK = 88, BSD_EDESTADDRREQ = 89, BSD_EMSGSIZE = 90, BSD_EPROTOTYPE = 91, BSD_ENOPROTOOPT = 92,
    BSD_EPROTONOSUPPORT = 93, BSD_EOPNOTSUPP = 95, BSD_EAFNOSUPPORT = 97, BSD_EADDRINUSE = 98,
    BSD_EADDRNOTAVAIL = 99, BSD_ENETDOWN = 100, BSD_ENETUNREACH = 101, BSD_ENETRESET = 102, BSD_ECONNABORTED = 103,
    BSD_ECONNRESET = 104, BSD_ENOBUFS = 105, BSD_EISCONN = 106, BSD_ENOTCONN = 107, BSD_ESHUTDOWN = 108,
    BSD_ETIMEDOUT = 110, BSD_ECONNREFUSED = 111, BSD_EHOSTDOWN = 112, BSD_EHOSTUNREACH = 113, BSD_EALREADY = 114,
    BSD_EINPROGRESS = 115,
};

static inline bool hsock_would_block(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS;
#endif
}

// a host errno (errno / WSAGetLastError) -> FreeBSD's number
static inline uint32_t hsock_bsd_errno(int e) {
#ifdef _WIN32
    switch (e) {
    case WSAEWOULDBLOCK: return BSD_EAGAIN;
    case WSAEINPROGRESS: return BSD_EINPROGRESS;
    case WSAEALREADY: return BSD_EALREADY;
    case WSAENOTSOCK: return BSD_ENOTSOCK;
    case WSAEDESTADDRREQ: return BSD_EDESTADDRREQ;
    case WSAEMSGSIZE: return BSD_EMSGSIZE;
    case WSAEPROTOTYPE: return BSD_EPROTOTYPE;
    case WSAENOPROTOOPT: return BSD_ENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return BSD_EPROTONOSUPPORT;
    case WSAEOPNOTSUPP: return BSD_EOPNOTSUPP;
    case WSAEAFNOSUPPORT: return BSD_EAFNOSUPPORT;
    case WSAEADDRINUSE: return BSD_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return BSD_EADDRNOTAVAIL;
    case WSAENETDOWN: return BSD_ENETDOWN;
    case WSAENETUNREACH: return BSD_ENETUNREACH;
    case WSAENETRESET: return BSD_ENETRESET;
    case WSAECONNABORTED: return BSD_ECONNABORTED;
    case WSAECONNRESET: return BSD_ECONNRESET;
    case WSAENOBUFS: return BSD_ENOBUFS;
    case WSAEISCONN: return BSD_EISCONN;
    case WSAENOTCONN: return BSD_ENOTCONN;
    case WSAESHUTDOWN: return BSD_ESHUTDOWN;
    case WSAETIMEDOUT: return BSD_ETIMEDOUT;
    case WSAECONNREFUSED: return BSD_ECONNREFUSED;
    case WSAEHOSTDOWN: return BSD_EHOSTDOWN;
    case WSAEHOSTUNREACH: return BSD_EHOSTUNREACH;
    case WSAEINVAL: return BSD_EINVAL;
    case WSAEBADF: return BSD_EBADF;
    case WSAEACCES: return BSD_EACCES;
    case WSAEFAULT: return BSD_EFAULT;
    case WSAEMFILE: return BSD_EMFILE;
    case WSAEINTR: return BSD_EINTR;
    default: return BSD_EIO;
    }
#else
    static const struct { int host; uint32_t bsd; } map[] = {
        {EPERM, BSD_EPERM}, {ENOENT, BSD_ENOENT}, {EINTR, BSD_EINTR}, {EIO, BSD_EIO}, {EBADF, BSD_EBADF},
        {ENOMEM, BSD_ENOMEM}, {EACCES, BSD_EACCES}, {EFAULT, BSD_EFAULT}, {EBUSY, BSD_EBUSY}, {EINVAL, BSD_EINVAL},
        {EMFILE, BSD_EMFILE}, {EPIPE, BSD_EPIPE}, {EAGAIN, BSD_EAGAIN}, {EWOULDBLOCK, BSD_EAGAIN},
        {EINPROGRESS, BSD_EINPROGRESS}, {EALREADY, BSD_EALREADY}, {ENOTSOCK, BSD_ENOTSOCK},
        {EDESTADDRREQ, BSD_EDESTADDRREQ}, {EMSGSIZE, BSD_EMSGSIZE}, {EPROTOTYPE, BSD_EPROTOTYPE},
        {ENOPROTOOPT, BSD_ENOPROTOOPT}, {EPROTONOSUPPORT, BSD_EPROTONOSUPPORT}, {EOPNOTSUPP, BSD_EOPNOTSUPP},
        {EAFNOSUPPORT, BSD_EAFNOSUPPORT}, {EADDRINUSE, BSD_EADDRINUSE}, {EADDRNOTAVAIL, BSD_EADDRNOTAVAIL},
        {ENETDOWN, BSD_ENETDOWN}, {ENETUNREACH, BSD_ENETUNREACH}, {ENETRESET, BSD_ENETRESET},
        {ECONNABORTED, BSD_ECONNABORTED}, {ECONNRESET, BSD_ECONNRESET}, {ENOBUFS, BSD_ENOBUFS},
        {EISCONN, BSD_EISCONN}, {ENOTCONN, BSD_ENOTCONN}, {ESHUTDOWN, BSD_ESHUTDOWN}, {ETIMEDOUT, BSD_ETIMEDOUT},
        {ECONNREFUSED, BSD_ECONNREFUSED}, {EHOSTDOWN, BSD_EHOSTDOWN}, {EHOSTUNREACH, BSD_EHOSTUNREACH},
    };
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (map[i].host == e) return map[i].bsd;
    return BSD_EIO;
#endif
}
