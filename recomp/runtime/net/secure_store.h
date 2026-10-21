// A tiny secret store for the Nextendo sign-in (nextendo.c): a few short strings kept by key. On iOS they live in
// the Keychain (secure_store_apple.m); everywhere else, macOS too, in net/nextendo.txt in the data folder (mode 0600).
// RECOMP_SECURE_STORE=file / keychain picks one on Apple. Values must not contain newlines.
#pragma once
#include <stdbool.h>
#include <stddef.h>

bool secure_get(const char *key, char *out, size_t cap);  // false: not stored (or does not fit)
bool secure_set(const char *key, const char *val);
void secure_del(const char *key);

#ifdef __APPLE__
// the Keychain backend (secure_store_apple.m); secure_store.c picks between it and the file
bool secure_apple_get(const char *key, char *out, size_t cap);
bool secure_apple_set(const char *key, const char *val);
void secure_apple_del(const char *key);
#endif
