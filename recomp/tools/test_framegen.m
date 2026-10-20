// Frame generation's passes (runtime/nvn/framegen_src.c through framegen_metal.m) on synthetic images: a scroll lands
// halfway, a still image and a fixed HUD stay exact, a hard cut shows the previous frame. `make test-framegen`.
#import <Metal/Metal.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn/framegen.h"

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

enum { W = 512, H = 256 };
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;

static uint32_t hash3(int x, int y, int seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
// Value noise with cells of `cell` px, bilinear inside each cell: 0..1
static double noise(double x, double y, int cell, int seed) {
    double fx = x / cell, fy = y / cell;
    int cx = (int)floor(fx), cy = (int)floor(fy);
    double tx = fx - cx, ty = fy - cy;
    double v00 = (hash3(cx, cy, seed) & 0xffff) / 65535.0, v10 = (hash3(cx + 1, cy, seed) & 0xffff) / 65535.0;
    double v01 = (hash3(cx, cy + 1, seed) & 0xffff) / 65535.0, v11 = (hash3(cx + 1, cy + 1, seed) & 0xffff) / 65535.0;
    return (v00 * (1 - tx) + v10 * tx) * (1 - ty) + (v01 * (1 - tx) + v11 * tx) * ty;
}
// The test image: three octaves (64, 16 and 8 px cells), each channel its own seed. BGRA bytes.
static void pattern(uint8_t *px, int shift, int seed) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 3; c++) {
                int s = seed * 7 + c;
                double v = 0.5 * noise(x - shift, y, 64, s) + 0.3 * noise(x - shift, y, 16, s + 100) +
                           0.2 * noise(x - shift, y, 8, s + 200);
                v = (v - 0.5) * 2.0 + 0.5;  // (more contrast)
                px[(y * W + x) * 4 + c] = (uint8_t)lround(fmin(fmax(v, 0), 1) * 255);
            }
    for (int i = 0; i < W * H; i++) px[i * 4 + 3] = 255;
}
static void hud(uint8_t *px) {  // a 96 x 32 box at (40, 40) with a 4-px checker inside
    for (int y = 40; y < 72; y++)
        for (int x = 40; x < 136; x++) {
            uint8_t v = ((x / 4 + y / 4) & 1) ? 230 : 20;
            uint8_t *p = &px[(y * W + x) * 4];
            p[0] = v, p[1] = v, p[2] = (uint8_t)(255 - v);
        }
}

static id<MTLTexture> tex(const uint8_t *px) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    if (px) [t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4];
    return t;
}

// The in-between image of prev and cur (a fresh history each time), read back into out. false: fgm_encode failed.
static bool generate(const uint8_t *prev, const uint8_t *cur, uint8_t *out) {
    id<MTLTexture> a = tex(prev), b = tex(cur), o = tex(NULL);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (!fgm_encode(dev, cb, a, b, 0.5f, true, o)) return false;
    [cb commit];
    [cb waitUntilCompleted];
    [o getBytes:out bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    return true;
}

static uint8_t A[W * H * 4], B[W * H * 4], O[W * H * 4], E[W * H * 4];

static int maxdiff(const uint8_t *x, const uint8_t *y, int x0, int y0, int x1, int y1) {
    int m = 0;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            for (int c = 0; c < 3; c++) {
                int d = abs(x[(yy * W + xx) * 4 + c] - y[(yy * W + xx) * 4 + c]);
                if (d > m) m = d;
            }
    return m;
}

// FG_TEST_DUMP=<dir>: write a test's output and expected images there (PPM) for a look
static void dump(const char *name, const uint8_t *px) {
    const char *dir = getenv("FG_TEST_DUMP");
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { fputc(px[i * 4 + 2], f); fputc(px[i * 4 + 1], f); fputc(px[i * 4], f); }
    fclose(f);
}

static int fails;
static void report(const char *name, bool ok, const char *fmt, ...) {
    char v[128] = "";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    printf("%s %s (%s)\n", ok ? "PASS" : "FAIL", name, v);
    fails += !ok;
}

static void test_scroll(void) {
    pattern(A, 0, 1);
    pattern(B, 16, 1);
    pattern(E, 8, 1);
    if (!generate(A, B, O)) { report("test_scroll", false, "fgm_encode failed"); return; }
    double se = 0;
    int n = 0;
    for (int y = 32; y < H - 32; y++)
        for (int x = 32; x < W - 32; x++)
            for (int c = 0; c < 3; c++) {
                double d = O[(y * W + x) * 4 + c] - E[(y * W + x) * 4 + c];
                se += d * d, n++;
            }
    double psnr = se > 0 ? 10 * log10(255.0 * 255.0 / (se / n)) : 99;
    report("test_scroll", psnr >= 30, "PSNR %.1f dB, need 30", psnr);
}

static void test_static(void) {
    pattern(A, 0, 1);
    if (!generate(A, A, O)) { report("test_static", false, "fgm_encode failed"); return; }
    int m = maxdiff(O, A, 0, 0, W, H);
    report("test_static", m == 0, "max diff %d", m);
}

static void test_cut(void) {
    pattern(A, 0, 1);
    pattern(B, 0, 2);
    if (!generate(A, B, O)) { report("test_cut", false, "fgm_encode failed"); return; }
    int m = maxdiff(O, A, 0, 0, W, H);
    report("test_cut", m == 0, "max diff from prev %d", m);
}

static void test_hud(void) {
    pattern(A, 0, 1);
    pattern(B, 16, 1);
    hud(A);
    hud(B);
    if (!generate(A, B, O)) { report("test_hud", false, "fgm_encode failed"); return; }
    int m = maxdiff(O, A, 40, 40, 136, 72);
    report("test_hud", m <= 2, "max diff in the box %d", m);
}

// a 24 x 24 sprite (its own pattern, seed 9) at (x, 120) over the still pattern
static void sprite(uint8_t *px, int x) {
    static uint8_t spr[W * H * 4];
    pattern(spr, 0, 9);
    for (int y = 0; y < 24; y++)
        for (int i = 0; i < 24; i++) memcpy(&px[((120 + y) * W + x + i) * 4], &spr[(y * W + i) * 4], 4);
}

static void test_sprite(void) {  // a small object moving 6 px over a still background lands 3 px along
    pattern(A, 0, 1);
    pattern(B, 0, 1);
    pattern(E, 0, 1);
    sprite(A, 200);
    sprite(B, 206);
    sprite(E, 203);
    if (!generate(A, B, O)) { report("test_sprite", false, "fgm_encode failed"); return; }
    dump("sprite_out", O);
    dump("sprite_expected", E);
    double se = 0;
    int n = 0;
    for (int y = 112; y < 152; y++)
        for (int x = 192; x < 238; x++)
            for (int c = 0; c < 3; c++) {
                double d = O[(y * W + x) * 4 + c] - E[(y * W + x) * 4 + c];
                se += d * d, n++;
            }
    double psnr = se > 0 ? 10 * log10(255.0 * 255.0 / (se / n)) : 99;
    report("test_sprite", psnr >= 30, "PSNR %.1f dB around the sprite, need 30", psnr);
}

int main(void) {
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        queue = [dev newCommandQueue];
        test_scroll();
        test_static();
        test_cut();
        test_hud();
        test_sprite();
    }
    return fails ? 1 : 0;
}
