// Blocking HTTP/1.1 client (runtime/net/http.h): one connection per request, timeouts on connect / send / receive,
// Content-Length, chunked or read-to-close bodies.
//
// https:// is TLS 1.2 through Mbed TLS (third_party/mbedtls, built with net/mbedtls_user_config.h) over the same
// blocking socket: the BIO callbacks send / recv on the fd. The server's certificate must verify, and must name the
// URL's host (also sent as SNI). Trust is the system's CA bundle when the machine has one (/etc/ssl/cert.pem, ...)
// plus the roots embedded in net/roots.c, so a machine with no bundle works too. RECOMP_HTTPS_CA=<pem file> replaces
// both, for the test server; it is honoured only when the connection really goes to a local address, so it cannot be
// used to point a real host at a forged certificate. RECOMP_HTTPS_NO_SYSTEM_ROOTS=1 skips the system bundles (tests
// only: it can only remove trust).
#include "http.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "../game.h"
#include "../hle.h"
#include "netcfg.h"
#include "roots.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
static void net_init(void) {
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
}
static int sock_err_inprogress(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static int sock_err_again(void) { int e = WSAGetLastError(); return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK; }
static void sock_nonblock(sock_t s, int on) { u_long v = (u_long)on; ioctlsocket(s, FIONBIO, &v); }
static void sock_timeouts(sock_t s, int ms) {
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
}
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
static void net_init(void) {}
static int sock_err_inprogress(void) { return errno == EINPROGRESS; }
static int sock_err_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
static void sock_nonblock(sock_t s, int on) {
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
}
static void sock_timeouts(sock_t s, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int fail(HttpResponse *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->error, sizeof out->error, fmt, ap);
    va_end(ap);
    out->status = 0;
    return 0;
}

// "http[s]://host[:port]/path" -> host, port, path (tls: https)
static int parse_url(const char *url, int *tls, char *host, size_t hcap, char *port, size_t pcap, const char **path) {
    if (!strncmp(url, "https://", 8)) *tls = 1;
    else if (!strncmp(url, "http://", 7)) *tls = 0;
    else return 0;
    const char *h = url + (*tls ? 8 : 7), *end = h + strcspn(h, ":/?");
    if (end == h || (size_t)(end - h) >= hcap) return 0;
    memcpy(host, h, (size_t)(end - h));
    host[end - h] = 0;
    snprintf(port, pcap, *tls ? "443" : "80");
    if (*end == ':') {
        const char *p = end + 1, *pe = p + strspn(p, "0123456789");
        if (pe == p || (size_t)(pe - p) >= pcap) return 0;
        memcpy(port, p, (size_t)(pe - p));
        port[pe - p] = 0;
        end = pe;
    }
    *path = *end ? end : "/";
    return 1;
}

static sock_t connect_to(const char *host, const char *port, int timeout_ms, HttpResponse *out) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) { fail(out, "cannot resolve %s", host); return SOCK_BAD; }
    sock_t s = SOCK_BAD;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == SOCK_BAD) continue;
        sock_nonblock(s, 1);
        int r = connect(s, a->ai_addr, (int)a->ai_addrlen);
        if (r != 0 && sock_err_inprogress()) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s, &w);
            struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            r = select((int)s + 1, NULL, &w, NULL, &tv) == 1 ? 0 : -1;
            if (r == 0) {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
                if (err) r = -1;
            }
        }
        if (r == 0) {
            sock_nonblock(s, 0);
            sock_timeouts(s, timeout_ms);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            break;
        }
        sock_close(s);
        s = SOCK_BAD;
    }
    freeaddrinfo(res);
    if (s == SOCK_BAD) fail(out, "cannot connect to %s:%s", host, port);
    return s;
}

// ---- TLS: one random generator for every request (Mbed TLS's is not thread-safe on its own)
static pthread_mutex_t rng_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static int rng_ok;

static int rng(void *p, unsigned char *out, size_t n) {
    (void)p;
    pthread_mutex_lock(&rng_lock);
    int r = mbedtls_ctr_drbg_random(&drbg, out, n);
    pthread_mutex_unlock(&rng_lock);
    return r;
}

static int rng_init(void) {
    pthread_mutex_lock(&rng_lock);
    if (!rng_ok) {
        psa_crypto_init();
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        static const char pers[] = GAME_EXE " http";
        rng_ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, sizeof pers) == 0;
        if (!rng_ok) LOG("[http] random generator failed to start\n");
    }
    int ok = rng_ok;
    pthread_mutex_unlock(&rng_lock);
    return ok;
}

// the BIO: the blocking socket (its send / receive timeouts bound the handshake and each read)
static int bio_send(void *p, const unsigned char *b, size_t n) {
    long r = send(*(sock_t *)p, (const char *)b, (int)(n > 1 << 20 ? 1 << 20 : n), MSG_NOSIGNAL);
    if (r >= 0) return (int)r;
    return sock_err_again() ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv(void *p, unsigned char *b, size_t n) {
    long r = recv(*(sock_t *)p, (char *)b, (int)(n > 1 << 20 ? 1 << 20 : n), 0);
    if (r >= 0) return (int)r;
    return sock_err_again() ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

// Is the connected peer a loopback / private address (host order for v4)? The check is on the address the socket
// really reached, not on the name in the URL.
static int peer_is_local(sock_t s) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getpeername(s, (struct sockaddr *)&ss, &len)) return 0;
    if (ss.ss_family == AF_INET) return net_ip_is_local(ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr));
    if (ss.ss_family == AF_INET6) {
        const unsigned char *a = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        static const unsigned char lo[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return !memcmp(a, lo, 16);
    }
    return 0;
}

static const char *const bundles[] = {"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt",
                                      "/etc/pki/tls/certs/ca-bundle.crt"};

// The CAs this request trusts. false = none could be loaded.
static int load_trust(mbedtls_x509_crt *ca, int local) {
    const char *only = getenv("RECOMP_HTTPS_CA");
    if (only && *only) {
        if (local) return mbedtls_x509_crt_parse_file(ca, only) == 0;
        LOG("[http] RECOMP_HTTPS_CA is for local test servers only: ignored for this host\n");
    }
    const char *nosys = getenv("RECOMP_HTTPS_NO_SYSTEM_ROOTS");  // (tests: prove the embedded roots alone suffice)
    for (size_t i = 0; i < sizeof bundles / sizeof *bundles && !(nosys && *nosys == '1'); i++)
        if (mbedtls_x509_crt_parse_file(ca, bundles[i]) >= 0 && ca->version) break;  // (a bad entry is skipped)
    const char *pem = net_root_pems();
    return mbedtls_x509_crt_parse(ca, (const unsigned char *)pem, strlen(pem) + 1) >= 0;
}

typedef struct {
    sock_t s;
    int tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
} Conn;

static void conn_close(Conn *c) {
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_x509_crt_free(&c->ca);
    }
    sock_close(c->s);
}

// TLS handshake for host over c->s; on failure the socket is closed and out says why
static int tls_open(Conn *c, const char *host, HttpResponse *out) {
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    c->tls = 1;
    if (!rng_init() || !load_trust(&c->ca, peer_is_local(c->s)) ||
        mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT)) {
        conn_close(c);
        return fail(out, "TLS: handshake failed");
    }
    mbedtls_ssl_conf_rng(&c->conf, rng, NULL);
    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) || mbedtls_ssl_set_hostname(&c->ssl, host)) {
        conn_close(c);
        return fail(out, "TLS: handshake failed");
    }
    mbedtls_ssl_set_bio(&c->ssl, &c->s, bio_send, bio_recv, NULL);
    int r;
    while ((r = mbedtls_ssl_handshake(&c->ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {}
    if (r == 0) return 1;
    char why[200];
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        mbedtls_x509_crt_verify_info(why, sizeof why, "", mbedtls_ssl_get_verify_result(&c->ssl));
        for (char *p = why; *p; p++) if (*p == '\n') *p = ' ';
    } else {
        mbedtls_strerror(r, why, sizeof why);
    }
    NLOG("[http] %s: TLS: %s\n", host, why);
    conn_close(c);
    return fail(out, r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ? "TLS: certificate not trusted" : "TLS: handshake failed");
}

static int conn_send(Conn *c, const char *p, size_t n) {
    while (n) {
        long k;
        if (c->tls) {
            k = mbedtls_ssl_write(&c->ssl, (const unsigned char *)p, n);
            if (k == MBEDTLS_ERR_SSL_WANT_READ || k == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        } else {
            k = send(c->s, p, (int)(n > 1 << 20 ? 1 << 20 : n), MSG_NOSIGNAL);
        }
        if (k <= 0) return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

// like recv: bytes read, 0 at the end of the stream, -1 on an error or a timeout
static long conn_recv(Conn *c, char *b, size_t n) {
    if (!c->tls) return recv(c->s, b, (int)n, 0);
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)b, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
        return r < 0 ? -1 : r;
    }
}

typedef struct { char *p; size_t n, cap; } Buf;
static int buf_add(Buf *b, const char *p, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8192;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = realloc(b->p, cap);
        if (!q) return 0;
        b->p = q, b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}

static const char *find_header(const char *head, const char *name) {  // case-insensitive, at a line start
    size_t ln = strlen(name);
    for (const char *l = strstr(head, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        const char *h = l + 2;
        int ok = 1;
        for (size_t i = 0; i < ln && ok; i++) {
            char a = h[i], b = name[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            ok = a == b;
        }
        if (ok && h[ln] == ':') {
            h += ln + 1;
            while (*h == ' ') h++;
            return h;
        }
        if (h[0] == '\r') break;
    }
    return NULL;
}

// Decode a chunked body in place; returns the decoded length, or -1 if incomplete / malformed.
static long dechunk(char *p, size_t n) {
    size_t r = 0, w = 0;
    for (;;) {
        char *e;
        if (r >= n) return -1;
        unsigned long sz = strtoul(p + r, &e, 16);
        char *nl = strstr(p + r, "\r\n");
        if (!nl || e == p + r) return -1;
        r = (size_t)(nl - p) + 2;
        if (sz == 0) return (long)w;
        if (r + sz + 2 > n) return -1;
        memmove(p + w, p + r, sz);
        w += sz;
        r += sz + 2;
    }
}

int http_request(const char *method, const char *url, const char *headers, const void *body, size_t body_len,
                 int timeout_ms, HttpResponse *out) {
    memset(out, 0, sizeof *out);
    net_init();
    char host[256], port[8];
    const char *path;
    int tls;
    if (!parse_url(url, &tls, host, sizeof host, port, sizeof port, &path)) return fail(out, "bad url %s", url);
    Conn conn = {0};
    conn.s = connect_to(host, port, timeout_ms, out);
    if (conn.s == SOCK_BAD) return 0;
    if (tls && !tls_open(&conn, host, out)) return 0;
    Conn *c = &conn;
    Buf req = {0};
    char line[1024];
    char hostport[300];
    snprintf(hostport, sizeof hostport, strcmp(port, tls ? "443" : "80") ? "%s:%s" : "%s", host, port);
    snprintf(line, sizeof line,
             "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: " GAME_EXE "\r\nContent-Length: %zu\r\n",
             method, path, hostport, body_len);
    buf_add(&req, line, strlen(line));
    if (headers) buf_add(&req, headers, strlen(headers));
    buf_add(&req, "\r\n", 2);
    if (body_len) buf_add(&req, body, body_len);
    int sent = req.p && conn_send(c, req.p, req.n);
    free(req.p);
    if (!sent) { conn_close(c); return fail(out, "send failed"); }

    Buf resp = {0};
    char chunk[16384];
    long k;
    size_t head_end = 0;
    long want = -1;  // total bytes once the header and the Content-Length are known
    while ((k = conn_recv(c, chunk, sizeof chunk)) > 0) {
        if (!buf_add(&resp, chunk, (size_t)k)) break;
        if (!head_end) {
            char *e = strstr(resp.p, "\r\n\r\n");
            if (e) {
                head_end = (size_t)(e - resp.p) + 4;
                const char *cl = find_header(resp.p, "Content-Length");
                if (cl) want = (long)head_end + atol(cl);
            }
        }
        if (want >= 0 && (long)resp.n >= want) break;
    }
    conn_close(c);
    if (!head_end) { free(resp.p); return fail(out, k < 0 ? "read timed out" : "no answer"); }
    int major, minor, status;
    if (sscanf(resp.p, "HTTP/%d.%d %d", &major, &minor, &status) != 3) { free(resp.p); return fail(out, "bad answer"); }
    char *b = resp.p + head_end;
    size_t blen = resp.n - head_end;
    const char *te = find_header(resp.p, "Transfer-Encoding");
    if (te && !strncmp(te, "chunked", 7)) {
        long d = dechunk(b, blen);
        if (d < 0) { free(resp.p); return fail(out, "truncated chunked body"); }
        blen = (size_t)d;
    } else if (want >= 0 && (long)resp.n < want) {
        free(resp.p);
        return fail(out, "truncated body");
    }
    if (blen) {
        out->body = malloc(blen + 1);
        if (out->body) {
            memcpy(out->body, b, blen);
            out->body[blen] = 0;
            out->len = blen;
        }
    }
    free(resp.p);
    out->status = status;
    return status;
}

void http_free(HttpResponse *r) {
    free(r->body);
    r->body = NULL;
    r->len = 0;
}
