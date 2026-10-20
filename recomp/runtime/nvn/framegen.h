// Frame generation (Video tab "Frame generation"): one in-between frame per real 60 Hz frame for 120 Hz displays.
// The game still runs and is paced at 60 (objects.c display_pace); the generated frames exist only on the screen.
// framegen.c decides per real frame whether one is made; framegen_src.c holds the passes (MSL / GLSL);
// framegen_metal.m and vulkan/exec.c encode them; metal.m / vulkan/device.c present the two images.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct SDL_Window;

typedef struct {
    uint32_t w, h, fmt;   // the game image's (= drawable's) size and pixel format
    float crop[4];        // widescreen_present_crop's uv rectangle
    uint64_t now_ns;      // plat_now_ns() at this real frame's present
    float display_hz;     // the window's display refresh (SDL), 0 = unknown
    bool enabled;         // g_settings.frame_gen != FRAMEGEN_OFF
    bool held;            // a re-present while the game is held (backend_represent / iOS background)
    bool any_pace;        // (tests on slow software renderers, LARECOMP_FRAMEGEN_ANY_PACE=1: no 12-22 ms gap rule)
} FgFrame;
typedef struct { bool valid; uint32_t w, h, fmt; float crop[4]; uint64_t last_ns; } FgState;

// Per real frame: true = generate an in-between frame from the stored previous game image and this one.
// Afterwards this frame is the stored one (unless !enabled or held: then history is dropped).
bool framegen_step(FgState *s, const FgFrame *f);
// The window's display refresh in Hz (0 if unknown). LARECOMP_FORCE_DISPLAY_HZ=<hz> overrides it (tests).
float framegen_display_hz(struct SDL_Window *w);
extern uint64_t g_fg_real, g_fg_generated;  // counters for the fps log
#define FG_HALF_SLOT_NS 8333333ull           // the real frame is shown this long after the generated one
// When the real frame's present may go: half a slot after the in-between image is ready on the GPU (gen_done_ns),
// and not before the planned time - so the in-between image gets a refresh of its own (a mailbox swapchain would
// otherwise replace it with the real one)
uint64_t framegen_real_present_at(uint64_t planned_ns, uint64_t gen_done_ns);

// The passes (framegen_src.c): one shared source, assembled per backend.
char *framegen_msl(void);                     // malloc'd MSL library: every pass as fragment function <pass>_fs, plus fg_vs
const char *framegen_glsl(const char *pass);  // GLSL 450 fragment source for "fg_luma.frag" ... "fg_warp.frag" (NULL: unknown)

#ifdef __OBJC__
#import <Metal/Metal.h>
// The in-between image of prev and cur (same size and format) at phase t, drawn into out (any colour format, same
// size). reset: drop the kept luma pyramid and motion (prev's pyramid is then built from prev). false on a shader /
// pipeline error (logged once; frame generation then stays off).
bool fgm_encode(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> prev, id<MTLTexture> cur, float t, bool reset,
                id<MTLTexture> out);
#endif
