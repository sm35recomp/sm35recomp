// The Nextendo sign-in sheet on iOS (runtime/ui/online_rows.mm): ASWebAuthenticationSession shows nextendo.network in
// the system's sign-in sheet and catches the app's own redirect scheme (GAME_URL_SCHEME) itself (no Info.plist entry needed). Built for
// ios / iossim only (recomp/Makefile).
#import <AuthenticationServices/AuthenticationServices.h>
#import <UIKit/UIKit.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
#include "../game.h"
#include "../net/nextendo.h"
}

@interface NxAnchor : NSObject <ASWebAuthenticationPresentationContextProviding>
@end
@implementation NxAnchor
- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:(ASWebAuthenticationSession *)session {
    for (UIScene *sc in UIApplication.sharedApplication.connectedScenes) {
        if (![sc isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *ws = (UIWindowScene *)sc;
        for (UIWindow *w in ws.windows) if (w.isKeyWindow) return w;
        if (ws.windows.firstObject) return ws.windows.firstObject;
    }
    return [[UIWindow alloc] init];
}
@end

static ASWebAuthenticationSession *g_session;  // (held while the sheet is up)
static NxAnchor *g_anchor;

static void *finish_thread(void *arg) {  // the code exchange and game-token fetch block for up to ~20 s
    char *url = (char *)arg;
    nextendo_finish_sign_in(url);
    free(url);
    return NULL;
}

static unsigned g_gen;  // the sheet that is up (a late completion of an older one must not touch the newer)

extern "C" void nextendo_ios_sign_in(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_session) return;  // (a sheet is up: the gamepad's A can reach the row behind it)
        char url[1024];
        if (!nextendo_begin_sign_in(GAME_URL_SCHEME "://callback", url, sizeof url)) return;  // (the status says why)
        NSURL *start = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
        if (!g_anchor) g_anchor = [[NxAnchor alloc] init];
        unsigned gen = ++g_gen;
        ASWebAuthenticationSession *ses = [[ASWebAuthenticationSession alloc]
                initWithURL:start
                callbackURLScheme:@GAME_URL_SCHEME
                completionHandler:^(NSURL *cb, NSError *err) {
                    if (gen == g_gen) g_session = nil;
                    if (cb) {
                        pthread_t th;
                        char *copy = strdup(cb.absoluteString.UTF8String);
                        if (pthread_create(&th, NULL, finish_thread, copy) == 0) pthread_detach(th);
                        else free(copy);
                    } else if ([err.domain isEqualToString:ASWebAuthenticationSessionErrorDomain] &&
                               err.code == ASWebAuthenticationSessionErrorCodeCanceledLogin) {
                        nextendo_sign_in_aborted("Sign-in cancelled");
                    } else {
                        nextendo_sign_in_aborted(err.localizedDescription.UTF8String);
                    }
                }];
        g_session = ses;
        ses.presentationContextProvider = g_anchor;
        ses.prefersEphemeralWebBrowserSession = NO;
        if (![ses start]) {
            g_session = nil;
            nextendo_sign_in_aborted("Could not open the sign-in sheet.");
        }
    });
}
