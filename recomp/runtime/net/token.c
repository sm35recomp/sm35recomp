// The local account identity and ID token (runtime/net/token.h).
//
// The token is a JWT: header {"alg":"HS256","typ":"JWT","kid":"recomp-local"}; claims {"iss":"recomp-local",
// "sub": the account ID (16 hex digits), "aud": the title ID, "iat", "exp" (+3 hours), "jti": random, "typ":
// "id_token", "nintendo": {"ai": title ID, "av": the game's version}, "tv": the game's version, and with
// nex_token_file= "nnex": that NEX login token}. The claims follow the shape of a network service account's ID token
// [inf: kinnay's NintendoClients wiki, "BaaS"]; "nnex" / "tv" are where a community NEX server looks for its own
// login token and the game version [inf: research/specs/recomp-nextendo.md §2.1, from their published docs]. The
// issuer and the key are ours: the game does not check the signature, a server checks its own token inside.
//
// Backend nextendo: an RS256 JWT shaped like the network service account's ID token [inf: kinnay's NintendoClients
// wiki, "BaaS"; research/specs/recomp-nextendo.md §2.1]: header {"alg":"RS256","typ":"JWT","kid":
// "nextendo-baas-key-1"}; claims "iss" (the BaaS issuer), "jku" (its certificates), "aud", "sub": the Nextendo PID
// (16 hex digits), "iat", "exp" (+3 hours), "jti": random, "typ": "id_token", "di" / "bs:did": a device id (random,
// kept), "sn", "hm", "nintendo" {"ai", "av"}, "tv" and "nnex": the account's game token (runtime/net/nextendo.h),
// which is what Nextendo's server checks. The RSA-2048 key is made once (about a second) and kept with the device id
// in net/nextendo-jwt-key.pem (private). Without a game token there is no ID token (the game reports its own login
// error); nnex is never empty. The PID, the game token and the token itself are never logged.
#include "token.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/rsa.h"
#include "psa/crypto.h"

#include "../game.h"
#include "../hle.h"
#include "netcfg.h"
#include "nextendo.h"

#define ACCOUNT_FILE "net/account.txt"

static pthread_once_t once = PTHREAD_ONCE_INIT;
static uint64_t nsa_id;
static uint8_t key[32];
static bool have_key;

static void random_bytes(uint8_t *out, size_t n) {
    mbedtls_entropy_context e;
    mbedtls_ctr_drbg_context d;
    mbedtls_entropy_init(&e);
    mbedtls_ctr_drbg_init(&d);
    static const char pers[] = GAME_EXE " account";
    if (mbedtls_ctr_drbg_seed(&d, mbedtls_entropy_func, &e, (const unsigned char *)pers, sizeof pers) ||
        mbedtls_ctr_drbg_random(&d, out, n)) {
        for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(rand() ^ (int)time(NULL) >> (i & 7));  // (no entropy: weak)
    }
    mbedtls_ctr_drbg_free(&d);
    mbedtls_entropy_free(&e);
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void load(void) {
    FILE *f = fopen(ACCOUNT_FILE, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            unsigned long long id;
            if (sscanf(line, "nsa_id %llx", &id) == 1) nsa_id = id;
            if (!strncmp(line, "key ", 4)) {
                have_key = true;
                for (int i = 0; i < 32; i++) {
                    int h = hexval(line[4 + 2 * i]), l = h < 0 ? -1 : hexval(line[5 + 2 * i]);
                    if (h < 0 || l < 0) { have_key = false; break; }
                    key[i] = (uint8_t)(h << 4 | l);
                }
            }
        }
        fclose(f);
    }
    if (nsa_id && have_key) return;
    random_bytes((uint8_t *)&nsa_id, sizeof nsa_id);
    random_bytes(key, sizeof key);
    have_key = true;
    mkdir("net", 0700);  // (Windows: the runtime's compat header takes the mode too)
    if ((f = fopen(ACCOUNT_FILE, "w"))) {
        fprintf(f, "# The local network service account for the game's own online code (runtime/net/token.h). "
                   "Private: the key signs this account's ID tokens.\nnsa_id %016llx\nkey ", (unsigned long long)nsa_id);
        for (int i = 0; i < 32; i++) fprintf(f, "%02x", key[i]);
        fprintf(f, "\n");
        fclose(f);
#ifndef _WIN32
        chmod(ACCOUNT_FILE, 0600);
#endif
        LOG("[token] new local account %016llx (" ACCOUNT_FILE ")\n", (unsigned long long)nsa_id);
    }
}

uint64_t net_nsa_id(void) {
    pthread_once(&once, load);
    return nsa_id;
}

static size_t b64url(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        int k = n - i >= 3 ? 4 : (int)(n - i) + 1;
        for (int j = 0; j < k && o + 1 < cap; j++) out[o++] = t[v >> (18 - 6 * j) & 63];
    }
    out[o] = 0;
    return o;
}

static size_t from_file(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) { LOG("[token] id_token_file %s: not found; using the local token\n", path); return 0; }
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) n--;
    out[n] = 0;
    return n;
}

// ---- backend nextendo: the RS256 token ----

#define JWT_KEY_FILE "net/nextendo-jwt-key.pem"
#define BAAS_ISS "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com"

static pthread_once_t jwt_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t jwt_lock = PTHREAD_MUTEX_INITIALIZER;  // (Mbed TLS contexts: one user at a time)
static mbedtls_pk_context jwt_key;
static mbedtls_entropy_context jwt_entropy;
static mbedtls_ctr_drbg_context jwt_drbg;
static bool jwt_ok;
static char device_id[17];

static bool jwt_key_read(void) {
    FILE *f = fopen(JWT_KEY_FILE, "rb");
    if (!f) return false;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *di = strstr(buf, "\ndi ");
    bool ok = di && strspn(di + 4, "0123456789abcdef") == 16;
    if (ok) memcpy(device_id, di + 4, 16), device_id[16] = 0;
    // (the comment and the device id come before the PEM block; the parser looks for its BEGIN line)
    ok = ok && !mbedtls_pk_parse_key(&jwt_key, (const unsigned char *)buf, n + 1, NULL, 0, mbedtls_ctr_drbg_random, &jwt_drbg) &&
         mbedtls_pk_get_type(&jwt_key) == MBEDTLS_PK_RSA && mbedtls_pk_get_bitlen(&jwt_key) == 2048;
    mbedtls_platform_zeroize(buf, sizeof buf);
    if (!ok) mbedtls_pk_free(&jwt_key), mbedtls_pk_init(&jwt_key);
    return ok;
}

static bool jwt_key_make(void) {
    LOG("[token] making the Nextendo ID token key (RSA-2048, once)\n");
    if (mbedtls_pk_setup(&jwt_key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) ||
        mbedtls_rsa_gen_key(mbedtls_pk_rsa(jwt_key), mbedtls_ctr_drbg_random, &jwt_drbg, 2048, 65537))
        return false;
    uint8_t d[8];
    random_bytes(d, sizeof d);
    for (int i = 0; i < 8; i++) snprintf(device_id + 2 * i, 3, "%02x", d[i]);
    static unsigned char pem[4096];
    if (mbedtls_pk_write_key_pem(&jwt_key, pem, sizeof pem)) return false;
    const char *tmp = JWT_KEY_FILE ".tmp";
    mkdir("net", 0700);  // (Windows: the runtime's compat header takes the mode too)
#ifdef _WIN32
    FILE *f = fopen(tmp, "w");
#else
    unlink(tmp);  // (a stale file keeps its old mode; a symlink is never followed)
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
    if (fd >= 0 && !f) close(fd);
#endif
    bool ok = f != NULL;
    if (f) {
        fprintf(f, "# The key that signs this device's Nextendo ID tokens (runtime/net/token.c). Private.\ndi %s\n%s",
                device_id, (const char *)pem);
        ok = fclose(f) == 0;
    }
    mbedtls_platform_zeroize(pem, sizeof pem);
#ifdef _WIN32
    if (ok) ok = MoveFileExA(tmp, JWT_KEY_FILE, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    if (ok) ok = rename(tmp, JWT_KEY_FILE) == 0;
#endif
    if (!ok) remove(tmp), LOG("[token] could not save " JWT_KEY_FILE " (the key is used for this run only)\n");
    return true;
}

static void jwt_load(void) {
    psa_crypto_init();
    mbedtls_pk_init(&jwt_key);
    mbedtls_entropy_init(&jwt_entropy);
    mbedtls_ctr_drbg_init(&jwt_drbg);
    static const char pers[] = GAME_EXE " nextendo id token";
    if (mbedtls_ctr_drbg_seed(&jwt_drbg, mbedtls_entropy_func, &jwt_entropy, (const unsigned char *)pers, sizeof pers)) {
        LOG("[token] no random generator: no Nextendo ID token\n");
        return;
    }
    jwt_ok = jwt_key_read() || jwt_key_make();
    if (!jwt_ok) LOG("[token] the Nextendo ID token key could not be made\n");
}

static void uuid4(char out[37]) {
    uint8_t b[16];
    random_bytes(b, sizeof b);
    b[6] = (uint8_t)(b[6] & 0x0f) | 0x40, b[8] = (uint8_t)(b[8] & 0x3f) | 0x80;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static size_t nextendo_id_token(char *out, size_t cap) {
    char nex[1200];
    size_t tn = nextendo_nex_token(nex, sizeof nex);
    bool ok = tn > 0;
    for (size_t i = 0; i < tn; i++) ok &= nex[i] > ' ' && nex[i] != '"' && nex[i] != '\\';
    uint64_t pid = nextendo_pid();
    if (!ok || !pid) {
        LOG("[token] no Nextendo game token%s: no ID token (the game reports its login error)\n", tn && !ok ? " (not a token)" : "");
        mbedtls_platform_zeroize(nex, sizeof nex);
        return 0;
    }
    pthread_once(&jwt_once, jwt_load);
    if (!jwt_ok) return 0;
    char jti[37];
    uuid4(jti);
    long long now = (long long)time(NULL);
    char claims[2048];
    int cn = snprintf(claims, sizeof claims,
                      "{\"iss\":\"" BAAS_ISS "\",\"jku\":\"" BAAS_ISS "/1.0.0/certificates\",\"aud\":\"ed9e2f05d286f7b8\","
                      "\"sub\":\"%016llx\",\"iat\":%lld,\"exp\":%lld,\"jti\":\"%s\",\"typ\":\"id_token\",\"di\":\"%s\","
                      "\"bs:did\":\"%s\",\"sn\":\"XAW10000000000\",\"hm\":true,\"nintendo\":{\"ai\":\"%016llx\",\"av\":\"%s\"},"
                      "\"tv\":\"%s\",\"nnex\":\"%s\"}",
                      (unsigned long long)pid, now, now + 3 * 3600, jti, device_id, device_id,
                      (unsigned long long)GAME_TITLE_ID, GAME_DISPLAY_VERSION, GAME_DISPLAY_VERSION, nex);
    mbedtls_platform_zeroize(nex, sizeof nex);
    static const char header[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"nextendo-baas-key-1\"}";
    size_t n = 0, bufcap = 4096;
    char *buf = malloc(bufcap);
    if (cn > 0 && (size_t)cn < sizeof claims) {
        n = b64url((const uint8_t *)header, strlen(header), buf, bufcap);
        buf[n++] = '.';
        n += b64url((const uint8_t *)claims, (size_t)cn, buf + n, bufcap - n);
    }
    mbedtls_platform_zeroize(claims, sizeof claims);
    uint8_t hash[32], sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sn = 0;
    int r = -1;
    if (n && n + 1 + 344 < bufcap) {  // (a 256-byte signature: 342 characters)
        mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)buf, n, hash);
        pthread_mutex_lock(&jwt_lock);
        r = mbedtls_pk_sign(&jwt_key, MBEDTLS_MD_SHA256, hash, sizeof hash, sig, sizeof sig, &sn, mbedtls_ctr_drbg_random, &jwt_drbg);
        pthread_mutex_unlock(&jwt_lock);
    }
    if (r == 0) {
        buf[n++] = '.';
        n += b64url(sig, sn, buf + n, bufcap - n);
    }
    if (r || n >= cap) {
        LOG("[token] Nextendo ID token: %s\n", r ? "could not sign it" : "too long for the game's buffer");
        n = 0;
    } else {
        memcpy(out, buf, n + 1);
    }
    mbedtls_platform_zeroize(buf, bufcap);
    free(buf);
    NLOG("[token] Nextendo ID token (RS256, %zu bytes, game token %zu chars)\n", n, tn);
    return n;
}

// ---- the token for the backend in effect ----

size_t net_id_token(char *out, size_t cap) {
    bool nx = net_backend() == NET_BACKEND_NEXTENDO;
    if (!nx) pthread_once(&once, load);  // (nextendo: the account is Nextendo's, no local one is made)
    if (cap < 8) return 0;
    const char *file = net_id_token_file();
    if (file) {
        size_t n = from_file(file, out, cap);
        if (n) { NLOG("[token] ID token from %s (%zu bytes)\n", file, n); return n; }
    }
    if (nx) return nextendo_id_token(out, cap);
    uint8_t jti[12];
    random_bytes(jti, sizeof jti);
    char jtis[17];
    b64url(jti, sizeof jti, jtis, sizeof jtis);
    long long now = (long long)time(NULL);
    char nnex[512] = "";
    const char *nf = net_nex_token_file();
    if (nf) {
        char t[400];
        size_t tn = from_file(nf, t, sizeof t);
        bool ok = tn > 0;
        for (size_t i = 0; i < tn; i++) ok &= t[i] > ' ' && t[i] != '"' && t[i] != '\\';
        if (ok) snprintf(nnex, sizeof nnex, ",\"nnex\":\"%s\"", t);
        else if (tn) LOG("[token] nex_token_file: not a token (left out)\n");
    }
    char claims[1024];
    snprintf(claims, sizeof claims,
             "{\"iss\":\"recomp-local\",\"sub\":\"%016llx\",\"aud\":\"%016llx\",\"iat\":%lld,\"exp\":%lld,"
             "\"jti\":\"%s\",\"typ\":\"id_token\",\"nintendo\":{\"ai\":\"%016llx\",\"av\":\"%s\"},\"tv\":\"%s\"%s}",
             (unsigned long long)nsa_id, (unsigned long long)GAME_TITLE_ID, now, now + 3 * 3600, jtis,
             (unsigned long long)GAME_TITLE_ID, GAME_DISPLAY_VERSION, GAME_DISPLAY_VERSION, nnex);
    static const char header[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\",\"kid\":\"recomp-local\"}";
    char *buf = malloc(4096);
    size_t n = b64url((const uint8_t *)header, strlen(header), buf, 4096);
    buf[n++] = '.';
    n += b64url((const uint8_t *)claims, strlen(claims), buf + n, 4096 - n);
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, sizeof key, (const uint8_t *)buf, n, mac);
    buf[n++] = '.';
    n += b64url(mac, sizeof mac, buf + n, 4096 - n);
    if (n >= cap) n = 0;
    else memcpy(out, buf, n + 1);
    free(buf);
    NLOG("[token] local ID token for account %016llx (%zu bytes)\n", (unsigned long long)nsa_id, n);
    return n;
}
