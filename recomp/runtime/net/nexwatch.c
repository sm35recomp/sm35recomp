// Why the game couldn't go online (nexwatch.h).
//
// The server's side of a game TLS connection, as it reaches the game: an HTTP "101 Switching Protocols" reply, then
// WebSocket binary frames (server to client: not masked). Each frame holds one or more PRUDP lite packets:
//   u8 0x80, u8 option length, u16 payload size, u8 stream types, u8 source port, u8 destination port,
//   u8 fragment id, u16 type and flags (type: low 4 bits; flags above: 1 ACK, 0x200 MULTI_ACK), u16 sequence id,
//   the options, the payload.
// A DATA packet's payload is one RMC message (fragment id 0: whole; larger ones are split, and a login reply never
// is): u32 size, u8 protocol (0x80 set: a request), then for a response u8 success, and
//   success: u32 call id, u32 method | 0x8000, the body (a login's starts with its Result, u32)
//   error:   u32 error code, u32 call id
#include "nexwatch.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "netcfg.h"

void menu_notice(const char *text, double seconds);  // runtime/ui/menu.mm (C linkage)

struct NexWatch {
    uint8_t *buf;
    size_t n, cap;
    bool upgraded;  // past the HTTP reply's header
    bool dead;      // not WebSocket after all: stop looking
};

NexWatch *nexwatch_new(void) { return calloc(1, sizeof(NexWatch)); }

void nexwatch_free(NexWatch *w) {
    if (!w) return;
    free(w->buf);
    free(w);
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static const struct { uint32_t code; const char *name; } NAMES[] = {
    {0x00010001, "Core::Unknown"},
    {0x00010006, "Core::AccessDenied"},
    {0x0001000A, "Core::InvalidArgument"},
    {0x00030001, "RendezVous::ConnectionFailure"},
    {0x00030002, "RendezVous::NotAuthenticated"},
    {0x00030064, "RendezVous::InvalidUsername"},
    {0x00030065, "RendezVous::InvalidPassword"},
    {0x00030067, "RendezVous::AccountDisabled"},
    {0x00030068, "RendezVous::AccountExpired"},
    {0x00030069, "RendezVous::ConcurrentLoginDenied"},
    {0x0003006A, "RendezVous::EncryptionFailure"},
    {0x0003006B, "RendezVous::InvalidPID"},
    {0x0003006C, "RendezVous::MaxConnectionsReached"},
    {0x000300CF, "RendezVous::GameServerMaintenance"},
    {0x000300DC, "RendezVous::DatabaseTemporarilyUnavailable"},
    {0x000300E0, "RendezVous::AccountTemporarilyDisabled"},
    {0x000300E1, "RendezVous::PartiallyServiceClosed"},
    {0x000300E2, "RendezVous::ConnectionDisconnectedForConcurrentLogin"},
    {0x00680001, "Authentication::NASAuthenticateError"},
    {0x00680002, "Authentication::TokenParseError"},
    {0x00680003, "Authentication::HttpConnectionError"},
    {0x00680006, "Authentication::TokenExpired"},
    {0x00680007, "Authentication::ValidationFailed"},
    {0x00680008, "Authentication::InvalidParam"},
    {0x00680009, "Authentication::PrincipalIdUnmatched"},
    {0x0068000B, "Authentication::UnderMaintenance"},
    {0x0068000C, "Authentication::UnsupportedVersion"},
    {0x0068000D, "Authentication::ServerVersionIsOld"},
};

const char *nex_error_name(uint32_t code) {
    code &= 0x7fffffff;
    for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; i++)
        if (NAMES[i].code == code) return NAMES[i].name;
    return NULL;
}

bool nexwatch_login_error(const uint8_t *m, size_t n, uint32_t *code) {
    if (n < 10 || rd32(m) + 4 > n) return false;
    uint8_t proto = m[4];
    if (proto & 0x80) return false;  // a request
    if (proto != 10 && proto != 11) return false;
    if (m[5] == 0) {  // an RMC error: u32 code, u32 call id (which call: the connection's first replies are logins)
        if (n < 14) return false;
        *code = rd32(m + 6);
        return true;
    }
    if (n < 18) return false;
    uint32_t method = rd32(m + 10) & 0x7fff;
    bool login = proto == 10 ? method == 1 || method == 2 || method == 3 || method == 6 : method == 1 || method == 4;
    if (!login) return false;
    uint32_t result = rd32(m + 14);
    if (!(result & 0x80000000u)) return false;
    *code = result;
    return true;
}

static void prudp(const uint8_t *p, size_t n) {
    size_t at = 0;
    while (at + 12 <= n && p[at] == 0x80) {
        size_t opt = p[at + 1], size = rd16(p + at + 2);
        if (at + 12 + opt + size > n) return;
        uint32_t tf = rd16(p + at + 8);
        uint8_t frag = p[at + 7];
        if ((tf & 0xf) == 2 && !((tf >> 4) & (1 | 0x200)) && frag == 0) {
            uint32_t code;
            if (nexwatch_login_error(p + at + 12 + opt, size, &code)) net_failure_notice(NET_FAIL_LOGIN, NULL, code);
        }
        at += 12 + opt + size;
    }
}

void nexwatch_in(NexWatch *w, const uint8_t *p, size_t n) {
    if (!w || w->dead || !n) return;
    if (w->n + n > 1 << 20) { w->dead = true; return; }  // (never a login's size: not our kind of connection)
    if (w->n + n > w->cap) {
        w->cap = (w->n + n) * 2;
        w->buf = realloc(w->buf, w->cap);
    }
    memcpy(w->buf + w->n, p, n);
    w->n += n;
    size_t at = 0;
    if (!w->upgraded) {
        if (w->n >= 5 && memcmp(w->buf, "HTTP/", 5)) { w->dead = true; return; }
        for (size_t i = 0; i + 4 <= w->n; i++)
            if (!memcmp(w->buf + i, "\r\n\r\n", 4)) { w->upgraded = true, at = i + 4; break; }
        if (!w->upgraded) return;
    }
    for (;;) {  // whole frames only; the rest waits for more bytes
        if (w->n - at < 2) break;
        const uint8_t *f = w->buf + at;
        size_t len = f[1] & 0x7f, hdr = 2;
        if (len == 126) {
            if (w->n - at < 4) break;
            len = (size_t)f[2] << 8 | f[3], hdr = 4;
        } else if (len == 127) {
            if (w->n - at < 10) break;
            len = 0;
            for (int i = 2; i < 10; i++) len = len << 8 | f[i];
            hdr = 10;
            if (len > 1 << 20) { w->dead = true; return; }
        }
        bool masked = f[1] & 0x80;  // (a server never masks; handled anyway)
        size_t total = hdr + (masked ? 4 : 0) + len;
        if (w->n - at < total) break;
        if ((f[0] & 0x0f) == 2) {  // binary
            uint8_t *data = (uint8_t *)f + hdr + (masked ? 4 : 0);
            if (masked)
                for (size_t i = 0; i < len; i++) data[i] ^= f[hdr + (i & 3)];
            prudp(data, len);
        }
        at += total;
    }
    memmove(w->buf, w->buf + at, w->n - at);
    w->n -= at;
}

// ---- the notice

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char last[512];
static uint64_t last_ns;

const char *net_failure_last(void) { return last; }

void net_failure_notice(int kind, const char *detail, uint32_t code) {
    char t[512];
    const char *name = code ? nex_error_name(code) : NULL;
    uint32_t c = code & 0x7fffffff;
    if (kind == NET_FAIL_CONNECT || kind == NET_FAIL_TLS) {
        snprintf(t, sizeof t, "Can't reach Nextendo's game server (%s). Check your internet connection; if it works, "
                 "Nextendo may be down: try again later.", detail && *detail ? detail : "no answer");
    } else if (c == 0x000300CF || c == 0x0068000B || c == 0x000300E1 || c == 0x000300DC) {
        snprintf(t, sizeof t, "Nextendo's game server is under maintenance or busy (%s). Try again later.", name);
    } else if (c == 0x00030069 || c == 0x000300E2) {
        snprintf(t, sizeof t, "Your Nextendo account is already online somewhere else (%s): another device or "
                 "emulator. Close it there, then try again.", name);
    } else if (c == 0x00030067 || c == 0x00030068 || c == 0x000300E0) {
        snprintf(t, sizeof t, "Nextendo says your account can't play right now (%s).", name);
    } else if (c == 0x00030002 || c == 0x00030064 || c == 0x00030065 || c == 0x0003006B || (c >> 16) == 0x0068) {
        snprintf(t, sizeof t, "Nextendo didn't accept your sign-in (%s). Open the menu, Online: sign out, sign in "
                 "again, then restart the game.", name ? name : "an account error");
    } else {
        snprintf(t, sizeof t, "Nextendo's game server refused the login (%s%s0x%08X).", name ? name : "",
                 name ? ", " : "", code | 0x80000000u);
    }
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&lock);
    bool show = !last_ns || now - last_ns > 20000000000ull || strcmp(t, last);
    if (show) snprintf(last, sizeof last, "%s", t), last_ns = now;
    pthread_mutex_unlock(&lock);
    if (!show) return;
    LOG("[online] notice: %s\n", t);
    menu_notice(t, 10);
}

static const char *errno_words(uint32_t e) {  // (the guest's errno: Linux numbers, runtime/net/hostsock.h)
    switch (e) {
    case 111: return "connection refused";
    case 110: return "timed out";
    case 101: return "network unreachable";
    case 113: return "no route to the server";
    case 104: return "connection reset";
    default: return "connection failed";
    }
}

void net_connect_failed(uint32_t ip, uint32_t err) {
    if (!net_is_game_server(ip)) return;
    net_failure_notice(NET_FAIL_CONNECT, errno_words(err), 0);
}
