// Tests for runtime/net/http.c (http:// and https://) and runtime/net/nextendo.c (sign-in, against the OAuth mock). Run by tools/test_net.sh, which starts the mocks.
// Usage: test_net <case>...   Environment: NET_TEST_TLS_PORT, NET_TEST_HTTP_PORT, RECOMP_HTTPS_CA (set per case),
// NET_TEST_CA (the throwaway CA's file), NET_TEST_OAUTH_PORT / NET_TEST_OFFLINE_PORT / NET_TEST_SLOW_PORT (the OAuth mocks).
#include <stdarg.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/x509_crt.h"

#include "../runtime/net/http.h"
#include "../runtime/net/nextendo.h"
#include "../runtime/net/roots.h"
#include "../runtime/net/secure_store.h"

// what netcfg.c / http.c need from the runtime. LOG is rt_log here: it also keeps everything in logbuf, which
// no_secret_in_logs searches.
static char logbuf[65536];
static size_t loglen;
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    if (loglen < sizeof logbuf) loglen += (size_t)vsnprintf(logbuf + loglen, sizeof logbuf - loglen, fmt, ap);
    va_end(ap);
}
bool game_net_config(const char *key, char *out, size_t cap) { (void)key, (void)out, (void)cap; return false; }
void game_net_config_set(const char *key, const char *val) { (void)key, (void)val; }
int game_net_default_backend(void) { return 0; }
const char *game_net_backend_label(int b) { (void)b; return NULL; }
const char *game_net_backend_desc(int b) { (void)b; return NULL; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", __func__); printf(__VA_ARGS__); printf("\n"); failures++; return; } } while (0)

static void url_for(char *u, size_t n, const char *fmt, const char *portenv) {
    const char *p = getenv(portenv);
    snprintf(u, n, fmt, p ? p : "0");
}

static void https_local_ok(void) {
    char u[128];
    url_for(u, sizeof u, "https://localhost:%s/hello", "NET_TEST_TLS_PORT");
    setenv("RECOMP_HTTPS_CA", getenv("NET_TEST_CA"), 1);
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    CHECK(st == 200, "status %d (%s)", st, r.error);
    CHECK(r.body && !strcmp(r.body, "hello"), "body %s", r.body ? r.body : "(none)");
    http_free(&r);
    unsetenv("RECOMP_HTTPS_CA");
}

static void https_untrusted(void) {
    char u[128];
    url_for(u, sizeof u, "https://localhost:%s/hello", "NET_TEST_TLS_PORT");
    unsetenv("RECOMP_HTTPS_CA");
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    CHECK(st == 0, "status %d, wanted a refusal", st);
    CHECK(strstr(r.error, "not trusted"), "error \"%s\"", r.error);
}

static void https_wrong_host(void) {  // the CA is right and the mock is reached, but the certificate names other.test
    char u[128];
    url_for(u, sizeof u, "https://localhost:%s/hello", "NET_TEST_BADHOST_PORT");
    setenv("RECOMP_HTTPS_CA", getenv("NET_TEST_CA"), 1);
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    unsetenv("RECOMP_HTTPS_CA");
    CHECK(st == 0, "status %d, wanted a refusal", st);
    CHECK(strstr(r.error, "not trusted"), "error \"%s\"", r.error);
}

static void embedded_roots_valid(void) {  // exactly the five roots, none expired
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    const char *pem = net_root_pems();
    int r = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1);
    CHECK(r == 0, "parse: %d", r);
    int n = 0;
    for (mbedtls_x509_crt *c = &crt; c && c->version; c = c->next) {
        n++;
        CHECK(!mbedtls_x509_time_is_past(&c->valid_to), "root %d expired", n);
        CHECK(!mbedtls_x509_time_is_future(&c->valid_from), "root %d not yet valid", n);
    }
    mbedtls_x509_crt_free(&crt);
    CHECK(n == 5, "%d roots, wanted 5", n);
}

static void https_live_nextendo(void) {
    unsetenv("RECOMP_HTTPS_CA");
    HttpResponse r;
    int st = http_request("GET", "https://nextendo.network/", NULL, NULL, 0, 15000, &r);
    CHECK(st >= 200 && st <= 399, "status %d (%s)", st, r.error);
    http_free(&r);
}

static void https_live_nextendo_embedded_only(void) {  // iOS / Android / Windows have no usable system bundle
    unsetenv("RECOMP_HTTPS_CA");
    setenv("RECOMP_HTTPS_NO_SYSTEM_ROOTS", "1", 1);
    HttpResponse r;
    int st = http_request("GET", "https://nextendo.network/", NULL, NULL, 0, 15000, &r);
    unsetenv("RECOMP_HTTPS_NO_SYSTEM_ROOTS");
    CHECK(st >= 200 && st <= 399, "status %d (%s)", st, r.error);
    http_free(&r);
}

static void https_live_selfsigned_refused(void) {
    unsetenv("RECOMP_HTTPS_CA");
    HttpResponse r;
    int st = http_request("GET", "https://51.178.29.194/", NULL, NULL, 0, 15000, &r);
    CHECK(st == 0, "status %d, wanted a refusal", st);
    printf("  (error: %s)\n", r.error);
}

static void http_plain_still_works(void) {
    char u[128];
    url_for(u, sizeof u, "http://127.0.0.1:%s/hello", "NET_TEST_HTTP_PORT");
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    CHECK(st == 200, "status %d (%s)", st, r.error);
    CHECK(r.body && !strcmp(r.body, "hello"), "body");
    http_free(&r);
}


// ---- Nextendo sign-in (runtime/net/nextendo.c) against the OAuth mock (net_mock.py) ----

// a fresh data folder, the file store, and the mock as the API
static void nx_setup(const char *portenv) {
    char dir[512], api[64];
    snprintf(dir, sizeof dir, "%s/nxtest-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir) || chdir(dir)) { perror("mkdtemp"); exit(2); }
    setenv("RECOMP_SECURE_STORE", "file", 1);
    snprintf(api, sizeof api, "http://127.0.0.1:%s", getenv(portenv));
    setenv("RECOMP_NEXTENDO_API", api, 1);
    nx_test_reset();
    loglen = 0;
}

static int nx_get(const char *path, char *out, size_t cap) {  // GET on the mock; the body into out
    char u[256];
    snprintf(u, sizeof u, "%s%s", getenv("RECOMP_NEXTENDO_API"), path);
    HttpResponse r;
    int st = http_request("GET", u, NULL, NULL, 0, 5000, &r);
    snprintf(out, cap, "%s", r.body ? r.body : "");
    http_free(&r);
    return st;
}

// the loopback port in an authorize URL's redirect_uri
static int redirect_port(const char *url) {
    const char *p = strstr(url, "127.0.0.1%3A");
    return p ? atoi(p + 12) : 0;
}

static bool port_open(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    bool ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

// what the browser does: GET the authorize URL (the mock answers with the redirect in the body), then GET that
static int nx_browser(const char *url) {
    HttpResponse r;
    int st = http_request("GET", url, NULL, NULL, 0, 5000, &r);
    if (st != 302 || !r.body) { http_free(&r); return st; }
    char loc[1024];
    snprintf(loc, sizeof loc, "%s", r.body);
    http_free(&r);
    st = http_request("GET", loc, NULL, NULL, 0, 5000, &r);
    http_free(&r);
    return st;
}

static bool nx_wait(NxState want, int ms, NxStatus *s) {
    for (int i = 0; i < ms / 20; i++) {
        nextendo_status(s);
        if (s->state == want) return true;
        usleep(20000);
    }
    nextendo_status(s);
    return s->state == want;
}

static bool nx_signin(const char *extra) {  // begin + browser + wait: true when signed in
    char url[1024];
    if (!nextendo_begin_sign_in(NULL, url, sizeof url)) return false;
    if (extra) strcat(url, extra);
    nx_browser(url);
    NxStatus s;
    return nx_wait(NX_SIGNED_IN, 5000, &s);
}

// (the RFC 7636 appendix B vector)
static void pkce_rfc7636(void) {
    char c[44];
    nx_pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", c);
    CHECK(!strcmp(c, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"), "challenge %s", c);
}

static void signin_loopback_full(void) {
    nx_setup("NET_TEST_OAUTH_PORT");
    CHECK(nx_signin(NULL), "not signed in");
    CHECK(nextendo_prepare_game_login(5000), "no game token");
    CHECK(nextendo_pid() == 1234567, "pid %llu", (unsigned long long)nextendo_pid());
    char u[64], t[64];
    CHECK(nextendo_username(u, sizeof u) && !strcmp(u, "tester"), "username %s", u);
    CHECK(nextendo_nex_token(t, sizeof t) && !strcmp(t, "nx2.test.sig"), "nex token %s", t);
    NxStatus s;
    nextendo_status(&s);
    CHECK(!strcmp(s.username, "tester"), "status user %s", s.username);
}

static void refresh_rotates_and_persists(void) {
    nx_setup("NET_TEST_OAUTH_PORT");
    CHECK(nx_signin(NULL), "not signed in");
    char latest[32], stored[64];
    for (int i = 0; i < 2; i++) {  // (a fresh module each time: it must come from the store)
        nx_test_reset();
        CHECK(nextendo_signed_in(), "round %d: nothing stored", i);
        CHECK(nextendo_prepare_game_login(5000), "round %d: no game token", i);
    }
    nx_get("/debug/latest-refresh", latest, sizeof latest);
    CHECK(!strcmp(latest, "R3"), "mock latest %s", latest);
    CHECK(secure_get("refresh_token", stored, sizeof stored) && !strcmp(stored, latest), "store %s, mock %s", stored, latest);
}

static void callback_state_mismatch(void) {
    nx_setup("NET_TEST_OAUTH_PORT");
    char url[1024];
    CHECK(nextendo_begin_sign_in(NULL, url, sizeof url), "begin");
    CHECK(!nextendo_finish_sign_in("http://127.0.0.1/callback?code=C1&state=bad"), "accepted a wrong state");
    NxStatus s;
    nextendo_status(&s);
    CHECK(s.state != NX_SIGNED_IN, "signed in");
    CHECK(!nextendo_signed_in(), "a token is stored");
}

static void callback_error_denied(void) {
    nx_setup("NET_TEST_OAUTH_PORT");
    char url[1024];
    CHECK(nextendo_begin_sign_in(NULL, url, sizeof url), "begin");
    int port = redirect_port(url);
    CHECK(port > 0 && port_open(port), "listener not open on %d", port);
    strcat(url, "&deny=1");
    nx_browser(url);
    NxStatus s;
    CHECK(nx_wait(NX_ERROR, 5000, &s), "state %d", s.state);
    CHECK(strstr(s.message, "denied"), "message \"%s\"", s.message);
    CHECK(!port_open(port), "listener still open");
}

static void begin_twice_cancels_first(void) {
    nx_setup("NET_TEST_OAUTH_PORT");
    char u1[1024], u2[1024];
    CHECK(nextendo_begin_sign_in(NULL, u1, sizeof u1), "begin 1");
    int p1 = redirect_port(u1);
    CHECK(p1 > 0 && port_open(p1), "first listener not open");
    CHECK(nextendo_begin_sign_in(NULL, u2, sizeof u2), "begin 2");
    int p2 = redirect_port(u2);
    CHECK(!port_open(p1), "first listener still open");
    CHECK(p2 > 0 && port_open(p2), "second listener not open");
    nextendo_sign_out();
    CHECK(!port_open(p2), "second listener open after sign-out");
}

static void prepare_offline_times_out(void) {
    nx_setup("NET_TEST_OFFLINE_PORT");
    CHECK(secure_set("refresh_token", "R1"), "store");
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    bool ok = nextendo_prepare_game_login(2000);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    CHECK(!ok, "succeeded offline");
    CHECK(s < 3.0, "took %.1f s", s);
    CHECK(nextendo_signed_in(), "the offline failure dropped the session");
}

static void no_secret_in_logs(void) {
    nx_setup("NET_TEST_OAUTH_PORT");
    CHECK(nx_signin(NULL), "not signed in");
    CHECK(nextendo_prepare_game_login(5000), "no game token");
    nx_test_reset();
    CHECK(nextendo_prepare_game_login(5000), "no game token after refresh");
    nextendo_sign_out();
    CHECK(loglen > 0, "nothing logged");
    static const char *const secrets[] = {"R1", "R2", "A1", "A2", "C1", "nx2.test.sig", "1234567"};
    for (size_t i = 0; i < sizeof secrets / sizeof *secrets; i++)
        CHECK(!strstr(logbuf, secrets[i]), "the log contains %s", secrets[i]);
}

static void *prepare_thread(void *arg) {
    *(bool *)arg = nextendo_prepare_game_login(8000);
    return NULL;
}

// a sign-out while a request is in flight must stay a sign-out: the late answer is dropped, not stored
static void signout_during_refresh(void) {
    nx_setup("NET_TEST_SLOW_PORT");
    char v[64], t[64];
    CHECK(nx_signin(NULL), "not signed in");  // (the listener's game-token fetch is slow: in flight now)
    usleep(300000);
    nextendo_sign_out();
    sleep(3);
    CHECK(!secure_get("refresh_token", v, sizeof v), "game-token answer re-stored the refresh token");
    CHECK(!secure_get("username", v, sizeof v), "game-token answer re-stored the user name");
    CHECK(nextendo_nex_token(t, sizeof t) == 0 && nextendo_pid() == 0, "game token survived the sign-out");
    // and a refresh in flight
    nx_test_reset();
    CHECK(secure_set("refresh_token", "R1"), "store");
    pthread_t th;
    bool ok = true;
    pthread_create(&th, NULL, prepare_thread, &ok);
    usleep(300000);
    nextendo_sign_out();
    pthread_join(th, NULL);
    CHECK(!ok, "prepare succeeded across a sign-out");
    CHECK(!secure_get("refresh_token", v, sizeof v), "refresh answer re-stored the refresh token");
    CHECK(nextendo_nex_token(t, sizeof t) == 0, "game token survived the sign-out");
    NxStatus s;
    nextendo_status(&s);
    CHECK(s.state == NX_SIGNED_OUT && !nextendo_signed_in(), "state %d", s.state);
}

// RECOMP_NEXTENDO_API names a local test server only: look-alike hosts fall back to the real one
static void api_override_rejects_lookalikes(void) {
    static const char *const bad[] = {"http://localhost.example.com", "http://127.0.0.1.example.com", "http://127.0.0.1x/",
                                      "ftp://127.0.0.1", "127.0.0.1", "http://8.8.8.8", "http://user@127.0.0.1.evil.test/"};
    nx_setup("NET_TEST_OAUTH_PORT");
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        setenv("RECOMP_NEXTENDO_API", bad[i], 1);
        char url[1024];
        CHECK(nextendo_begin_sign_in(NULL, url, sizeof url), "begin (%s)", bad[i]);
        CHECK(!strncmp(url, "https://nextendo.network/oauth/authorize?", 41), "%s accepted: %.60s", bad[i], url);
    }
    setenv("RECOMP_NEXTENDO_API", "http://localhost:1234", 1);  // (the genuine forms still work)
    char url[1024];
    CHECK(nextendo_begin_sign_in(NULL, url, sizeof url) && !strncmp(url, "http://localhost:1234/oauth/authorize?", 38), "localhost refused: %.60s", url);
    nextendo_sign_out();
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    {"https_local_ok", https_local_ok}, {"https_untrusted", https_untrusted}, {"https_wrong_host", https_wrong_host},
    {"embedded_roots_valid", embedded_roots_valid},
    {"https_live_nextendo", https_live_nextendo}, {"https_live_nextendo_embedded_only", https_live_nextendo_embedded_only}, {"https_live_selfsigned_refused", https_live_selfsigned_refused},
    {"http_plain_still_works", http_plain_still_works},
    {"pkce_rfc7636", pkce_rfc7636}, {"signin_loopback_full", signin_loopback_full},
    {"refresh_rotates_and_persists", refresh_rotates_and_persists}, {"callback_state_mismatch", callback_state_mismatch},
    {"callback_error_denied", callback_error_denied}, {"begin_twice_cancels_first", begin_twice_cancels_first},
    {"prepare_offline_times_out", prepare_offline_times_out}, {"no_secret_in_logs", no_secret_in_logs},
    {"signout_during_refresh", signout_during_refresh}, {"api_override_rejects_lookalikes", api_override_rejects_lookalikes},
};

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        int found = 0;
        for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
            if (strcmp(argv[i], cases[k].name)) continue;
            found = 1;
            int before = failures;
            cases[k].fn();
            printf("%s %s\n", failures == before ? "ok  " : "FAIL", cases[k].name);
        }
        if (!found) { printf("no case %s\n", argv[i]); failures++; }
    }
    return failures ? 1 : 0;
}
