// The game's own network stack (runtime/services/bsd.c sockets, sfdnsres.c names, nsd.c, ssl.c TLS, acc.c token):
// which backend it talks to, which names resolve where, which addresses a game socket may reach, and how TLS
// servers are trusted. Deny by default: nothing here ever asks the host's DNS for a name the game gives, and a game
// socket reaches only the addresses the redirect table names (and, with nextendo, PIA peers over UDP).
//
// Backends (setting "backend"):
//   ours       the game's own choice without NEX (SMM2: its mods answer Course World from our course server over
//              HTTP; SM35: offline); the game's own sockets stay off (only the virtual LAN, runtime/net/vlan.h). The
//              default unless the game names another (game_net_default_backend: SM35 says nextendo)
//   local-nex  the game's own NEX client runs (the mods' online overrides switch off) against a local NEX test
//              server (tools/nex_test_server): real TCP / UDP sockets, the game's hosts resolve to that server
//   nextendo   Nextendo Network (runtime/net/nextendo.h): the game's own NEX client against Nextendo's game server.
//              Needs a signed-in Nextendo account; without one it is logged and the backend is "ours". The game's
//              hosts resolve to Nextendo's game server (nncs2-lp1.n.n.srv.nintendo.net: their NAT check), TCP
//              reaches those two only (not net_hosts= targets), UDP any address (PIA's peers), and the game server's TLS certificate is
//              pinned (its public key's SHA-256). The account's PID, user name and an ID token carrying the
//              account's game token come from acc (services/acc.c, runtime/net/token.c)
//
// Settings: environment variable, else the game's settings (game_net_config: online.ini for SM35, games/sm35/net.c):
//   RECOMP_NET_BACKEND        backend=           ours | local-nex | nextendo
//   RECOMP_NET_TEST_SERVER    nex_test_server=   the local NEX test server, ip[:port] (default 127.0.0.1:44300); must
//                                                be a local address (loopback, private, link-local, CGNAT 100.64/10).
//                                                The game's TCP connections to it on port 443 go to that port (443
//                                                needs privileges on most hosts).
//   RECOMP_NET_HOSTS          net_hosts=         extra redirect entries "name=ip;*.domain=ip" (local addresses only;
//                                                they come before the backend's own)
//   RECOMP_NET_TLS_TRUST      tls_trust=         local-nex: local-ca (default): server certificates must chain to the
//                                                local test CA; any: accept any certificate (only towards local
//                                                addresses). (nextendo: always the pin)
//   RECOMP_NET_TLS_CA         tls_ca=            the local test CA (PEM), default net/test-ca.pem in the data folder
//   RECOMP_NEXTENDO_SERVER    nextendo_server=   nextendo: the game server's address (default 51.178.29.194)
//   RECOMP_NEXTENDO_NAT       nextendo_nat=      nextendo: the NAT check's address (default 164.132.111.120)
//   RECOMP_NEXTENDO_TLS_PIN   nextendo_tls_pin=  nextendo: the game server's SPKI SHA-256, 64 hex digits
//   RECOMP_NET_ID_TOKEN_FILE  id_token_file=     a backend-issued account id token to send instead of the local one
//   RECOMP_NET_NEX_TOKEN_FILE nex_token_file=    local-nex: a backend's NEX login token, carried in the local ID
//                                                token's "nnex" claim (runtime/net/token.c)
//   RECOMP_NET_LOG            net_log=1          log every socket, name, TLS and token call
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { NET_BACKEND_OURS = 0, NET_BACKEND_LOCAL_NEX = 1, NET_BACKEND_NEXTENDO = 2, NET_BACKEND_COUNT } NetBackend;
typedef enum { NET_TRUST_LOCAL_CA = 0, NET_TRUST_ANY = 1, NET_TRUST_PINNED = 2 } NetTrust;

NetBackend net_backend(void);          // in effect (nextendo without a signed-in account -> ours)
NetBackend net_backend_setting(void);  // as set
const char *net_backend_key(NetBackend b);  // "ours", "local-nex", "nextendo"
bool net_real_sockets(void);           // the game's sockets reach the host's network stack (local-nex, nextendo)
bool net_log_on(void);
// A log line with the player id masked: every "PID=<digits>" (any case; NEX station URLs carry it) becomes
// "PID=<hidden>". Always NUL-terminated within cap; a cut digit run is dropped, never printed.
void net_log_redact(const char *in, char *out, size_t cap);
#define NLOG(...) do { if (net_log_on()) LOG(__VA_ARGS__); } while (0)

// For a settings page: the backends, which can be chosen now, and choosing one (saved; takes effect at the next
// start). Nextendo is available while a Nextendo account is signed in.
typedef struct { NetBackend id; const char *key, *label, *desc; bool available; } NetBackendOption;
int net_backend_options(const NetBackendOption **out);
bool net_backend_select(NetBackend b);  // false: not available

// Names. Returns true and the IPv4 address (host order) the redirect table gives `name`, false = no such name
// (every name not in the table). Literal dotted addresses resolve to themselves.
bool net_resolve(const char *name, uint32_t *ip);
// The last name resolved to ip (TLS: the game connects by address and names the address as the host); false = none
bool net_name_for_ip(uint32_t ip, char *out, size_t cap);
// May a game socket connect / send to ip (host order)? TCP (and anything not UDP): local-nex: loopback and the
// redirect table's targets; nextendo: Nextendo's game server and NAT check only. UDP: the same with local-nex; any
// address with nextendo (PIA's peers).
bool net_dest_allowed(uint32_t ip);
bool net_dest_allowed_udp(uint32_t ip);
bool net_ip_is_local(uint32_t ip);     // loopback, private, link-local, CGNAT
uint32_t net_test_server_ip(void);     // host order
// The port a game connection to ip:port really goes to (the test server's port stands in for 443)
uint16_t net_dest_port(uint32_t ip, uint16_t port);
const char *net_nsd_environment(void); // the nsd environment that replaces '%' in the game's host names ("lp1")

// TLS
NetTrust net_tls_trust(void);         // nextendo: NET_TRUST_PINNED
const char *net_tls_ca_path(void);
bool net_tls_pin(uint8_t out[32]);     // NET_TRUST_PINNED: the server certificate's SPKI SHA-256 to require
// (runtime/services/ssl.c) true when the certificate's public key (SubjectPublicKeyInfo, DER) hashes to pin
struct mbedtls_x509_crt;
bool ssl_pin_check(const struct mbedtls_x509_crt *peer, const uint8_t pin[32]);
const char *net_id_token_file(void);   // NULL: none (the local token, runtime/net/token.c)
const char *net_nex_token_file(void);  // NULL: none (local-nex only)

void ip_to_str(uint32_t ip, char out[16]);
