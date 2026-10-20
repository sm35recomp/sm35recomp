// Frame generation: whether this real frame gets an in-between frame before it (framegen.h).
#include "framegen.h"

#include <stdlib.h>
#include <string.h>

#ifndef FG_NO_SDL
#include <SDL3/SDL.h>
#endif

uint64_t g_fg_real, g_fg_generated;

// An in-between frame needs the previous real frame on the same image (size, format, crop) one 60 Hz slot ago
// (12-22 ms: not a hitch, not the game's 30 fps mode), and a display that can show it: below 100 Hz the two presents
// per refresh would only block the next drawable and slow the game.
bool framegen_step(FgState *s, const FgFrame *f) {
    g_fg_real++;
    if (!f->enabled || f->held) {
        s->valid = false;
        return false;
    }
    uint64_t gap = f->now_ns - s->last_ns;
    bool gen = s->valid && s->w == f->w && s->h == f->h && s->fmt == f->fmt && !memcmp(s->crop, f->crop, sizeof s->crop) &&
               (f->any_pace || (gap >= 12000000 && gap <= 22000000)) && f->display_hz >= 100;
    s->valid = true;
    s->w = f->w, s->h = f->h, s->fmt = f->fmt;
    memcpy(s->crop, f->crop, sizeof s->crop);
    s->last_ns = f->now_ns;
    if (gen) g_fg_generated++;
    return gen;
}

uint64_t framegen_real_present_at(uint64_t planned_ns, uint64_t gen_done_ns) {
    uint64_t after = gen_done_ns + FG_HALF_SLOT_NS;
    return after > planned_ns ? after : planned_ns;
}

float framegen_display_hz(struct SDL_Window *w) {
    static float forced = -1;
    if (forced < 0) forced = getenv("LARECOMP_FORCE_DISPLAY_HZ") ? (float)atof(getenv("LARECOMP_FORCE_DISPLAY_HZ")) : 0;
    if (forced > 0) return forced;
#ifdef FG_NO_SDL
    (void)w;
    return 0;
#else
    const SDL_DisplayMode *m = w ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)) : NULL;
    return m ? m->refresh_rate : 0;
#endif
}
