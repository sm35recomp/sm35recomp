// A stand-in for runtime/net/nextendo.c in tools/test_netcfg.c: the netcfg / token / acc cases need a signed-in
// account with a known PID, user name and game token, offline. The test sets the stub_* values; the calls are counted
// (the default backend must never ask).
#include <stdio.h>
#include <string.h>

#include "../runtime/net/nextendo.h"

bool stub_signed_in;
uint64_t stub_pid;
const char *stub_username = "", *stub_nex = "";
int stub_signed_in_calls, stub_prepare_calls;

bool nextendo_signed_in(void) { stub_signed_in_calls++; return stub_signed_in; }
bool nextendo_prepare_game_login(int timeout_ms) { (void)timeout_ms; stub_prepare_calls++; return stub_signed_in && stub_nex[0]; }
uint64_t nextendo_pid(void) { return stub_signed_in ? stub_pid : 0; }

bool nextendo_username(char *out, size_t cap) {
    if (!stub_signed_in || !stub_username[0] || strlen(stub_username) >= cap) return false;
    strcpy(out, stub_username);
    return true;
}

size_t nextendo_nex_token(char *out, size_t cap) {
    size_t n = stub_signed_in ? strlen(stub_nex) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, stub_nex, n + 1);
    return n;
}
