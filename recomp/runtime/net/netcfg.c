// The game network stack's configuration (runtime/net/netcfg.h): backend, redirect table, destination policy, TLS
// trust. Read once, the first time anything asks.
//
// Nextendo's addresses and their game server's key [obs, 2026-10-20]: the game server 51.178.29.194:443 presents a
// self-signed certificate (CN=nintendo-private-server) over TLS 1.2; the pin is the SHA-256 of its
// SubjectPublicKeyInfo (`openssl x509 -pubkey | openssl pkey -pubin -outform der | sha256`). Their NAT check answers
// on 164.132.111.120 for nncs2-* (nncs1-* is the game server).
#include "netcfg.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "nextendo.h"

// a game's own settings store (runtime/game_hooks.c, weak: none): SM35's online.ini (games/sm35/net.c)
bool game_net_config(const char *key, char *out, size_t cap);
void game_net_config_set(const char *key, const char *val);
// a game's backend when none is set, and its own words for the choices (runtime/game_hooks.c: ours, and NULL = the
// text below)
int game_net_default_backend(void);
const char *game_net_backend_label(int backend);
const char *game_net_backend_desc(int backend);

#define MAX_RULES 32
typedef struct { char pattern[128]; uint32_t ip; } Rule;  // "name" or "*.domain"

#define NEXTENDO_SERVER "51.178.29.194"
#define NEXTENDO_NAT "164.132.111.120"
#define NEXTENDO_NAT_HOST "nncs2-lp1.n.n.srv.nintendo.net"
#define NEXTENDO_PIN "d2e7388e48ee30cfbecfbeb522874f2cc6b85f7aa1105c57669f3c2aa85fb30e"

static pthread_once_t once = PTHREAD_ONCE_INIT;
static NetBackend backend_set = NET_BACKEND_OURS, backend_eff = NET_BACKEND_OURS;
static NetTrust trust = NET_TRUST_LOCAL_CA;
static bool log_on;
static uint32_t test_ip = 0x7f000001;
static uint16_t test_port = 44300;
static Rule rules[MAX_RULES];
static int nrules;
static char ca_path[512] = "net/test-ca.pem", token_file[512], nex_token_file[512];
static uint32_t nx_server, nx_nat;  // nextendo: the game server and the NAT check (host order)
static uint8_t pin[32];

static pthread_mutex_t rev_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t ip; char name[128]; } rev[16];
static int nrev;

static const char *const backend_keys[NET_BACKEND_COUNT] = {"ours", "local-nex", "nextendo"};

static bool cfg(const char *env, const char *key, char *out, size_t cap) {
    const char *e = getenv(env);
    if (e) { snprintf(out, cap, "%s", e); return true; }
    return game_net_config(key, out, cap);
}

void ip_to_str(uint32_t ip, char out[16]) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool parse_ip(const char *s, uint32_t *ip) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    *ip = a << 24 | b << 16 | c << 8 | d;
    return true;
}

bool net_ip_is_local(uint32_t ip) {
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == (172 << 4 | 1) || (ip >> 16) == (192 << 8 | 168) ||
           (ip >> 16) == (169 << 8 | 254) || (ip >> 22) == (100u << 2 | 1);  // (100.64.0.0/10: CGNAT, e.g. Tailscale)
}

static bool parse_pin(const char *s, uint8_t out[32]) {
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 32; i++) {
        int h = hexval(s[2 * i]), l = hexval(s[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)(h << 4 | l);
    }
    return true;
}

// allow_public: the backend's own targets (Nextendo's servers); the user's net_hosts= entries stay local-only
static void add_rule(const char *pattern, uint32_t ip, const char *why, bool allow_public) {
    char a[16];
    ip_to_str(ip, a);
    if (!allow_public && !net_ip_is_local(ip)) {
        LOG("[net] redirect %s -> %s refused: not a local address (only loopback / private test servers)\n", pattern, a);
        return;
    }
    if (nrules == MAX_RULES) return;
    snprintf(rules[nrules].pattern, sizeof rules[nrules].pattern, "%s", pattern);
    for (char *p = rules[nrules].pattern; *p; p++) *p = (char)tolower((unsigned char)*p);
    rules[nrules++].ip = ip;
    if (log_on) LOG("[net] redirect %s -> %s (%s)\n", pattern, a, why);  // (inside load: not NLOG)
}

static void load(void) {
    char v[1024];
    if (cfg("RECOMP_NET_LOG", "net_log", v, sizeof v)) log_on = atoi(v) != 0;
    int def = game_net_default_backend();
    if (def > 0 && def < NET_BACKEND_COUNT) backend_set = (NetBackend)def;
    if (cfg("RECOMP_NET_BACKEND", "backend", v, sizeof v)) {
        for (int i = 0; i < NET_BACKEND_COUNT; i++)
            if (!strcmp(v, backend_keys[i])) backend_set = (NetBackend)i;
        if (!strcmp(v, "localnex") || !strcmp(v, "local_nex")) backend_set = NET_BACKEND_LOCAL_NEX;
    }
    backend_eff = backend_set;
    if (backend_set == NET_BACKEND_NEXTENDO && !nextendo_signed_in()) {  // (the store only: no network)
        LOG("[net] backend nextendo: not signed in; %s\n", game_net_backend_label(NET_BACKEND_OURS) ? "offline" : "using our server");
        backend_eff = NET_BACKEND_OURS;
    }
    if (cfg("RECOMP_NET_TEST_SERVER", "nex_test_server", v, sizeof v)) {
        uint32_t ip;
        char *colon = strchr(v, ':');
        if (colon) *colon = 0, test_port = (uint16_t)atoi(colon + 1);
        if (parse_ip(v, &ip) && net_ip_is_local(ip)) test_ip = ip;
        else LOG("[net] nex_test_server=%s: a local IPv4 address wanted; using 127.0.0.1\n", v);
        if (!test_port) test_port = 443;
    }
    if (cfg("RECOMP_NET_TLS_TRUST", "tls_trust", v, sizeof v)) trust = !strcmp(v, "any") ? NET_TRUST_ANY : NET_TRUST_LOCAL_CA;
    if (cfg("RECOMP_NET_TLS_CA", "tls_ca", v, sizeof v) && v[0]) snprintf(ca_path, sizeof ca_path, "%s", v);
    if (cfg("RECOMP_NET_ID_TOKEN_FILE", "id_token_file", v, sizeof v)) snprintf(token_file, sizeof token_file, "%s", v);
    if (cfg("RECOMP_NET_NEX_TOKEN_FILE", "nex_token_file", v, sizeof v)) snprintf(nex_token_file, sizeof nex_token_file, "%s", v);
    if (backend_eff == NET_BACKEND_OURS) return;  // (no name resolves, no game socket reaches the host)
    // explicit entries first (first match wins), then the game's hosts -> the test server / Nextendo
    if (cfg("RECOMP_NET_HOSTS", "net_hosts", v, sizeof v)) {
        for (char *tok = strtok(v, ";, "); tok; tok = strtok(NULL, ";, ")) {
            char *eq = strchr(tok, '=');
            uint32_t ip;
            if (!eq || !parse_ip(eq + 1, &ip)) { LOG("[net] net_hosts entry \"%s\": name=ip wanted\n", tok); continue; }
            *eq = 0;
            add_rule(tok, ip, "net_hosts", false);
        }
    }
    static const char *const game_domains[] = {"*.nintendo.net", "*.nintendo.com", "*.nintendowifi.net",
                                               "*.nintendo.co.jp", "*.nintendoswitch.cn"};
    char a[16];
    if (backend_eff == NET_BACKEND_NEXTENDO) {
        parse_ip(NEXTENDO_SERVER, &nx_server), parse_ip(NEXTENDO_NAT, &nx_nat), parse_pin(NEXTENDO_PIN, pin);
        uint32_t ip;
        if (cfg("RECOMP_NEXTENDO_SERVER", "nextendo_server", v, sizeof v)) {
            if (parse_ip(v, &ip) && ip) nx_server = ip;
            else LOG("[net] nextendo_server=%s: an IPv4 address wanted; using " NEXTENDO_SERVER "\n", v);
        }
        if (cfg("RECOMP_NEXTENDO_NAT", "nextendo_nat", v, sizeof v)) {
            if (parse_ip(v, &ip) && ip) nx_nat = ip;
            else LOG("[net] nextendo_nat=%s: an IPv4 address wanted; using " NEXTENDO_NAT "\n", v);
        }
        if (cfg("RECOMP_NEXTENDO_TLS_PIN", "nextendo_tls_pin", v, sizeof v) && !parse_pin(v, pin))
            LOG("[net] nextendo_tls_pin: 64 hex digits wanted (the SPKI SHA-256); using the built-in pin\n");
        add_rule(NEXTENDO_NAT_HOST, nx_nat, "Nextendo NAT check", true);
        for (size_t i = 0; i < sizeof game_domains / sizeof *game_domains; i++)
            add_rule(game_domains[i], nx_server, "Nextendo game server", true);
        trust = NET_TRUST_PINNED;
        ip_to_str(nx_server, a);
        LOG("[net] backend nextendo: the game's own NEX client against Nextendo Network (game server %s, TLS: pinned "
            "certificate)\n", a);
        return;
    }
    for (size_t i = 0; i < sizeof game_domains / sizeof *game_domains; i++) add_rule(game_domains[i], test_ip, "test server", false);
    ip_to_str(test_ip, a);
    LOG("[net] backend local-nex: the game's own NEX client against the local test server %s:%u (TLS trust: %s)\n", a, test_port,
        trust == NET_TRUST_ANY ? "any certificate" : "local test CA");
}

static void init(void) { pthread_once(&once, load); }

NetBackend net_backend(void) { init(); return backend_eff; }
NetBackend net_backend_setting(void) { init(); return backend_set; }
const char *net_backend_key(NetBackend b) { return b < NET_BACKEND_COUNT ? backend_keys[b] : "?"; }
bool net_real_sockets(void) { NetBackend b = net_backend(); return b == NET_BACKEND_LOCAL_NEX || b == NET_BACKEND_NEXTENDO; }
bool net_log_on(void) { init(); return log_on; }

// (the PID is the player's account id: the TLS plaintext dump would show it in every NEX station URL)
void net_log_redact(const char *in, char *out, size_t cap) {
    static const char mask[] = "<hidden>";
    size_t o = 0;
    if (!cap) return;
    while (*in && o + 1 < cap) {
        if (tolower((unsigned char)in[0]) == 'p' && tolower((unsigned char)in[1]) == 'i' &&
            tolower((unsigned char)in[2]) == 'd' && in[3] == '=' && isdigit((unsigned char)in[4])) {
            for (int i = 0; i < 4 && o + 1 < cap; i++) out[o++] = *in++;
            while (isdigit((unsigned char)*in)) in++;
            for (const char *m = mask; *m && o + 1 < cap; m++) out[o++] = *m;
            continue;
        }
        out[o++] = *in++;
    }
    out[o] = 0;
}
uint32_t net_test_server_ip(void) { init(); return test_ip; }
uint16_t net_dest_port(uint32_t ip, uint16_t port) {
    init();
    return backend_eff == NET_BACKEND_LOCAL_NEX && ip == test_ip && port == 443 ? test_port : port;
}
NetTrust net_tls_trust(void) { init(); return trust; }
const char *net_tls_ca_path(void) { init(); return ca_path; }
const char *net_id_token_file(void) { init(); return token_file[0] ? token_file : NULL; }
const char *net_nex_token_file(void) { init(); return nex_token_file[0] ? nex_token_file : NULL; }
const char *net_nsd_environment(void) { return "lp1"; }

bool net_tls_pin(uint8_t out[32]) {
    init();
    if (trust != NET_TRUST_PINNED) return false;
    memcpy(out, pin, 32);
    return true;
}

static NetBackendOption opt_tables[2][NET_BACKEND_COUNT] = {
    {{NET_BACKEND_OURS, "ours", "Our server",
      "Course World against your course server (the mod's server= setting), answered by the mod. Nothing else "
      "goes online.", true},
     {NET_BACKEND_LOCAL_NEX, "local-nex", "Local test server",
      "The game's own online code talks to a NEX test server on this computer or your local network "
      "(tools/nex_test_server). For development.", true},
     {NET_BACKEND_NEXTENDO, "nextendo", "Nextendo Network",
      "The game's online play on Nextendo's community servers. Needs a Nextendo account (sign in below).", false}}};

static void words(void) {  // the second table (signed in), and the game's own label / description for each choice
    opt_tables[1][0] = opt_tables[0][0], opt_tables[1][1] = opt_tables[0][1], opt_tables[1][2] = opt_tables[0][2];
    opt_tables[1][NET_BACKEND_NEXTENDO].available = true;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < NET_BACKEND_COUNT; i++) {
            const char *l = game_net_backend_label(i), *d = game_net_backend_desc(i);
            if (l) opt_tables[k][i].label = l;
            if (d) opt_tables[k][i].desc = d;
        }
}

int net_backend_options(const NetBackendOption **out) {
    static pthread_once_t o = PTHREAD_ONCE_INIT;
    pthread_once(&o, words);
    *out = opt_tables[nextendo_signed_in() ? 1 : 0];
    return NET_BACKEND_COUNT;
}

bool net_backend_select(NetBackend b) {
    const NetBackendOption *o;
    int n = net_backend_options(&o);
    if ((int)b >= n || !o[b].available) return false;
    game_net_config_set("backend", backend_keys[b]);
    LOG("[net] backend \"%s\" from the next start\n", backend_keys[b]);
    return true;
}

static bool match(const char *pattern, const char *name) {
    if (pattern[0] == '*' && pattern[1] == '.') {
        size_t pl = strlen(pattern + 1), nl = strlen(name);  // ".domain": the domain itself or any name under it
        return (nl > pl && !strcmp(name + nl - pl, pattern + 1)) || !strcmp(name, pattern + 2);
    }
    return !strcmp(pattern, name);
}

static void remember(uint32_t ip, const char *name) {
    pthread_mutex_lock(&rev_lock);
    int i = 0;
    while (i < nrev && rev[i].ip != ip) i++;
    if (i == nrev) i = nrev < 16 ? nrev++ : 15;
    rev[i].ip = ip;
    snprintf(rev[i].name, sizeof rev[i].name, "%s", name);
    pthread_mutex_unlock(&rev_lock);
}

bool net_resolve(const char *name, uint32_t *ip) {
    init();
    char n[256];
    size_t len = 0;
    // (a '%' is nsd's environment, as nsd resolves it: the game asks sfdnsres for "nncs2-%.n.n.srv.nintendo.net")
    for (const char *p = name; *p && len < sizeof n - 1; p++)
        if (*p == '%')
            for (const char *e = net_nsd_environment(); *e && len < sizeof n - 1; e++) n[len++] = *e;
        else n[len++] = (char)tolower((unsigned char)*p);
    n[len] = 0;
    while (len && n[len - 1] == '.') n[--len] = 0;  // (an absolute name)
    if (parse_ip(n, ip)) return true;
    if (!strcmp(n, "localhost")) { *ip = 0x7f000001; return true; }
    for (int i = 0; i < nrules; i++)
        if (match(rules[i].pattern, n)) {
            *ip = rules[i].ip;
            remember(*ip, n);
            return true;
        }
    return false;
}

bool net_name_for_ip(uint32_t ip, char *out, size_t cap) {
    pthread_mutex_lock(&rev_lock);
    bool found = false;
    for (int i = 0; i < nrev && !found; i++)
        if (rev[i].ip == ip) snprintf(out, cap, "%s", rev[i].name), found = true;
    pthread_mutex_unlock(&rev_lock);
    return found;
}

// (nextendo: TCP to Nextendo's game server and their NAT check only, not to net_hosts= targets)
bool net_dest_allowed(uint32_t ip) {
    init();
    if (backend_eff == NET_BACKEND_OURS) return false;
    if (backend_eff == NET_BACKEND_NEXTENDO) return ip == nx_server || ip == nx_nat;
    if (backend_eff == NET_BACKEND_LOCAL_NEX && (ip >> 24) == 127) return true;
    for (int i = 0; i < nrules; i++)
        if (rules[i].ip == ip) return true;
    return false;
}

bool net_dest_allowed_udp(uint32_t ip) {
    init();
    if (backend_eff == NET_BACKEND_NEXTENDO) return true;  // (PIA: peer to peer, the addresses come from the server)
    return net_dest_allowed(ip);
}
