// The local account identity and ID token (runtime/net/token.h).
//
// The token is a JWT: header {"alg":"HS256","typ":"JWT","kid":"recomp-local"}; claims {"iss":"recomp-local",
// "sub": the account ID (16 hex digits), "aud": the title ID, "iat", "exp" (+3 hours), "jti": random, "typ":
// "id_token", "nintendo": {"ai": title ID, "av": the game's version}, "tv": the game's version, and with
// nex_token_file= "nnex": that NEX login token}. The claims follow the shape of a network service account's ID token
// [inf: kinnay's NintendoClients wiki, "BaaS"]; "nnex" / "tv" are where a community NEX server looks for its own
// login token and the game version [inf: research/specs/recomp-nextendo.md §2.1, from their published docs]. The
// issuer and the key are ours: the game does not check the signature, a server checks its own token inside.
#include "token.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"

#include "../game.h"
#include "../hle.h"
#include "netcfg.h"

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

size_t net_id_token(char *out, size_t cap) {
    pthread_once(&once, load);
    if (cap < 8) return 0;
    const char *file = net_id_token_file();
    if (file) {
        size_t n = from_file(file, out, cap);
        if (n) { NLOG("[token] ID token from %s (%zu bytes)\n", file, n); return n; }
    }
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
