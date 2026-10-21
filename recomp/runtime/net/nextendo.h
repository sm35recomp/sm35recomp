// The Nextendo Network account (nextendo.network): sign in in the system browser (OAuth 2.0 authorization code with
// PKCE, public client), keep the refresh token in the secret store (secure_store.h), and turn it into the game's NEX
// login token at launch. The browser redirects back to a loopback listener on 127.0.0.1 (desktop) or to a custom
// URL scheme handed in by the app (iOS). Tokens, codes and the PKCE verifier are never logged.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { NX_SIGNED_OUT, NX_WAITING_BROWSER, NX_SIGNED_IN, NX_ERROR } NxState;
typedef struct { NxState state; char username[64]; char message[160]; } NxStatus;

void nextendo_status(NxStatus *out);  // thread-safe snapshot, cheap enough for the UI each frame
bool nextendo_signed_in(void);        // a refresh token is stored

// Desktop (redirect_uri NULL): opens a listener on 127.0.0.1:<free port> (own thread, 5 min timeout) and returns the
// authorize URL to open. Starting again cancels a pending one (its port is closed). An app can pass its own redirect
// instead (a URL scheme): no listener, the app calls nextendo_finish_sign_in with the URL it gets back. (Nextendo
// accepts only the loopback redirect for an emulator-type client such as this one, so iOS uses the listener too.)
bool nextendo_begin_sign_in(const char *redirect_uri, char *url_out, size_t cap);
bool nextendo_finish_sign_in(const char *callback_url);  // "...?code=..&state=.."; the listener calls it too
void nextendo_sign_in_aborted(const char *msg);          // the sign-in sheet ended without a redirect: shown as the error
void nextendo_sign_out(void);                            // forget the tokens (store and memory), cancel a pending sign-in

// Launch: refresh -> access token -> GET /api/nex-token. Blocks for at most timeout_ms. True when a game token is
// ready; fetched again when the cached one has less than an hour left.
bool nextendo_prepare_game_login(int timeout_ms);
uint64_t nextendo_pid(void);
bool nextendo_username(char *out, size_t cap);
size_t nextendo_nex_token(char *out, size_t cap);  // the length (0: none)
// Why this account's online login is refused (GET /api/online-status with the game token; blocks up to timeout_ms):
// true and *allow / reason ("unknown", "disabled", "unverified", "discord_unlinked", "elsewhere"; "" when allowed),
// false when there is no answer
bool nextendo_online_status(int timeout_ms, bool *allow, char *reason, size_t cap);

// RFC 7636 S256: base64url(SHA-256(verifier)) without padding
void nx_pkce_challenge(const char *verifier, char out[44]);
// tests: forget what is in memory (not the store) and cancel a pending sign-in. The API base is "https://nextendo.network"
// unless RECOMP_NEXTENDO_API names a local address (a test server).
void nx_test_reset(void);
