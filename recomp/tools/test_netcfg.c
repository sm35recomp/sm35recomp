// Tests for the backends in runtime/net/netcfg.c (routing, destinations, trust), the Nextendo ID token
// (runtime/net/token.c) and the certificate pin check (runtime/services/ssl.c), with a stand-in for the Nextendo
// account (tools/test_nextendo_stub.c). netcfg reads its settings once, so every case runs in a child process of its
// own, which sets the environment first. Run by tools/test_net.sh.
// Usage: test_netcfg <case>...   Environment: NET_TEST_LEAF (Task 1's local test leaf), NET_TEST_LEAF_PIN (its SPKI
// SHA-256 as openssl computes it).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "../runtime/net/json.h"
#include "../runtime/net/netcfg.h"
#include "../runtime/net/token.h"

// the stand-in account (test_nextendo_stub.c)
extern bool stub_signed_in;
extern uint64_t stub_pid;
extern const char *stub_username, *stub_nex;
extern int stub_signed_in_calls, stub_prepare_calls;

// what netcfg.c / token.c / ssl.c need from the runtime. LOG is rt_log here: it also keeps everything in logbuf.
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
// ssl.c's service side is not exercised here (only ssl_pin_check)
typedef struct Service Service;
typedef struct IpcResp IpcResp;
typedef struct BsdSock BsdSock;
Service *service_new(const char *name, void *h, void *state) { (void)name, (void)h, (void)state; return NULL; }
void ipc_return_object(IpcResp *resp, Service *s) { (void)resp, (void)s; }
BsdSock *bsd_sock_ref(int fd) { (void)fd; return NULL; }
void bsd_sock_unref(BsdSock *s) { (void)s; }
uint32_t bsd_sock_peer_ip(BsdSock *s) { (void)s; return 0; }
int bsd_sock_send(BsdSock *s, const void *d, size_t n) { (void)s, (void)d, (void)n; return -2; }
int bsd_sock_recv(BsdSock *s, void *d, size_t n) { (void)s, (void)d, (void)n; return -2; }
int bsd_sock_wait(BsdSock *s, bool w, int ms) { (void)s, (void)w, (void)ms; return -1; }
uint64_t plat_now_ns(void) { return 0; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", __func__); printf(__VA_ARGS__); printf("\n"); failures++; return; } } while (0)

#define SERVER 0x33b21dc2u  // 51.178.29.194
#define NAT 0xa4846f78u     // 164.132.111.120
static const char NEXTENDO_PIN[] = "d2e7388e48ee30cfbecfbeb522874f2cc6b85f7aa1105c57669f3c2aa85fb30e";

static bool unhex(const char *s, uint8_t *out, size_t n) {
    if (!s || strlen(s) != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static void stub_account(void) {
    stub_signed_in = true;
    stub_pid = 0x00000001a2b3c4d5ull;
    stub_username = "StubPlayer";
    stub_nex = "nx2.stubgametoken.stubsig";
}

static void nextendo_env(void) {
    setenv("RECOMP_NET_BACKEND", "nextendo", 1);
    stub_account();
}

static bool resolves(const char *name, uint32_t want) {
    uint32_t ip = 0;
    return net_resolve(name, &ip) && ip == want;
}

// ---- backends and routing ----

static void ours_default_untouched(void) {  // the default asks nothing, resolves nothing, reaches nothing
    stub_account();
    uint32_t ip;
    CHECK(net_backend() == NET_BACKEND_OURS, "backend %d", net_backend());
    CHECK(!net_real_sockets(), "real sockets");
    CHECK(!net_resolve("g22306d00-lp1.s.n.srv.nintendo.net", &ip), "a Nintendo name resolved");
    CHECK(!net_dest_allowed(SERVER) && !net_dest_allowed_udp(SERVER) && !net_dest_allowed(0x7f000001), "a destination allowed");
    CHECK(stub_signed_in_calls == 0, "the account was asked %d times", stub_signed_in_calls);
}

static void nextendo_routes(void) {
    nextendo_env();
    uint32_t ip;
    CHECK(net_backend() == NET_BACKEND_NEXTENDO, "backend %d", net_backend());
    CHECK(net_real_sockets(), "no real sockets");
    CHECK(resolves("g22306d00-lp1.s.n.srv.nintendo.net", SERVER), "game server");
    CHECK(resolves("nncs2-lp1.n.n.srv.nintendo.net", NAT), "NAT check");
    CHECK(resolves("nncs1-lp1.n.n.srv.nintendo.net", SERVER), "nncs1");
    CHECK(resolves("api.accounts.nintendo.com", SERVER), "*.nintendo.com");
    CHECK(!net_resolve("www.google.com", &ip), "www.google.com resolved");
    CHECK(!net_resolve("nintendo.net.evil.test", &ip), "a look-alike resolved");
    const NetBackendOption *o;
    int n = net_backend_options(&o);
    CHECK(n == 3 && o[2].id == NET_BACKEND_NEXTENDO && o[2].available, "nextendo not available when signed in");
    CHECK(!strcmp(o[2].desc, "The game's online play on Nextendo's community servers. Needs a Nextendo account (sign "
                             "in below)."), "desc %s", o[2].desc);
}

static void nextendo_hosts_local_only(void) {  // net_hosts= stays local-only; it comes first
    nextendo_env();
    setenv("RECOMP_NET_HOSTS", "evil.test=8.8.8.8;dev.test=192.168.1.5;g22306d00-lp1.s.n.srv.nintendo.net=10.0.0.7", 1);
    uint32_t ip;
    CHECK(!net_resolve("evil.test", &ip), "a public net_hosts entry resolved");
    CHECK(resolves("dev.test", 0xc0a80105), "dev.test");
    CHECK(resolves("g22306d00-lp1.s.n.srv.nintendo.net", 0x0a000007), "net_hosts is not first");
    CHECK(resolves("nncs1-lp1.n.n.srv.nintendo.net", SERVER), "the built-in targets after it");
    CHECK(!net_dest_allowed(0xc0a80105) && !net_dest_allowed(0x0a000007), "TCP to a net_hosts target allowed");
    CHECK(net_dest_allowed(SERVER) && net_dest_allowed(NAT), "TCP to Nextendo refused");
}

static void nextendo_dest(void) {
    nextendo_env();
    CHECK(net_dest_allowed(SERVER), "TCP to the game server refused");
    CHECK(net_dest_allowed(NAT), "TCP to the NAT check refused");
    CHECK(!net_dest_allowed(0x08080808), "TCP to 8.8.8.8 allowed");
    CHECK(!net_dest_allowed(0x7f000001), "TCP to loopback allowed");
    CHECK(net_dest_allowed_udp(0xcb007105), "UDP to 203.0.113.5 (a PIA peer) refused");
    CHECK(net_tls_trust() == NET_TRUST_PINNED, "trust %d", net_tls_trust());
    uint8_t pin[32], want[32];
    unhex(NEXTENDO_PIN, want, 32);
    CHECK(net_tls_pin(pin) && !memcmp(pin, want, 32), "pin");
}

static void nextendo_overrides(void) {
    nextendo_env();
    setenv("RECOMP_NEXTENDO_SERVER", "198.51.100.7", 1);
    setenv("RECOMP_NEXTENDO_NAT", "198.51.100.8", 1);
    setenv("RECOMP_NEXTENDO_TLS_PIN", "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", 1);
    CHECK(resolves("g22306d00-lp1.s.n.srv.nintendo.net", 0xc6336407), "server override");
    CHECK(resolves("nncs2-lp1.n.n.srv.nintendo.net", 0xc6336408), "NAT override");
    CHECK(net_dest_allowed(0xc6336407) && !net_dest_allowed(SERVER), "destinations follow the override");
    uint8_t pin[32], want[32];
    unhex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", want, 32);
    CHECK(net_tls_pin(pin) && !memcmp(pin, want, 32), "pin override");
}

static void nextendo_not_signed_in_falls_back(void) {
    setenv("RECOMP_NET_BACKEND", "nextendo", 1);
    stub_signed_in = false;
    uint32_t ip;
    CHECK(net_backend() == NET_BACKEND_OURS, "backend %d", net_backend());
    CHECK(net_backend_setting() == NET_BACKEND_NEXTENDO, "setting %d", net_backend_setting());
    CHECK(!net_real_sockets(), "real sockets");
    CHECK(!net_resolve("g22306d00-lp1.s.n.srv.nintendo.net", &ip), "resolved");
    CHECK(!net_dest_allowed(SERVER) && !net_dest_allowed_udp(0xcb007105), "a destination allowed");
    CHECK(strstr(logbuf, "[net] backend nextendo: not signed in; using our server"), "log");
    const NetBackendOption *o;
    net_backend_options(&o);
    CHECK(!o[2].available, "nextendo available while signed out");
    CHECK(!net_backend_select(NET_BACKEND_NEXTENDO), "selected while signed out");
}

// ---- the ID token ----

static size_t b64url_dec(const char *s, size_t n, uint8_t *out) {
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '-' ? 62 : c == '_' ? 63 : -1;
        if (v < 0) return 0;
        acc = acc << 6 | (uint32_t)v, bits += 6;
        if (bits >= 8) out[o++] = (uint8_t)(acc >> (bits -= 8));
    }
    return o;
}

static int rng(void *p, unsigned char *out, size_t n) { return mbedtls_ctr_drbg_random(p, out, n); }

static void id_token_rs256(void) {
    nextendo_env();
    setenv("RECOMP_NET_LOG", "1", 1);
    char tok[2048];
    size_t n = net_id_token(tok, sizeof tok);
    CHECK(n > 0 && n < 2048 && strlen(tok) == n, "length %zu", n);
    char *d1 = strchr(tok, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    CHECK(d1 && d2 && !strchr(d2 + 1, '.'), "not three parts");
    static uint8_t hdr[512], cl[2048], sig[512];
    size_t hn = b64url_dec(tok, (size_t)(d1 - tok), hdr), cn = b64url_dec(d1 + 1, (size_t)(d2 - d1 - 1), cl),
           sn = b64url_dec(d2 + 1, strlen(d2 + 1), sig);
    Json *h = json_parse((const char *)hdr, hn), *c = json_parse((const char *)cl, cn);
    CHECK(h && c, "header / claims not JSON");
    CHECK(!strcmp(json_str(json_get(h, "alg"), ""), "RS256"), "alg %s", json_str(json_get(h, "alg"), "?"));
    CHECK(!strcmp(json_str(json_get(h, "kid"), ""), "nextendo-baas-key-1"), "kid");
    CHECK(!strcmp(json_str(json_get(c, "nnex"), ""), stub_nex), "nnex %s", json_str(json_get(c, "nnex"), "?"));
    CHECK(!strcmp(json_str(json_get(c, "sub"), ""), "00000001a2b3c4d5"), "sub %s", json_str(json_get(c, "sub"), "?"));
    const char *iss = json_str(json_get(c, "iss"), "");
    CHECK(!strcmp(iss, "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com"), "iss %s", iss);
    char jku[160];
    snprintf(jku, sizeof jku, "%s/1.0.0/certificates", iss);
    CHECK(!strcmp(json_str(json_get(c, "jku"), ""), jku), "jku");
    CHECK(!strcmp(json_str(json_get(c, "aud"), ""), "ed9e2f05d286f7b8"), "aud");
    CHECK(!strcmp(json_str(json_get(c, "typ"), ""), "id_token") && !strcmp(json_str(json_get(c, "sn"), ""), "XAW10000000000"), "typ / sn");
    CHECK(json_bool(json_get(c, "hm"), false), "hm");
    const char *di = json_str(json_get(c, "di"), "");
    CHECK(strlen(di) == 16 && !strcmp(di, json_str(json_get(c, "bs:did"), "")), "di %s", di);
    double iat = json_num(json_get(c, "iat"), 0), exp = json_num(json_get(c, "exp"), 0);
    CHECK(iat > 1.7e9 && exp == iat + 3 * 3600, "iat %.0f exp %.0f", iat, exp);
    CHECK(strlen(json_str(json_get(c, "jti"), "")) >= 16, "jti");
    CHECK(json_get(json_get(c, "nintendo"), "ai") && json_get(c, "tv"), "nintendo / tv");
    json_free(h), json_free(c);
    // the signature, with the stored key's public half
    struct stat st;
    CHECK(stat("net/nextendo-jwt-key.pem", &st) == 0 && (st.st_mode & 0777) == 0600, "key file mode %o", st.st_mode & 0777);
    psa_crypto_init();
    mbedtls_entropy_context e;
    mbedtls_ctr_drbg_context dr;
    mbedtls_entropy_init(&e);
    mbedtls_ctr_drbg_init(&dr);
    mbedtls_ctr_drbg_seed(&dr, mbedtls_entropy_func, &e, NULL, 0);
    mbedtls_pk_context key, pub;
    mbedtls_pk_init(&key), mbedtls_pk_init(&pub);
    int r = mbedtls_pk_parse_keyfile(&key, "net/nextendo-jwt-key.pem", NULL, rng, &dr);
    CHECK(r == 0 && mbedtls_pk_get_type(&key) == MBEDTLS_PK_RSA && mbedtls_pk_get_bitlen(&key) == 2048, "key: %d", r);
    static uint8_t der[1024];
    int dn = mbedtls_pk_write_pubkey_der(&key, der, sizeof der);
    CHECK(dn > 0 && mbedtls_pk_parse_public_key(&pub, der + sizeof der - dn, (size_t)dn) == 0, "public half");
    uint8_t hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)tok, (size_t)(d2 - tok), hash);
    r = mbedtls_pk_verify(&pub, MBEDTLS_MD_SHA256, hash, 32, sig, sn);
    CHECK(r == 0 && sn == 256, "signature: %d (%zu bytes)", r, sn);
    // a second token: the same key, a new jti, the same device id
    char tok2[2048];
    CHECK(net_id_token(tok2, sizeof tok2) > 0 && strcmp(tok, tok2), "second token");
    mbedtls_pk_free(&key), mbedtls_pk_free(&pub);
    // nothing secret in the log
    char pidhex[17];
    snprintf(pidhex, sizeof pidhex, "%016llx", (unsigned long long)stub_pid);
    CHECK(!strstr(logbuf, stub_nex) && !strstr(logbuf, pidhex) && !strstr(logbuf, "a2b3c4d5"), "a secret in the log");
    CHECK(!strstr(logbuf, tok) && !strstr(logbuf, d2 + 1), "the token in the log");
}

static const char *claim_di(const char *tok, char out[17]) {  // the "di" claim of a token
    static uint8_t cl[2048];
    const char *d1 = strchr(tok, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    size_t cn = d2 ? b64url_dec(d1 + 1, (size_t)(d2 - d1 - 1), cl) : 0;
    Json *c = json_parse((const char *)cl, cn);
    snprintf(out, 17, "%s", json_str(json_get(c, "di"), ""));
    json_free(c);
    return out;
}

static void id_token_key_kept(void) {  // a second start reads the key and the device id; it makes no new key
    nextendo_env();
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {  // (the first start, in this data folder)
        char t[2048], di[17];
        FILE *f = fopen("first-di", "w");
        if (net_id_token(t, sizeof t) && f) fputs(claim_di(t, di), f);
        if (f) fclose(f);
        _exit(0);
    }
    waitpid(p, NULL, 0);
    char first[17] = "", tok[2048], di[17];
    FILE *f = fopen("first-di", "r");
    CHECK(f && fgets(first, sizeof first, f), "the first start made no token");
    fclose(f);
    loglen = 0;
    CHECK(net_id_token(tok, sizeof tok) > 0, "no token");
    CHECK(!strcmp(claim_di(tok, di), first), "device id %s, first %s", di, first);
    CHECK(!strstr(logbuf, "making the Nextendo ID token key"), "a new key was made");
}

static void id_token_no_nex_token(void) {  // no game token: no ID token at all (never an empty nnex)
    nextendo_env();
    stub_nex = "";
    char tok[2048];
    CHECK(net_id_token(tok, sizeof tok) == 0, "a token without nnex");
}

// ---- the pin (ssl.c) ----

static void pin_mismatch_refused(void) {
    mbedtls_x509_crt leaf;
    mbedtls_x509_crt_init(&leaf);
    const char *path = getenv("NET_TEST_LEAF");
    CHECK(path && mbedtls_x509_crt_parse_file(&leaf, path) == 0, "no local test leaf (%s)", path ? path : "NET_TEST_LEAF unset");
    uint8_t nx[32], own[32];
    unhex(NEXTENDO_PIN, nx, 32);
    CHECK(unhex(getenv("NET_TEST_LEAF_PIN"), own, 32), "NET_TEST_LEAF_PIN");
    CHECK(!ssl_pin_check(&leaf, nx), "the local test leaf passed Nextendo's pin");
    CHECK(ssl_pin_check(&leaf, own), "the leaf's own pin refused (the SPKI hash is wrong)");
    CHECK(!ssl_pin_check(NULL, nx), "no certificate passed");
    mbedtls_x509_crt_free(&leaf);
}

static int bio_send(void *p, const unsigned char *b, size_t n) { long r = send(*(int *)p, b, n, 0); return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)r; }
static int bio_recv(void *p, unsigned char *b, size_t n) { long r = recv(*(int *)p, b, n, 0); return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)r; }

static void pin_live_ok(void) {  // a handshake with Nextendo's game server (nothing is sent after it)
    psa_crypto_init();
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET, a.sin_port = htons(443), a.sin_addr.s_addr = htonl(SERVER);
    struct timeval tv = {15, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(connect(s, (struct sockaddr *)&a, sizeof a) == 0, "connect");
    mbedtls_entropy_context e;
    mbedtls_ctr_drbg_context dr;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_init(&e), mbedtls_ctr_drbg_init(&dr), mbedtls_ssl_init(&ssl), mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_seed(&dr, mbedtls_entropy_func, &e, NULL, 0);
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&conf, rng, &dr);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);  // (as ssl.c does for NET_TRUST_PINNED)
    mbedtls_ssl_setup(&ssl, &conf);
    mbedtls_ssl_set_hostname(&ssl, "g22306d00-lp1.s.n.srv.nintendo.net");
    mbedtls_ssl_set_bio(&ssl, &s, bio_send, bio_recv, NULL);
    int r = mbedtls_ssl_handshake(&ssl);
    close(s);
    CHECK(r == 0, "handshake: -0x%x", -r);
    uint8_t pin[32];
    unhex(NEXTENDO_PIN, pin, 32);
    CHECK(ssl_pin_check(mbedtls_ssl_get_peer_cert(&ssl), pin), "Nextendo's certificate failed the pin");
    printf("  (%s, %s)\n", mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));
    mbedtls_ssl_free(&ssl), mbedtls_ssl_config_free(&conf);
}

// The game asks sfdnsres for the NAT check's names with nsd's '%' still in them ("nncs2-%.n.n.srv.nintendo.net"):
// '%' is the nsd environment ("lp1") there too, so nncs2 reaches the NAT check and nncs1 the game server.
static void nextendo_percent_names(void) {
    nextendo_env();
    CHECK(resolves("nncs2-%.n.n.srv.nintendo.net", NAT), "nncs2-%% not to the NAT check");
    CHECK(resolves("nncs1-%.n.n.srv.nintendo.net", SERVER), "nncs1-%% not to the game server");
    CHECK(resolves("g22306d00-%.s.n.srv.nintendo.net", SERVER), "g22306d00-%% not to the game server");
    CHECK(resolves("NNCS2-%.N.N.SRV.NINTENDO.NET.", NAT), "upper case / absolute nncs2-%%");
}

// The net log's plaintext dump (ssl.c) masks the player id in NEX station URLs: "PID=<digits>" -> "PID=<hidden>"
static void log_redacts_pid(void) {
    char out[256];
    net_log_redact("prudp:/address=1.2.3.4;port=1;PID=1234567890;RVCID=1140;sid=2", out, sizeof out);
    CHECK(!strcmp(out, "prudp:/address=1.2.3.4;port=1;PID=<hidden>;RVCID=1140;sid=2"), "got %s", out);
    net_log_redact("a pid=42 and PID=7 and PID= and PIDX=5 ...PID=99", out, sizeof out);
    CHECK(!strcmp(out, "a pid=<hidden> and PID=<hidden> and PID= and PIDX=5 ...PID=<hidden>"), "got %s", out);
    net_log_redact("no ids here", out, sizeof out);
    CHECK(!strcmp(out, "no ids here"), "got %s", out);
    char small[16];
    net_log_redact("PID=123456789012345", small, sizeof small);
    CHECK(strlen(small) < sizeof small && !strpbrk(small, "0123456789"), "small buffer leaked digits: %s", small);
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    {"nextendo_percent_names", nextendo_percent_names}, {"log_redacts_pid", log_redacts_pid},
    {"ours_default_untouched", ours_default_untouched}, {"nextendo_routes", nextendo_routes},
    {"nextendo_hosts_local_only", nextendo_hosts_local_only}, {"nextendo_dest", nextendo_dest},
    {"nextendo_overrides", nextendo_overrides}, {"nextendo_not_signed_in_falls_back", nextendo_not_signed_in_falls_back},
    {"id_token_rs256", id_token_rs256}, {"id_token_key_kept", id_token_key_kept}, {"id_token_no_nex_token", id_token_no_nex_token},
    {"pin_mismatch_refused", pin_mismatch_refused}, {"pin_live_ok", pin_live_ok},
};

int main(int argc, char **argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        int found = 0;
        for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
            if (strcmp(argv[i], cases[k].name)) continue;
            found = 1;
            fflush(stdout);
            pid_t p = fork();
            if (p == 0) {  // a fresh process: netcfg not read yet, an empty data folder
                char dir[512];
                snprintf(dir, sizeof dir, "%s/nxcfg-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
                if (!mkdtemp(dir) || chdir(dir)) { perror("mkdtemp"); _exit(2); }
                cases[k].fn();
                fflush(stdout);
                _exit(failures ? 1 : 0);
            }
            int st = 0;
            waitpid(p, &st, 0);
            bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
            printf("%s %s\n", ok ? "ok  " : "FAIL", cases[k].name);
            bad += !ok;
        }
        if (!found) { printf("no case %s\n", argv[i]); bad++; }
    }
    return bad ? 1 : 0;
}
