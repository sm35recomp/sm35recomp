// The Keychain backend of the secret store (runtime/net/secure_store.h): one generic password item per key, service
// GAME_BUNDLE_ID ".nextendo", readable after the first unlock (the game may start before the device is unlocked
// again). Compiled on macOS / iOS only; secure_store.c chooses between this and the file.
#include "secure_store.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "../game.h"

static NSMutableDictionary *query(const char *key) {
    return [@{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: @GAME_BUNDLE_ID ".nextendo",
        (__bridge id)kSecAttrAccount: [NSString stringWithUTF8String:key],
    } mutableCopy];
}

bool secure_apple_get(const char *key, char *out, size_t cap) {
    NSMutableDictionary *q = query(key);
    q[(__bridge id)kSecReturnData] = @YES;
    q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef res = NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &res) != errSecSuccess || !res) return false;
    NSData *d = CFBridgingRelease(res);
    if (d.length >= cap) return false;
    memcpy(out, d.bytes, d.length);
    out[d.length] = 0;
    return true;
}

bool secure_apple_set(const char *key, const char *val) {
    NSMutableDictionary *q = query(key);
    NSData *data = [NSData dataWithBytes:val length:strlen(val)];
    OSStatus st = SecItemUpdate((__bridge CFDictionaryRef)q, (__bridge CFDictionaryRef)@{
        (__bridge id)kSecValueData: data,
        (__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleAfterFirstUnlock,
    });
    if (st != errSecItemNotFound) return st == errSecSuccess;
    q[(__bridge id)kSecValueData] = data;
    q[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlock;
    return SecItemAdd((__bridge CFDictionaryRef)q, NULL) == errSecSuccess;
}

void secure_apple_del(const char *key) { SecItemDelete((__bridge CFDictionaryRef)query(key)); }
