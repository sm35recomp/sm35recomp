// The game's own network stack (runtime/services/bsd.c sockets, sfdnsres.c names, nsd.c, ssl.c TLS, acc.c token):
// which backend it talks to, which names resolve where, which addresses a game socket may reach, and how TLS
// servers are trusted. Deny by default: nothing here ever asks the host's DNS for a name the game gives, and a game
// socket reaches only loopback and the local addresses the redirect table names.
//
// Backends (setting "backend"):
//   ours       (default) the current mod path: the mods answer Course World from our course server over HTTP
//              (games/smm2/mods); the game's own sockets stay off (only the virtual LAN, runtime/net/vlan.h)
//   local-nex  the game's own NEX client runs (the mods' online overrides switch off) against a local NEX test
//              server (tools/nex_test_server): real TCP / UDP sockets, the game's hosts resolve to that server
//   nextendo   Nextendo Network: not available until the owner has Nextendo's permission and an app registration;
//              chosen, it is refused (logged) and the backend stays "ours"
//
// Settings: environment variable, else the game's settings (game_net_config: smm2mods.ini for SMM2):
//   RECOMP_NET_BACKEND        backend=           ours | local-nex | nextendo (refused)
//   RECOMP_NET_TEST_SERVER    nex_test_server=   the local NEX test server, ip[:port] (default 127.0.0.1:44300); must
//                                                be a local address (loopback, private, link-local, CGNAT 100.64/10).
//                                                The game's TCP connections to it on port 443 go to that port (443
//                                                needs privileges on most hosts).
//   RECOMP_NET_HOSTS          net_hosts=         extra redirect entries "name=ip;*.domain=ip" (local addresses only)
//   RECOMP_NET_TLS_TRUST      tls_trust=         local-ca (default): server certificates must chain to the local test
//                                                CA; any: accept any certificate (only towards local addresses)
//   RECOMP_NET_TLS_CA         tls_ca=            the local test CA (PEM), default net/test-ca.pem in the data folder
//   RECOMP_NET_ID_TOKEN_FILE  id_token_file=     a backend-issued account id token to send instead of the local one
//   RECOMP_NET_NEX_TOKEN_FILE nex_token_file=    a backend's NEX login token, carried in the local ID token's "nnex"
//                                                claim (the shape a community server reads; runtime/net/token.c)
//   RECOMP_NET_LOG            net_log=1          log every socket, name, TLS and token call
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { NET_BACKEND_OURS = 0, NET_BACKEND_LOCAL_NEX = 1, NET_BACKEND_NEXTENDO = 2, NET_BACKEND_COUNT } NetBackend;
typedef enum { NET_TRUST_LOCAL_CA = 0, NET_TRUST_ANY = 1 } NetTrust;

NetBackend net_backend(void);          // in effect (nextendo refused -> ours)
NetBackend net_backend_setting(void);  // as set
const char *net_backend_key(NetBackend b);  // "ours", "local-nex", "nextendo"
bool net_real_sockets(void);           // the game's sockets reach the host's network stack (local-nex)
bool net_log_on(void);
#define NLOG(...) do { if (net_log_on()) LOG(__VA_ARGS__); } while (0)

// For a settings page: the backends, which can be chosen now, and choosing one (saved; takes effect at the next
// start). Nextendo is listed, unavailable ("awaiting registration").
typedef struct { NetBackend id; const char *key, *label, *desc; bool available; } NetBackendOption;
int net_backend_options(const NetBackendOption **out);
bool net_backend_select(NetBackend b);  // false: not available

// Names. Returns true and the IPv4 address (host order) the redirect table gives `name`, false = no such name
// (every name not in the table). Literal dotted addresses resolve to themselves.
bool net_resolve(const char *name, uint32_t *ip);
// The last name resolved to ip (TLS: the game connects by address and names the address as the host); false = none
bool net_name_for_ip(uint32_t ip, char *out, size_t cap);
// May a game socket connect / send to ip (host order)? Loopback and the redirect table's targets only.
bool net_dest_allowed(uint32_t ip);
bool net_ip_is_local(uint32_t ip);     // loopback, private, link-local, CGNAT
uint32_t net_test_server_ip(void);     // host order
// The port a game connection to ip:port really goes to (the test server's port stands in for 443)
uint16_t net_dest_port(uint32_t ip, uint16_t port);
const char *net_nsd_environment(void); // the nsd environment that replaces '%' in the game's host names ("lp1")

// TLS
NetTrust net_tls_trust(void);
const char *net_tls_ca_path(void);
const char *net_id_token_file(void);   // NULL: none (the local token, runtime/net/token.c)
const char *net_nex_token_file(void);  // NULL: none

void ip_to_str(uint32_t ip, char out[16]);
