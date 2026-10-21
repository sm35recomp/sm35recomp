// The account's online identity for the game's own NEX client (runtime/services/acc.c, backends local-nex /
// nextendo): a network service account ID and an ID token (nextendo: an RS256 token carrying the Nextendo account's
// game token, runtime/net/token.c; the ID is the account's PID, runtime/net/nextendo.h).
//
// For now both are local: the ID is a random 64-bit number and the token a JWT signed (HS256) with a random local
// key, both made on first use and kept in net/account.txt in the data folder (private; never logged). The local NEX
// test server (tools/nex_test_server) reads the same file to check the signature. A backend that issues real tokens
// plugs in through id_token_file= (runtime/net/netcfg.h): its token is sent as it is.
#pragma once
#include <stddef.h>
#include <stdint.h>

uint64_t net_nsa_id(void);
// The current ID token (NUL-terminated) into out; returns its length (0: none)
size_t net_id_token(char *out, size_t cap);
