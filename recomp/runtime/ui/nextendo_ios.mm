// The Nextendo sign-in sheet on iOS (runtime/ui/online_rows.mm): ASWebAuthenticationSession shows nextendo.network in
// the system's sign-in sheet. Nextendo accepts only a loopback redirect for this app (http://127.0.0.1:<port>/callback),
// so the sign-in runs as on the desktop: nextendo.c's listener in the app takes the browser's redirect and finishes
// the sign-in, and the sheet (which never sees a redirect of its own scheme) is closed here once the status says the
// sign-in has ended. Built for ios / iossim only (recomp/Makefile).
#import <AuthenticationServices/AuthenticationServices.h>
#import <UIKit/UIKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
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
static NSTimer *g_watch;
static unsigned g_gen;  // the sheet that is up (a late completion of an older one must not touch the newer)

static void watch_stop(void) { [g_watch invalidate]; g_watch = nil; }

extern "C" void nextendo_ios_sign_in(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_session) return;  // (a sheet is up: the gamepad's A can reach the row behind it)
        char url[1024];
        if (!nextendo_begin_sign_in(NULL, url, sizeof url)) return;  // (the loopback listener; the status says why not)
        NSURL *start = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
        if (!g_anchor) g_anchor = [[NxAnchor alloc] init];
        unsigned gen = ++g_gen;
        ASWebAuthenticationSession *ses = [[ASWebAuthenticationSession alloc]
                initWithURL:start
                callbackURLScheme:nil
                completionHandler:^(NSURL *cb, NSError *err) {
                    (void)cb;
                    if (gen != g_gen) return;
                    g_session = nil;
                    watch_stop();
                    // closed by the player (or by the watch below once the sign-in ended): only a sign-in still
                    // waiting for the browser is called off
                    NxStatus st;
                    nextendo_status(&st);
                    if (st.state != NX_WAITING_BROWSER) return;
                    bool cancelled = [err.domain isEqualToString:ASWebAuthenticationSessionErrorDomain] &&
                                     err.code == ASWebAuthenticationSessionErrorCodeCanceledLogin;
                    nextendo_sign_in_aborted(cancelled || !err ? "Sign-in cancelled" : err.localizedDescription.UTF8String);
                }];
        g_session = ses;
        ses.presentationContextProvider = g_anchor;
        ses.prefersEphemeralWebBrowserSession = NO;
        if (![ses start]) {
            g_session = nil;
            nextendo_sign_in_aborted("Could not open the sign-in sheet.");
            return;
        }
        // the listener ends the sign-in (signed in, or an error): close the sheet then
        watch_stop();
        g_watch = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
            (void)t;
            NxStatus st;
            nextendo_status(&st);
            if (st.state == NX_WAITING_BROWSER || gen != g_gen) return;
            watch_stop();
            [g_session cancel];
        }];
    });
}
