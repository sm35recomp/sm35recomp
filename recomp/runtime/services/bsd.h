// The bsd service's host sockets, for the ssl service (runtime/services/ssl.c), which runs TLS over a socket the
// game made (SetSocketDescriptor).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct BsdSock BsdSock;

BsdSock *bsd_sock_ref(int fd);     // the game's socket fd (a reference; NULL: no such host socket)
void bsd_sock_unref(BsdSock *s);
uint32_t bsd_sock_peer_ip(BsdSock *s);  // the connected peer (host order), 0 none
// Non-blocking I/O on the host socket: bytes moved (0 from recv: closed by the peer), -1 would block, -2 failed
int bsd_sock_send(BsdSock *s, const void *data, size_t len);
int bsd_sock_recv(BsdSock *s, void *data, size_t len);
// Wait until it can be read (write false) or written, at most timeout_ms (< 0: no limit); the calling guest thread's
// core is free meanwhile. 1 ready, 0 timed out, -1 closed / failed.
int bsd_sock_wait(BsdSock *s, bool write, int timeout_ms);
