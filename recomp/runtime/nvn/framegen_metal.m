// Frame generation on Metal: framegen_src.c's passes, encoded per real frame (framegen.h fgm_encode; metal.m presents).
#import <Metal/Metal.h>

#include "framegen.h"
#include "rt.h"

typedef enum { P_LUMA, P_DOWN, P_MOTION, P_MEDIAN, P_ROWS, P_TOTAL, N_PASSES } Pass;
static const char *const pass_fn[N_PASSES] = {"fg_luma_fs", "fg_down_fs", "fg_motion_fs", "fg_median_fs", "fg_cost_rows_fs",
                                              "fg_cost_total_fs"};

static id<MTLLibrary> lib;
static id<MTLRenderPipelineState> pipes[N_PASSES];
static struct { MTLPixelFormat f; id<MTLRenderPipelineState> p; } warp_cache[4];
static bool failed;

static id<MTLRenderPipelineState> pipeline(id<MTLDevice> dev, const char *fn, MTLPixelFormat f) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"fg_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@(fn)];
    pd.colorAttachments[0].pixelFormat = f;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) LOG("[framegen] pipeline %s: %s\n", fn, err.localizedDescription.UTF8String);
    return p;
}

static bool setup(id<MTLDevice> dev) {
    if (lib) return true;
    char *src = framegen_msl();
    NSError *err = nil;
    lib = [dev newLibraryWithSource:@(src) options:nil error:&err];
    free(src);
    if (!lib) { LOG("[framegen] shaders: %s\n", err.localizedDescription.UTF8String); return false; }
    for (int i = 0; i < N_PASSES; i++) {
        pipes[i] = pipeline(dev, pass_fn[i], i == P_LUMA || i == P_DOWN ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA16Float);
        if (!pipes[i]) return false;
    }
    return true;
}

static id<MTLRenderPipelineState> warp_pipe(id<MTLDevice> dev, MTLPixelFormat f) {
    for (int i = 0; i < 4; i++) if (warp_cache[i].p && warp_cache[i].f == f) return warp_cache[i].p;
    id<MTLRenderPipelineState> p = pipeline(dev, "fg_warp_fs", f);
    for (int i = 0; p && i < 4; i++) if (!warp_cache[i].p) { warp_cache[i].f = f; warp_cache[i].p = p; break; }
    return p;
}

// (kept until the size changes)
static id<MTLTexture> target(id<MTLDevice> dev, id<MTLTexture> old, NSUInteger w, NSUInteger h, MTLPixelFormat f) {
    if (old && old.width == w && old.height == h && old.pixelFormat == f) return old;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:td];
}

static void pass(id<MTLCommandBuffer> cb, id<MTLRenderPipelineState> p, id<MTLTexture> dst, id<MTLTexture> const in[4],
                 const float a[4], const float b[4]) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p];
    float pc[8] = {a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]};
    [e setFragmentBytes:pc length:sizeof pc atIndex:0];
    for (int i = 0; i < 4; i++) [e setFragmentTexture:in[i] atIndex:i];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static NSUInteger up(NSUInteger v, NSUInteger d) { return (v + d - 1) / d; }

bool fgm_encode(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> prev, id<MTLTexture> cur, float t, bool reset,
                id<MTLTexture> out) {
    static id<MTLTexture> pyr[2][3], mot[3], med[2], rows, total, dummy;  // pyramid levels 1/4, 1/8, 1/16
    static int k;              // pyr[k] / med[k]: this frame's; the other: the last frame's
    static bool have_prev_pyr, have_last;
    static NSUInteger lw, lh;
    if (failed) return false;
    if (!setup(dev)) { failed = true; return false; }
    id<MTLRenderPipelineState> wp = warp_pipe(dev, out.pixelFormat);
    if (!wp) { failed = true; return false; }
    NSUInteger W = cur.width, H = cur.height;
    if (W != lw || H != lh) reset = true, lw = W, lh = H;
    if (!dummy) dummy = target(dev, nil, 1, 1, MTLPixelFormatRGBA16Float);
    NSUInteger lv[3][2];
    lv[0][0] = up(W, 4), lv[0][1] = up(H, 4);
    for (int i = 1; i < 3; i++) lv[i][0] = up(lv[i - 1][0], 2), lv[i][1] = up(lv[i - 1][1], 2);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 3; i++) pyr[j][i] = target(dev, pyr[j][i], lv[i][0], lv[i][1], MTLPixelFormatRGBA8Unorm);
    for (int i = 0; i < 3; i++) mot[i] = target(dev, mot[i], up(lv[i][0], 4), lv[i][1] ? up(lv[i][1], 4) : 1, MTLPixelFormatRGBA16Float);
    for (int j = 0; j < 2; j++) med[j] = target(dev, med[j], mot[0].width, mot[0].height, MTLPixelFormatRGBA16Float);
    rows = target(dev, rows, 1, mot[0].height, MTLPixelFormatRGBA16Float);
    total = target(dev, total, 1, 1, MTLPixelFormatRGBA16Float);
    if (reset) have_prev_pyr = have_last = false;

    const float z[4] = {0};
    // the luma pyramids: this frame's, and the previous one's when there is none kept
    for (int which = have_prev_pyr ? 1 : 0; which < 2; which++) {
        id<MTLTexture> src = which ? cur : prev;
        __strong id<MTLTexture> *p = which ? pyr[k] : pyr[k ^ 1];
        id<MTLTexture> in[4] = {src, dummy, dummy, dummy};
        float a[4] = {(float)W, (float)H};
        pass(cb, pipes[P_LUMA], p[0], in, a, z);
        for (int i = 1; i < 3; i++) {
            id<MTLTexture> di[4] = {p[i - 1], dummy, dummy, dummy};
            float da[4] = {(float)p[i - 1].width, (float)p[i - 1].height};
            pass(cb, pipes[P_DOWN], p[i], di, da, z);
        }
    }
    // motion, coarse (1/16, +-4 texels) to fine (1/8, 1/4: +-2 around the coarser vector)
    for (int i = 2; i >= 0; i--) {
        float L = (float)(4 << i);
        id<MTLTexture> in[4] = {pyr[k][i], pyr[k ^ 1][i], i < 2 ? mot[i + 1] : dummy, have_last ? med[k ^ 1] : dummy};
        float a[4] = {L, i == 2 ? 4.0f : 2.0f, i < 2 ? 1.0f : 0.0f, i == 2 && have_last ? 1.0f : 0.0f};
        float b[4] = {(float)lv[i][0], (float)lv[i][1], (float)W, (float)H};
        pass(cb, pipes[P_MOTION], mot[i], in, a, b);
    }
    {
        id<MTLTexture> in[4] = {mot[0], pyr[k][0], pyr[k ^ 1][0], dummy};
        float ma[4] = {(float)lv[0][0], (float)lv[0][1]};
        pass(cb, pipes[P_MEDIAN], med[k], in, ma, z);
        id<MTLTexture> ri[4] = {med[k], dummy, dummy, dummy};
        float ra[4] = {(float)med[k].width};
        pass(cb, pipes[P_ROWS], rows, ri, ra, z);
        id<MTLTexture> ti[4] = {rows, dummy, dummy, dummy};
        float ta[4] = {(float)rows.height};
        pass(cb, pipes[P_TOTAL], total, ti, ta, z);
        static int log_cost = -1;  // LARECOMP_FRAMEGEN_COST=1: log each frame's mean match cost (tuning FG_CUT_COST)
        if (log_cost < 0) log_cost = getenv("LARECOMP_FRAMEGEN_COST") != NULL;
        if (log_cost) {
            id<MTLBuffer> bb = [dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:total sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1)
                       toBuffer:bb destinationOffset:0 destinationBytesPerRow:8 destinationBytesPerImage:8];
            [bl endEncoding];
            [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; LOG("[framegen] cost %.4f\n", (float)((__fp16 *)bb.contents)[0]); }];
        }
        id<MTLTexture> wi[4] = {prev, cur, med[k], total};
        float wa[4] = {t, (float)W, (float)H, 0};
        pass(cb, wp, out, wi, wa, z);
    }
    have_prev_pyr = have_last = true;
    k ^= 1;
    return true;
}
