// The Nextendo Network account (runtime/net/nextendo.h): OAuth 2.0 authorization code + PKCE (S256) as a public
// client, the refresh token in the secret store, and the game's NEX login token from GET /api/nex-token.
//
// Sign-in: nextendo_begin_sign_in makes a random PKCE verifier and state, and (desktop) opens a listener on
// 127.0.0.1 with an OS-chosen port, in its own thread. The redirect_uri is http://127.0.0.1:<port>/callback and must
// be the same string in the authorize URL and in the token request. The browser lands on /callback; the thread
// closes the listener, checks the state, exchanges the code and answers the browser with a small page. A second
// begin, sign-out or the 5-minute timeout closes the listener too (begin / sign-out wait until the port is free).
//
// Tokens [inf: Nextendo's developer docs]: the access token lasts one hour, the refresh token 30 days; each refresh
// returns a new refresh token and the old one stops working a minute later. So the new refresh token is written to
// the store before the new access token is used. The token endpoint is /oauth/token (the portal's snippet) and, when
// that answers 404, /api/oauth/token (the docs).
//
// Secrets (tokens, code, verifier, PID) are never logged; the log has states and lengths.
#include "nextendo.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"

#include "../game.h"
#include "../hle.h"
#include "http.h"
#include "json.h"
#include "netcfg.h"
#include "secure_store.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
static void msleep(int ms) { Sleep((DWORD)ms); }
static void sock_rcvtimeo(sock_t s, int ms) { DWORD t = (DWORD)ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t); }
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
static void msleep(int ms) { usleep((useconds_t)ms * 1000); }
static void sock_rcvtimeo(sock_t s, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
#endif

#define CLIENT_ID GAME_NEXTENDO_CLIENT_ID
#define DEFAULT_BASE "https://nextendo.network"
#define SCOPE "identity%20game.matchmaking"
#define SIGNIN_TIMEOUT_S 300
#define NEX_MIN_LEFT_S 3600  // refetch the game token when less than this is left
#define ACCESS_MIN_LEFT_S 60

// state, guarded by `lock` (the UI reads it every frame)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;  // one token / game-token exchange at a time
static struct {
    NxState state;
    char username[64], message[160];
    bool loaded;  // the store has been read
    char refresh[600];
    char access[1100];
    time_t access_exp;
    char nex[1100];
    time_t nex_exp;
    uint64_t pid;
    bool pending;  // a sign-in is waiting for its callback
    char verifier[64], oauth_state[32], redirect[200];
    unsigned session;  // counts begin / sign-out: an exchange that finds it changed drops its result
} S;
static int token_path;  // 0: /oauth/token, 1: /api/oauth/token (once the first answered 404); under net_lock

typedef struct { sock_t s; volatile int cancel; unsigned session; } Listener;
static Listener *cur;  // the open listener (under `lock`); NULL once its socket is closed

// ---- helpers ----

static void random_bytes(uint8_t *out, size_t n) {
    mbedtls_entropy_context e;
    mbedtls_ctr_drbg_context d;
    mbedtls_entropy_init(&e);
    mbedtls_ctr_drbg_init(&d);
    static const char pers[] = GAME_EXE " nextendo";
    if (mbedtls_ctr_drbg_seed(&d, mbedtls_entropy_func, &e, (const unsigned char *)pers, sizeof pers) ||
        mbedtls_ctr_drbg_random(&d, out, n)) {
        for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(rand() ^ (int)time(NULL) >> (i & 7));  // (no entropy: weak)
    }
    mbedtls_ctr_drbg_free(&d);
    mbedtls_entropy_free(&e);
}

// base64url without padding; out gets the text and a NUL
static void b64url(const uint8_t *in, size_t n, char *out) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = a[v >> 18 & 63];
        out[o++] = a[v >> 12 & 63];
        if (i + 1 < n) out[o++] = a[v >> 6 & 63];
        if (i + 2 < n) out[o++] = a[v & 63];
    }
    out[o] = 0;
}

void nx_pkce_challenge(const char *verifier, char out[44]) {
    uint8_t h[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char *)verifier, strlen(verifier), h);
    b64url(h, 32, out);
}

// percent-encode (everything but the unreserved characters); false when it does not fit
static bool urlenc(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (o + 2 > cap) return false;
            out[o++] = (char)c;
        } else {
            if (o + 4 > cap) return false;
            o += (size_t)snprintf(out + o, 4, "%%%02X", c);
        }
    }
    out[o] = 0;
    return true;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// the value of query parameter `name` (percent-decoded) in the URL's query; false when absent or too long
static bool qparam(const char *url, const char *name, char *out, size_t cap) {
    const char *q = strchr(url, '?');
    size_t nl = strlen(name);
    while (q) {
        q++;
        if (!strncmp(q, name, nl) && q[nl] == '=') {
            q += nl + 1;
            size_t o = 0;
            for (; *q && *q != '&' && *q != '#'; q++) {
                char c = *q;
                if (c == '+') c = ' ';
                else if (c == '%' && hexv(q[1]) >= 0 && hexv(q[2]) >= 0) { c = (char)(hexv(q[1]) << 4 | hexv(q[2])); q += 2; }
                if (o + 1 >= cap) return false;
                out[o++] = c;
            }
            out[o] = 0;
            return true;
        }
        q = strchr(q, '&');
    }
    return false;
}

static void set_state(NxState st, const char *msg) {  // under `lock`
    S.state = st;
    snprintf(S.message, sizeof S.message, "%s", msg ? msg : "");
}

// the API base: the default, or RECOMP_NEXTENDO_API when its host is a local address (a test server)
static const char *api_base(void) {
    static char base[200];
    static bool warned;
    const char *e = getenv("RECOMP_NEXTENDO_API");
    if (e && *e) {
        // http[s]://localhost or a local IPv4 literal, and the host must end there (not localhost.example.com)
        const char *h = !strncmp(e, "http://", 7) ? e + 7 : !strncmp(e, "https://", 8) ? e + 8 : NULL;
        unsigned a, b, c, d;
        int n = 0;
        bool ok = false;
        if (h && !strncmp(h, "localhost", 9)) { n = 9; ok = true; }
        else if (h && sscanf(h, "%u.%u.%u.%u%n", &a, &b, &c, &d, &n) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
            ok = net_ip_is_local(a << 24 | b << 16 | c << 8 | d);
        if (ok && h[n] != ':' && h[n] != '/' && h[n]) ok = false;
        if (ok && strlen(e) < sizeof base) {
            snprintf(base, sizeof base, "%s", e);
            size_t n = strlen(base);
            while (n && base[n - 1] == '/') base[--n] = 0;
            return base;
        }
        if (!warned) { warned = true; LOG("[nextendo] RECOMP_NEXTENDO_API ignored: not a local address\n"); }
    }
    return DEFAULT_BASE;
}

static long now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (long)(ts.tv_sec % 1000000) * 1000 + ts.tv_nsec / 1000000;
}

// POST a form to the token endpoint (retrying the docs' path on 404). Under net_lock.
static int token_post(const char *form, int timeout_ms, HttpResponse *r) {
    static const char *const paths[] = {"/oauth/token", "/api/oauth/token"};
    char url[300];
    for (int tries = 0; tries < 2; tries++) {
        snprintf(url, sizeof url, "%s%s", api_base(), paths[token_path]);
        int st = http_request("POST", url, "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n",
                              form, strlen(form), timeout_ms, r);
        if (st != 404 || token_path == 1) return st;
        http_free(r);
        token_path = 1;
    }
    return 0;
}

static void load_store(void) {  // under `lock`
    if (S.loaded) return;
    S.loaded = true;
    if (secure_get("refresh_token", S.refresh, sizeof S.refresh) && S.refresh[0]) S.state = NX_SIGNED_IN;
    else S.refresh[0] = 0;
    secure_get("username", S.username, sizeof S.username);
}

// ---- tokens ----

// store the answer of the token endpoint: the new refresh token first (rotation), then the access token
// (a result is dropped when the session changed meanwhile: a sign-out must not be undone by a late answer)
static bool take_tokens(const HttpResponse *r, const char *what, unsigned session) {
    Json *j = json_parse(r->body ? r->body : "", r->len);
    const char *rt = json_str(json_get(j, "refresh_token"), ""), *at = json_str(json_get(j, "access_token"), "");
    double exp = json_num(json_get(j, "expires_in"), 3600);
    bool ok = at[0] && strlen(at) < sizeof S.access && strlen(rt) < sizeof S.refresh;
    if (ok) {
        pthread_mutex_lock(&lock);
        if (S.session != session) {
            pthread_mutex_unlock(&lock);
            json_free(j);
            LOG("[nextendo] %s: dropped (signed out meanwhile)\n", what);
            return false;
        }
        if (rt[0]) {
            if (!secure_set("refresh_token", rt)) LOG("[nextendo] could not save the refresh token\n");
            snprintf(S.refresh, sizeof S.refresh, "%s", rt);
        }
        snprintf(S.access, sizeof S.access, "%s", at);
        S.access_exp = time(NULL) + (time_t)(exp > 0 ? exp : 3600);
        pthread_mutex_unlock(&lock);
        LOG("[nextendo] %s ok (access %zu chars, refresh %zu chars, %ds)\n", what, strlen(at), strlen(rt), (int)exp);
    } else {
        LOG("[nextendo] %s: the answer has no usable access token\n", what);
    }
    json_free(j);
    return ok;
}

// refresh_token -> new tokens. False on failure; a refused refresh token ends the session.
static bool refresh_access(int timeout_ms, unsigned session) {  // under net_lock
    char rt[600], enc[1300], form[1500];
    pthread_mutex_lock(&lock);
    snprintf(rt, sizeof rt, "%s", S.refresh);
    pthread_mutex_unlock(&lock);
    if (!urlenc(rt, enc, sizeof enc)) return false;
    snprintf(form, sizeof form, "grant_type=refresh_token&refresh_token=%s&client_id=" CLIENT_ID, enc);
    HttpResponse r;
    int st = token_post(form, timeout_ms, &r);
    bool ok = false;
    if (st == 200) {
        ok = take_tokens(&r, "refresh", session);
    } else if (st == 0) {
        LOG("[nextendo] refresh: no answer (%s)\n", r.error);
    } else {
        Json *j = json_parse(r.body ? r.body : "", r.len);
        bool gone = st == 401 || !strcmp(json_str(json_get(j, "error"), ""), "invalid_grant");
        json_free(j);
        LOG("[nextendo] refresh: server answered %d%s\n", st, gone ? " (session ended)" : "");
        if (gone) {
            pthread_mutex_lock(&lock);
            if (S.session == session) {
                secure_del("refresh_token");
                S.refresh[0] = S.access[0] = S.nex[0] = 0;
                S.pid = 0;
                set_state(NX_ERROR, "Your Nextendo session ended. Sign in again.");
            }
            pthread_mutex_unlock(&lock);
        }
    }
    http_free(&r);
    return ok;
}

// GET /api/nex-token with the access token. Returns the HTTP status (0: no answer).
static int fetch_nex(int timeout_ms, unsigned session) {  // under net_lock
    char url[300], hdr[1200];
    pthread_mutex_lock(&lock);
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\nAccept: application/json\r\n", S.access);
    pthread_mutex_unlock(&lock);
    snprintf(url, sizeof url, "%s/api/nex-token", api_base());
    HttpResponse r;
    int st = http_request("GET", url, hdr, NULL, 0, timeout_ms, &r);
    if (st == 200) {
        Json *j = json_parse(r.body ? r.body : "", r.len);
        const char *nt = json_str(json_get(j, "nex_token"), ""), *un = json_str(json_get(j, "username"), "");
        double pid = json_num(json_get(j, "pid"), 0), exp = json_num(json_get(j, "expires_in"), 3600);
        if (nt[0] && strlen(nt) < sizeof S.nex && pid > 0) {
            pthread_mutex_lock(&lock);
            if (S.session != session) {
                pthread_mutex_unlock(&lock);
                json_free(j);
                http_free(&r);
                LOG("[nextendo] game token: dropped (signed out meanwhile)\n");
                return 0;
            }
            snprintf(S.nex, sizeof S.nex, "%s", nt);
            S.pid = (uint64_t)pid;
            S.nex_exp = time(NULL) + (time_t)(exp > 0 ? exp : 3600);
            if (strlen(un) < sizeof S.username && un[0] && strcmp(un, S.username)) {
                snprintf(S.username, sizeof S.username, "%s", un);
                secure_set("username", un);
            }
            pthread_mutex_unlock(&lock);
            LOG("[nextendo] game token ok (%zu chars, %ds)\n", strlen(nt), (int)exp);
        } else {
            LOG("[nextendo] game token: unusable answer\n");
            st = 502;
        }
        json_free(j);
    } else {
        LOG("[nextendo] game token: %s %d\n", st ? "server answered" : "no answer", st);
    }
    http_free(&r);
    return st;
}

bool nextendo_prepare_game_login(int timeout_ms) {
    long end = now_ms() + timeout_ms;
    pthread_mutex_lock(&net_lock);
    bool ok = false;
    pthread_mutex_lock(&lock);
    load_store();
    bool have_refresh = S.refresh[0];
    unsigned session = S.session;
    bool nex_ok = S.nex[0] && S.nex_exp - time(NULL) > NEX_MIN_LEFT_S;
    bool access_ok = S.access[0] && S.access_exp - time(NULL) > ACCESS_MIN_LEFT_S;
    pthread_mutex_unlock(&lock);
    if (nex_ok) ok = true;
    else if (have_refresh) {
        int left = (int)(end - now_ms());
        if (!access_ok && left > 0) access_ok = refresh_access(left, session);
        left = (int)(end - now_ms());
        if (access_ok && left > 0) {
            int st = fetch_nex(left, session);
            if (st == 401 && (left = (int)(end - now_ms())) > 0 && refresh_access(left, session) && (left = (int)(end - now_ms())) > 0)
                st = fetch_nex(left, session);  // (the access token had expired early)
            ok = st == 200;
        }
    }
    pthread_mutex_unlock(&net_lock);
    pthread_mutex_lock(&lock);
    if (S.session != session) ok = false;
    pthread_mutex_unlock(&lock);
    return ok;
}

// ---- status ----

void nextendo_status(NxStatus *out) {
    pthread_mutex_lock(&lock);
    load_store();
    out->state = S.state;
    snprintf(out->username, sizeof out->username, "%s", S.username);
    snprintf(out->message, sizeof out->message, "%s", S.message);
    pthread_mutex_unlock(&lock);
}

bool nextendo_signed_in(void) {
    pthread_mutex_lock(&lock);
    load_store();
    bool r = S.refresh[0];
    pthread_mutex_unlock(&lock);
    return r;
}

uint64_t nextendo_pid(void) {
    pthread_mutex_lock(&lock);
    uint64_t p = S.nex[0] ? S.pid : 0;
    pthread_mutex_unlock(&lock);
    return p;
}

bool nextendo_username(char *out, size_t cap) {
    pthread_mutex_lock(&lock);
    load_store();
    bool ok = S.username[0] && strlen(S.username) < cap;
    if (ok) strcpy(out, S.username);
    pthread_mutex_unlock(&lock);
    return ok;
}

size_t nextendo_nex_token(char *out, size_t cap) {
    pthread_mutex_lock(&lock);
    size_t n = S.nex[0] && S.nex_exp > time(NULL) ? strlen(S.nex) : 0;
    if (n && n < cap) memcpy(out, S.nex, n + 1);
    else n = 0;
    pthread_mutex_unlock(&lock);
    return n;
}

// ---- sign-in ----

// close the listener's port and wait until it is closed (the thread does it within ~200 ms)
static void stop_listener(void) {
    pthread_mutex_lock(&lock);
    Listener *l = cur;
    if (l) l->cancel = 1;
    pthread_mutex_unlock(&lock);
    for (int i = 0; l && i < 200; i++) {
        pthread_mutex_lock(&lock);
        bool gone = cur != l;
        pthread_mutex_unlock(&lock);
        if (gone) break;
        msleep(10);
    }
}

static void html_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (r) { strcpy(out + o, r); o += strlen(r); } else out[o++] = *s;
    }
    out[o] = 0;
}

static void reply_page(sock_t c, int code, const char *text) {
    char esc[400], body[700], head[200];
    html_escape(text, esc, sizeof esc);
    int n = snprintf(body, sizeof body,
                     "<!doctype html><meta charset=utf-8><title>Nextendo</title><body style=\"font-family:sans-serif;"
                     "text-align:center;margin-top:20vh\"><h2>%s</h2></body>", esc);
    int h = snprintf(head, sizeof head, "HTTP/1.1 %d OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", code, n);
    send(c, head, (size_t)h, 0);
    send(c, body, (size_t)n, 0);
}

static void drop_listener(Listener *l) {  // close the port, and tell stop_listener
    sock_close(l->s);
    pthread_mutex_lock(&lock);
    if (cur == l) cur = NULL;
    pthread_mutex_unlock(&lock);
}

// finish a sign-in from a callback URL. On failure `err` says why (for the browser page).
static bool finish_core(const char *url, char *err, size_t errcap) {
    char st[64], code[512], e[64];
    pthread_mutex_lock(&lock);
    bool pending = S.pending;
    bool match = pending && qparam(url, "state", st, sizeof st) && !strcmp(st, S.oauth_state);
    char verifier[64], redirect[200];
    unsigned session = S.session;
    if (match) {
        snprintf(verifier, sizeof verifier, "%s", S.verifier);
        snprintf(redirect, sizeof redirect, "%s", S.redirect);
        S.pending = false;  // (single use)
    }
    pthread_mutex_unlock(&lock);
    if (!match) {
        snprintf(err, errcap, pending ? "Sign-in failed: the reply did not match the request." : "No sign-in is waiting.");
        LOG("[nextendo] callback refused: %s\n", pending ? "state mismatch" : "nothing pending");
        return false;
    }
    if (qparam(url, "error", e, sizeof e)) {
        if (!strcmp(e, "access_denied")) snprintf(err, errcap, "Sign-in was denied in the browser.");
        else snprintf(err, errcap, "Sign-in failed: %s", e);
        LOG("[nextendo] callback: error from the server\n");
        goto fail;
    }
    if (!qparam(url, "code", code, sizeof code) || !code[0]) {
        snprintf(err, errcap, "Sign-in failed: no code in the reply.");
        goto fail;
    }
    char ecode[1100], eredir[500], everif[200], form[2000];
    if (!urlenc(code, ecode, sizeof ecode) || !urlenc(redirect, eredir, sizeof eredir) || !urlenc(verifier, everif, sizeof everif)) {
        snprintf(err, errcap, "Sign-in failed: the reply is too long.");
        goto fail;
    }
    snprintf(form, sizeof form, "grant_type=authorization_code&client_id=" CLIENT_ID "&code=%s&redirect_uri=%s&code_verifier=%s", ecode, eredir, everif);
    pthread_mutex_lock(&net_lock);
    HttpResponse r;
    int status = token_post(form, 15000, &r);
    bool ok = false;
    if (status == 200) {
        ok = take_tokens(&r, "sign-in", session);  // (false too when cancelled meanwhile)
        pthread_mutex_lock(&lock);
        bool current = S.session == session;
        pthread_mutex_unlock(&lock);
        if (!ok) snprintf(err, errcap, current ? "Sign-in failed: unusable answer from Nextendo." : "Sign-in was cancelled.");
    } else if (status == 0) {
        snprintf(err, errcap, "Sign-in failed: could not reach Nextendo.");
    } else {
        snprintf(err, errcap, "Sign-in failed: Nextendo answered %d.", status);
        LOG("[nextendo] code exchange: server answered %d\n", status);
    }
    http_free(&r);
    pthread_mutex_unlock(&net_lock);
    pthread_mutex_lock(&lock);
    if (S.session == session) {
        if (ok) { S.nex[0] = 0; set_state(NX_SIGNED_IN, ""); }
        else set_state(NX_ERROR, err);
    }
    pthread_mutex_unlock(&lock);
    return ok;
fail:
    pthread_mutex_lock(&lock);
    if (S.session == session) set_state(NX_ERROR, err);
    pthread_mutex_unlock(&lock);
    return false;
}

bool nextendo_finish_sign_in(const char *callback_url) {
    char err[160];
    if (!finish_core(callback_url, err, sizeof err)) return false;
    nextendo_prepare_game_login(5000);  // (learns the user name)
    return true;
}

static void *listener_main(void *arg) {
    Listener *l = arg;
    time_t end = time(NULL) + SIGNIN_TIMEOUT_S;
    bool cb = false, closed = false;
    char target[1024] = "";
    while (!l->cancel && time(NULL) < end && !cb) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(l->s, &rf);
        struct timeval tv = {0, 200000};
        if (select((int)l->s + 1, &rf, NULL, NULL, &tv) <= 0) continue;
        sock_t c = accept(l->s, NULL, NULL);
        if (c == SOCK_BAD) continue;
        sock_rcvtimeo(c, 2000);
        char req[2048];
        int n = 0, got;
        while (n < (int)sizeof req - 1 && (got = (int)recv(c, req + n, (size_t)(sizeof req - 1 - n), 0)) > 0) {
            n += got;
            req[n] = 0;
            if (strchr(req, '\n')) break;
        }
        req[n > 0 ? n : 0] = 0;
        char path[1024];
        if (sscanf(req, "GET %1023s", path) == 1 && !strncmp(path, "/callback", 9) && (path[9] == '?' || !path[9])) {
            drop_listener(l);  // the port is free from here on
            closed = cb = true;
            snprintf(target, sizeof target, "%s", path);
            char err[160] = "";
            if (finish_core(target, err, sizeof err)) reply_page(c, 200, "Signed in to Nextendo. You can close this tab and go back to the game.");
            else {
                reply_page(c, 400, err);
                pthread_mutex_lock(&lock);
                if (S.session == l->session && S.state == NX_WAITING_BROWSER) { S.pending = false; set_state(NX_ERROR, err); }  // (a mismatch leaves it waiting)
                pthread_mutex_unlock(&lock);
                cb = false;
                sock_close(c);
                break;
            }
            sock_close(c);
        } else {
            reply_page(c, 404, "Not found.");
            sock_close(c);
        }
    }
    if (!cb) {  // cancelled, timed out, or the callback failed
        if (!closed) drop_listener(l);
        pthread_mutex_lock(&lock);
        if (!l->cancel && S.session == l->session && S.state == NX_WAITING_BROWSER) {
            S.pending = false;
            if (S.refresh[0]) set_state(NX_SIGNED_IN, "");
            else set_state(NX_ERROR, "Sign-in timed out. Try again.");
        }
        pthread_mutex_unlock(&lock);
    } else {
        nextendo_prepare_game_login(5000);  // (learns the user name)
    }
    free(l);
    return NULL;
}

// a listening socket on 127.0.0.1, any free port; returns the port in *port
static sock_t listen_loopback(int *port) {
#ifdef _WIN32
    static bool wsa;
    if (!wsa) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); wsa = true; }
#endif
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_BAD) return s;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001);
    socklen_t al = sizeof a;
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4) || getsockname(s, (struct sockaddr *)&a, &al)) {
        sock_close(s);
        return SOCK_BAD;
    }
    *port = ntohs(a.sin_port);
    return s;
}

bool nextendo_begin_sign_in(const char *redirect_uri, char *url_out, size_t cap) {
    stop_listener();
    uint8_t rnd[48];
    random_bytes(rnd, sizeof rnd);
    char verifier[64], state[32], challenge[44], redirect[200], eredir[500];
    b64url(rnd, 32, verifier);
    b64url(rnd + 32, 16, state);
    nx_pkce_challenge(verifier, challenge);
    Listener *l = NULL;
    if (redirect_uri) {
        snprintf(redirect, sizeof redirect, "%s", redirect_uri);
    } else {
        int port = 0;
        sock_t s = listen_loopback(&port);
        if (s == SOCK_BAD || !(l = calloc(1, sizeof *l))) {
            if (s != SOCK_BAD) sock_close(s);
            pthread_mutex_lock(&lock);
            set_state(NX_ERROR, "Could not open the local sign-in port.");
            pthread_mutex_unlock(&lock);
            return false;
        }
        l->s = s;
        snprintf(redirect, sizeof redirect, "http://127.0.0.1:%d/callback", port);
    }
    char base[300];
    if (!urlenc(redirect, eredir, sizeof eredir) ||
        snprintf(base, sizeof base, "%s/oauth/authorize", api_base()) >= (int)sizeof base ||
        (size_t)snprintf(url_out, cap, "%s?response_type=code&client_id=" CLIENT_ID "&redirect_uri=%s&scope=%s"
                         "&state=%s&code_challenge=%s&code_challenge_method=S256", base, eredir, SCOPE, state, challenge) >= cap) {
        if (l) { sock_close(l->s); free(l); }
        return false;
    }
    pthread_mutex_lock(&lock);
    load_store();
    S.session++;
    S.pending = true;
    snprintf(S.verifier, sizeof S.verifier, "%s", verifier);
    snprintf(S.oauth_state, sizeof S.oauth_state, "%s", state);
    snprintf(S.redirect, sizeof S.redirect, "%s", redirect);
    set_state(NX_WAITING_BROWSER, "");
    if (l) {
        l->session = S.session;
        cur = l;
    }
    pthread_mutex_unlock(&lock);
    if (l) {
        pthread_t t;
        if (pthread_create(&t, NULL, listener_main, l)) {
            drop_listener(l);
            free(l);
            pthread_mutex_lock(&lock);
            S.pending = false;
            set_state(NX_ERROR, "Could not start the local sign-in listener.");
            pthread_mutex_unlock(&lock);
            return false;
        }
        pthread_detach(t);
    }
    LOG("[nextendo] sign-in started (%s)\n", l ? "loopback listener" : "app redirect");
    return true;
}

void nextendo_sign_out(void) {
    stop_listener();
    pthread_mutex_lock(&lock);
    S.session++;
    S.pending = false;
    secure_del("refresh_token");
    secure_del("username");
    S.refresh[0] = S.access[0] = S.nex[0] = S.username[0] = 0;
    S.pid = 0;
    S.loaded = true;
    set_state(NX_SIGNED_OUT, "");
    pthread_mutex_unlock(&lock);
    LOG("[nextendo] signed out\n");
}

// The platform's sign-in sheet ended without a redirect (cancelled, failed): the pending sign-in becomes an error.
void nextendo_sign_in_aborted(const char *msg) {
    pthread_mutex_lock(&lock);
    bool waiting = S.state == NX_WAITING_BROWSER;
    if (waiting) { S.pending = false; set_state(NX_ERROR, msg); }
    pthread_mutex_unlock(&lock);
    if (waiting) stop_listener();  // (a loopback sign-in: its port closes)
}

void nx_test_reset(void) {
    stop_listener();
    pthread_mutex_lock(&net_lock);
    pthread_mutex_lock(&lock);
    unsigned session = S.session + 1;
    memset(&S, 0, sizeof S);
    S.session = session;
    token_path = 0;
    pthread_mutex_unlock(&lock);
    pthread_mutex_unlock(&net_lock);
}
