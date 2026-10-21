// The Mods tab's Online section (runtime/ui/menu.mm tab_mods): which backend the game's online features talk to, and
// the Nextendo Network account (runtime/net/nextendo.h) - sign in in the browser (desktop / Android) or in the system's
// sign-in sheet (iOS, runtime/ui/nextendo_ios.mm). Plain C++ here: the same on every platform.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "option_rows.h"
extern "C" {
#include "../net/netcfg.h"
#include "../net/nextendo.h"
}

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
extern "C" void nextendo_ios_sign_in(void);  // runtime/ui/nextendo_ios.mm
#define NX_SHEET 1
#endif

extern "C" bool online_rows_available(void) { return true; }

static void sign_in(void) {
#ifdef NX_SHEET
    nextendo_ios_sign_in();
#else
    char url[1024];
    if (nextendo_begin_sign_in(NULL, url, sizeof url)) SDL_OpenURL(url);  // (failure: the status says why)
#endif
}

// RECOMP_NEXTENDO_SIGN_IN=1: start the Nextendo sign-in as the game starts (the browser opens), for a machine where
// the overlay's key is out of reach (a remote desktop without F1). Signed in already: nothing.
extern "C" void online_sign_in_at_start(void) {
    const char *e = getenv("RECOMP_NEXTENDO_SIGN_IN");
    if (e && *e == '1' && !nextendo_signed_in()) sign_in();
}

extern "C" void online_rows(void) {
    opt_section("Online");

    // the choices: what can be chosen now, and the one that is set when it can't (labelled "(sign in)", so the row doesn't lie about what is set)
    const NetBackendOption *o;
    int n = net_backend_options(&o);
    NetBackend cur = net_backend_setting();
    static char names_buf[NET_BACKEND_COUNT][96];
    const char *names[NET_BACKEND_COUNT];
    int ids[NET_BACKEND_COUNT], m = 0, sel = 0;
    const char *desc = "";
    for (int i = 0; i < n && m < NET_BACKEND_COUNT; i++) {
        if (!o[i].available && o[i].id != cur) continue;
        if (o[i].available) names[m] = o[i].label;
        else snprintf(names_buf[m], sizeof names_buf[m], "%s (sign in)", o[i].label), names[m] = names_buf[m];
        ids[m] = o[i].id;
        if (o[i].id == cur) sel = m, desc = o[i].desc;
        m++;
    }
    static char d[700];
    snprintf(d, sizeof d, "%s\n\nTakes effect at the next start.", desc);
    if (m > 0 && opt_choice("Service", &sel, names, m, 0, d)) net_backend_select((NetBackend)ids[sel]);

    NxStatus st;
    nextendo_status(&st);
    // a sign-in that has just ended well chooses Nextendo (the player signed in to play there)
    static NxState was = NX_SIGNED_OUT;
    if (st.state == NX_SIGNED_IN && was == NX_WAITING_BROWSER && net_backend_setting() != NET_BACKEND_NEXTENDO)
        net_backend_select(NET_BACKEND_NEXTENDO);
    was = st.state;
    const char *val = "Sign in";
    if (st.state == NX_WAITING_BROWSER) val = "Waiting for the browser...";
    else if (st.state == NX_SIGNED_IN) val = st.username[0] ? st.username : "Signed in";
    else if (st.state == NX_ERROR) val = "Sign-in failed";  // (short: the full message is in the description)
    static char ad[400];
    snprintf(ad, sizeof ad, "%s%sSign in on nextendo.network in your browser. The game never sees your password; it "
             "keeps a sign-in token for this device.", st.state == NX_ERROR ? st.message : "",
             st.state == NX_ERROR && st.message[0] ? "\n\n" : "");
    if (opt_action("Nextendo account", val, ad)) {
#ifdef NX_SHEET
        if (st.state != NX_SIGNED_IN && st.state != NX_WAITING_BROWSER) sign_in();  // (the sheet may be up)
#else
        if (st.state != NX_SIGNED_IN) sign_in();
#endif
    }
    if (st.state == NX_SIGNED_IN &&
        opt_action("Sign out", "", "Forget this computer's Nextendo sign-in."))
        nextendo_sign_out();
}
