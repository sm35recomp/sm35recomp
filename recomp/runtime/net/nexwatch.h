// Why the game couldn't go online, told to the player (runtime/net/nexwatch.c). The game itself only shows an error
// code, or drops back to the menu; this puts a notice over the game naming the cause:
//   - the TCP connection or the TLS handshake to Nextendo's game server failed (down, or no internet),
//   - the game server refused the login (NEX TicketGranting / SecureConnection answered with an error).
// The login is seen in the server's replies on the game's TLS connections (PRUDP lite over WebSocket, clear text
// inside TLS), as they pass through runtime/services/ssl.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NexWatch NexWatch;
NexWatch *nexwatch_new(void);
void nexwatch_free(NexWatch *w);
// The server's bytes on one TLS connection, in order (the HTTP upgrade reply, then WebSocket frames).
void nexwatch_in(NexWatch *w, const uint8_t *p, size_t n);

// One RMC message (from its size field on): true with the error code when it is a refused login, i.e. a reply to
// TicketGranting (10) Login / LoginEx / RequestTicket / LoginWithContext or SecureConnection (11) Register /
// RegisterEx that is an RMC error, or a success whose first field (the call's Result) has the error bit.
bool nexwatch_login_error(const uint8_t *rmc, size_t n, uint32_t *code);
// "RendezVous::InvalidPassword" and the like, NULL when unknown (code with or without the error bit)
const char *nex_error_name(uint32_t code);

enum { NET_FAIL_CONNECT = 1, NET_FAIL_TLS = 2, NET_FAIL_LOGIN = 3 };
// The notice for a failure (detail: the OS's or TLS's words; code: NET_FAIL_LOGIN's NEX error), at most one every
// 20 seconds. The text is also logged, and kept for tests (net_failure_last).
void net_failure_notice(int kind, const char *detail, uint32_t code);
const char *net_failure_last(void);
// For runtime/services/bsd_host.c: a TCP connection to ip (host order) failed with the guest's errno err; a notice when
// ip is Nextendo's game server and Nextendo is the backend.
void net_connect_failed(uint32_t ip, uint32_t err);
