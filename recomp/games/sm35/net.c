// The online backend for Super Mario Bros. 35 (runtime/net/netcfg.h). The game is online-only in practice: even
// Course Practice asks for its online service, so the backend is Nextendo Network by default, and runs while a
// Nextendo account is signed in (the Online settings). Signed out, nothing goes online ("Offline").
//
// The settings live in online.ini in the data folder (environment variables win, as netcfg.h lists):
//
//   backend=nextendo | ours | local-nex     RECOMP_NET_BACKEND   (ours: offline)
//   nex_test_server=<ip>                    RECOMP_NET_TEST_SERVER
//   net_hosts=<name=ip;...>                 RECOMP_NET_HOSTS
//   tls_trust=local-ca | any                RECOMP_NET_TLS_TRUST
//   tls_ca=<path>                           RECOMP_NET_TLS_CA
//   id_token_file=<path>                    RECOMP_NET_ID_TOKEN_FILE
//   nex_token_file=<path>                   RECOMP_NET_NEX_TOKEN_FILE
//   net_log=1                               RECOMP_NET_LOG
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/net/netcfg.h"

static const char *ini_path(void) { return getenv("RECOMP_NET_INI") ? getenv("RECOMP_NET_INI") : "online.ini"; }

bool game_net_config(const char *key, char *out, size_t cap) {
    FILE *f = fopen(ini_path(), "r");
    if (!f) return false;
    char line[1024];
    bool found = false;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || strncmp(line, key, kl) || line[kl] != '=') continue;
        char *v = line + kl + 1, *e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        snprintf(out, cap, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}

// replace or add one key=value line (written to a temporary file, then renamed over the old one)
void game_net_config_set(const char *key, const char *val) {
    const char *path = ini_path();
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    size_t kl = strlen(key);
    bool done = false;
    char line[1024];
    if (!in) fprintf(out, "# Online settings (games/sm35/net.c)\n");
    while (in && fgets(line, sizeof line, in)) {
        if (line[0] != '#' && !strncmp(line, key, kl) && line[kl] == '=') {
            if (!done) fprintf(out, "%s=%s\n", key, val);
            done = true;
            continue;
        }
        fputs(line, out);
    }
    if (!done) fprintf(out, "%s=%s\n", key, val);
    if (in) fclose(in);
    if (fclose(out) == 0) {
        remove(path);  // (Windows: rename does not replace)
        rename(tmp, path);
    }
}

int game_net_default_backend(void) { return NET_BACKEND_NEXTENDO; }

const char *game_net_backend_label(int b) { return b == NET_BACKEND_OURS ? "Offline" : NULL; }

const char *game_net_backend_desc(int b) {
    switch (b) {
    case NET_BACKEND_OURS:
        return "Nothing goes online. The game needs its online service even for Course Practice, so courses don't "
               "start: the game reports that it can't connect.";
    case NET_BACKEND_NEXTENDO:
        return "35-player battles, Course Practice and the game's daily data on Nextendo Network's community servers. "
               "Needs a Nextendo account (sign in below).";
    default: return NULL;
    }
}
