// Frame generation's passes (framegen.h), one source for both backends: written in a small dialect (float2..4, int2,
// FG_TEX / FG_LOAD / FG_SAMPLE / FG_SIZE, mix / clamp / abs / min / max / dot, no select or swizzle assignment) that a
// head turns into MSL (framegen_msl: framegen_metal.m) or GLSL 450 (framegen_glsl: vulkan/shaders.c), with each
// pass's entry point after it - as SMAA is built (metal.m smaa_msl, vulkan/shaders.c).
//
// Per real frame: the colour image -> a luma pyramid (1/4, 1/8, 1/16) -> block motion from the previous frame's
// pyramid, coarse to fine (4 x 4-texel blocks at each level; candidates: zero, the coarser level's vector, and at the
// coarsest the last frame's vector there - the camera scroll carried over) -> a 3 x 3 median -> the mean match cost
// (two reductions) -> the warp: each pixel from prev at -t * motion and cur at +(1 - t) * motion, blended where they
// agree; where the motion pair doesn't match but the pixel didn't change, the still pixel; where neither matches
// (something revealed or covered), one real image, not a mix of two; and the previous frame alone when the whole
// image matches poorly (a scene cut, a fade, a wipe).
//
// Each pass: a full-screen triangle into an image of its output's size, params a / b (two float4), inputs t0..t3.
#include "framegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char framegen_src[] =
    // tuned against tools/test_framegen.m and real scrolling / running captures (LARECOMP_FRAMEGEN_SHOT)
    "#define FG_ZERO_BIAS 0.003\n"  // the still vector's head start (mean abs luma per texel): a HUD and backgrounds stay put
    "#define FG_MATCH 0.08\n"      // max rgb difference for two samples to count as the same thing
    "#define FG_CUT_COST 0.03\n"   // mean best match cost above which it is a cut: show prev (synthetic: matched 0.002, cut 0.05)
    "float fg_lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float fg_maxabs(float3 d) { d = abs(d); return max(d.x, max(d.y, d.z)); }\n"
    // 1/4 luma: the mean over each 4 x 4 (four bilinear taps, each the mean of a 2 x 2)
    "float4 fg_luma(FG_TEX src, int2 p, float2 size) {\n"
    "  float2 o = float2(p) * 4.0;\n"
    "  float s = fg_lum(FG_SAMPLE(src, (o + float2(1.0, 1.0)) / size).rgb) + fg_lum(FG_SAMPLE(src, (o + float2(3.0, 1.0)) / size).rgb)\n"
    "          + fg_lum(FG_SAMPLE(src, (o + float2(1.0, 3.0)) / size).rgb) + fg_lum(FG_SAMPLE(src, (o + float2(3.0, 3.0)) / size).rgb);\n"
    "  return float4(s * 0.25, 0.0, 0.0, 1.0);\n"
    "}\n"
    "float4 fg_down(FG_TEX src, int2 p, float2 size) {\n"
    "  return float4(FG_SAMPLE(src, (float2(p) * 2.0 + float2(1.0, 1.0)) / size).r, 0.0, 0.0, 1.0);\n"
    "}\n"
    // mean abs luma difference of the 4 x 4 block at o (level texels) between cur and prev moved by m (full-res px)
    "float fg_cost(FG_TEX lc, FG_TEX lp, int2 o, float2 m, float L, float2 lsize) {\n"
    "  float2 ml = m / L;\n"
    "  float s = 0.0;\n"
    "  for (int j = 0; j < 4; j++)\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      int2 q = o + int2(i, j);\n"
    "      s += abs(FG_LOAD(lc, q).r - FG_SAMPLE(lp, (float2(q) + float2(0.5, 0.5) - ml) / lsize).r);\n"
    "    }\n"
    "  return s * (1.0 / 16.0);\n"
    "}\n"
    // the block's motion at level L (full-res px per level texel): rg = prev -> cur in full-res px, b = its cost.
    // Around each seed (zero; the coarser vector or the last frame's) a +-radius search in level texels; the zero
    // vector costs FG_ZERO_BIAS less, and ties keep the earlier (zero first).
    "float4 fg_motion(FG_TEX lc, FG_TEX lp, FG_TEX coarse, FG_TEX last, int2 b, float4 a, float4 pb) {\n"
    "  float L = a.x; int r = int(a.y); float2 lsize = pb.xy;\n"
    "  int2 o = b * 4;\n"
    "  float2 c = float2(o) + float2(2.0, 2.0);\n"  // the block's centre, level texels
    "  float2 seed = float2(0.0, 0.0);\n"
    "  if (a.z > 0.5) seed = FG_LOAD(coarse, int2(c / 8.0)).rg;\n"            // coarser level: blocks of 8 of these texels
    "  else if (a.w > 0.5) seed = FG_LOAD(last, int2(c * L / 16.0)).rg;\n"    // last frame's final motion: 16-px blocks
    "  float2 best = float2(0.0, 0.0);\n"
    "  float bestc = fg_cost(lc, lp, o, best, L, lsize);\n"
    "  float bestk = bestc - FG_ZERO_BIAS;\n"
    "  for (int s = 0; s < 2; s++) {\n"
    "    float2 base = s == 0 ? float2(0.0, 0.0) : floor(seed / L + float2(0.5, 0.5)) * L;\n"
    "    if (s == 1 && base.x == 0.0 && base.y == 0.0) break;\n"
    "    for (int dy = -r; dy <= r; dy++)\n"
    "      for (int dx = -r; dx <= r; dx++) {\n"
    "        float2 m = base + float2(float(dx), float(dy)) * L;\n"
    "        if (m.x == 0.0 && m.y == 0.0) continue;\n"
    "        float k = fg_cost(lc, lp, o, m, L, lsize);\n"
    "        if (k < bestk) { bestk = k; bestc = k; best = m; }\n"
    "      }\n"
    "  }\n"
    // the finest level: +-1 texel around the best in quarter-texel (1 px) steps, so small objects that move a few px
    // a frame get their own motion rather than the nearest 4-px step
    "  if (L < 5.0) {\n"
    "    float2 base = best;\n"
    "    for (int dy = -4; dy <= 4; dy++)\n"
    "      for (int dx = -4; dx <= 4; dx++) {\n"
    "        float2 m = base + float2(float(dx), float(dy)) * (L * 0.25);\n"
    "        if ((dx == 0 && dy == 0) || (m.x == 0.0 && m.y == 0.0)) continue;\n"
    "        float k = fg_cost(lc, lp, o, m, L, lsize);\n"
    "        if (k < bestk) { bestk = k; bestc = k; best = m; }\n"
    "      }\n"
    "  }\n"
    "  return float4(best.x, best.y, bestc, 1.0);\n"
    "}\n"
    // per-component median of the 3 x 3 vectors around block p, as a candidate: it replaces the block's own vector
    // only where it matches at least as well (lc / lp: the 1/4 luma, lsize its size) - a plain median would hand a
    // sprite a block or two wide its still neighbours' vector
    "float fg_med9(float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7, float v8) {\n"
    "  float v[9];\n"
    "  v[0] = v0; v[1] = v1; v[2] = v2; v[3] = v3; v[4] = v4; v[5] = v5; v[6] = v6; v[7] = v7; v[8] = v8;\n"
    "  for (int i = 0; i < 5; i++)\n"
    "    for (int j = i + 1; j < 9; j++) { float lo = min(v[i], v[j]), hi = max(v[i], v[j]); v[i] = lo; v[j] = hi; }\n"
    "  return v[4];\n"
    "}\n"
    "float4 fg_median(FG_TEX m, FG_TEX lc, FG_TEX lp, int2 p, float2 lsize) {\n"
    "  float4 n4 = FG_LOAD(m, p);\n"
    "  float4 n0 = FG_LOAD(m, p + int2(-1, -1)), n1 = FG_LOAD(m, p + int2(0, -1)), n2 = FG_LOAD(m, p + int2(1, -1));\n"
    "  float4 n3 = FG_LOAD(m, p + int2(-1, 0)),  n5 = FG_LOAD(m, p + int2(1, 0));\n"
    "  float4 n6 = FG_LOAD(m, p + int2(-1, 1)),  n7 = FG_LOAD(m, p + int2(0, 1)),  n8 = FG_LOAD(m, p + int2(1, 1));\n"
    "  float2 md = float2(fg_med9(n0.x, n1.x, n2.x, n3.x, n4.x, n5.x, n6.x, n7.x, n8.x),\n"
    "                     fg_med9(n0.y, n1.y, n2.y, n3.y, n4.y, n5.y, n6.y, n7.y, n8.y));\n"
    "  if (md.x == n4.x && md.y == n4.y) return n4;\n"
    "  float k = fg_cost(lc, lp, p * 4, md, 4.0, lsize);\n"
    "  return k <= n4.z ? float4(md, k, 1.0) : n4;\n"
    "}\n"
    "float4 fg_cost_rows(FG_TEX m, int y, int w) {\n"
    "  float s = 0.0;\n"
    "  for (int x = 0; x < w; x++) s += FG_LOAD(m, int2(x, y)).b;\n"
    "  return float4(s / float(w), 0.0, 0.0, 1.0);\n"
    "}\n"
    "float4 fg_cost_total(FG_TEX rows, int h) {\n"
    "  float s = 0.0;\n"
    "  for (int y = 0; y < h; y++) s += FG_LOAD(rows, int2(0, y)).r;\n"
    "  return float4(s / float(h), 0.0, 0.0, 1.0);\n"
    "}\n"
    // pix: the output pixel's centre (full-res px); motion has one texel per 16 x 16 px. Each pixel tries its block's
    // vector and its four neighbours' and keeps the one whose two samples agree best: a block holding the edge of a
    // moving sprite over a still background has the background's vector, its neighbour the sprite's.
    "float4 fg_warp(FG_TEX prev, FG_TEX cur, FG_TEX mot, FG_TEX cost, float2 pix, float t, float2 size) {\n"
    "  float3 z0 = FG_SAMPLE(prev, pix / size).rgb;\n"
    "  if (FG_LOAD(cost, int2(0, 0)).r > FG_CUT_COST) return float4(z0, 1.0);\n"
    "  float3 z1 = FG_SAMPLE(cur, pix / size).rgb;\n"
    "  int2 q = int2(pix / 16.0);\n"
    "  float3 a = z0, b = z1;\n"
    "  float dm = 1e9;\n"
    "  for (int i = 0; i < 5; i++) {\n"
    "    int2 d = i == 0 ? int2(0, 0) : i == 1 ? int2(-1, 0) : i == 2 ? int2(1, 0) : i == 3 ? int2(0, -1) : int2(0, 1);\n"
    "    float2 m = FG_LOAD(mot, q + d).rg;\n"
    "    float3 ai = FG_SAMPLE(prev, (pix - t * m) / size).rgb;\n"
    "    float3 bi = FG_SAMPLE(cur, (pix + (1.0 - t) * m) / size).rgb;\n"
    "    float di = fg_maxabs(ai - bi);\n"
    "    if (di < dm) { dm = di; a = ai; b = bi; }\n"
    "  }\n"
    "  float dz = fg_maxabs(z0 - z1);\n"
    "  if (dz <= dm && dz < FG_MATCH) return float4(mix(z0, z1, t), 1.0);\n"
    "  if (dm < FG_MATCH) return float4(mix(a, b, t), 1.0);\n"
    "  return float4(t < 0.5 ? a : b, 1.0);\n"
    "}\n";

// ---- Metal
static const char msl_head[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constexpr sampler fg_lin(filter::linear, address::clamp_to_edge);\n"
    "#define FG_TEX texture2d<float>\n"
    "#define FG_SIZE(t) int2(int((t).get_width()), int((t).get_height()))\n"
    "#define FG_LOAD(t, p) (t).read(uint2(clamp((p), int2(0, 0), FG_SIZE(t) - int2(1, 1))))\n"
    "#define FG_SAMPLE(t, uv) (t).sample(fg_lin, (uv), level(0.0))\n";
static const char msl_tail[] =
    "struct V { float4 pos [[position]]; };\n"
    "struct P { float4 a; float4 b; };\n"
    "vertex V fg_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); return o; }\n"
    "#define FG_IN V in [[stage_in]], constant P &pc [[buffer(0)]], texture2d<float> t0 [[texture(0)]], \\\n"
    "  texture2d<float> t1 [[texture(1)]], texture2d<float> t2 [[texture(2)]], texture2d<float> t3 [[texture(3)]]\n"
    "fragment float4 fg_luma_fs(FG_IN) { return fg_luma(t0, int2(in.pos.xy), pc.a.xy); }\n"
    "fragment float4 fg_down_fs(FG_IN) { return fg_down(t0, int2(in.pos.xy), pc.a.xy); }\n"
    "fragment float4 fg_motion_fs(FG_IN) { return fg_motion(t0, t1, t2, t3, int2(in.pos.xy), pc.a, pc.b); }\n"
    "fragment float4 fg_median_fs(FG_IN) { return fg_median(t0, t1, t2, int2(in.pos.xy), pc.a.xy); }\n"
    "fragment float4 fg_cost_rows_fs(FG_IN) { return fg_cost_rows(t0, int(in.pos.y), int(pc.a.x)); }\n"
    "fragment float4 fg_cost_total_fs(FG_IN) { return fg_cost_total(t0, int(pc.a.x)); }\n"
    "fragment float4 fg_warp_fs(FG_IN) { return fg_warp(t0, t1, t2, t3, in.pos.xy, pc.a.x, pc.a.yz); }\n";

char *framegen_msl(void) {
    size_t n = sizeof msl_head + sizeof framegen_src + sizeof msl_tail;
    char *s = malloc(n);
    snprintf(s, n, "%s%s%s", msl_head, framegen_src, msl_tail);
    return s;
}

// ---- Vulkan (GLSL 450, push constants a / b, inputs at bindings 0..3 of set 0)
static const char glsl_head[] =
    "#version 450\n"
    "#define float2 vec2\n"
    "#define float3 vec3\n"
    "#define float4 vec4\n"
    "#define int2 ivec2\n"
    "#define FG_TEX sampler2D\n"
    "#define FG_SIZE(t) textureSize(t, 0)\n"
    "#define FG_LOAD(t, p) texelFetch(t, clamp((p), ivec2(0, 0), textureSize(t, 0) - ivec2(1, 1)), 0)\n"
    "#define FG_SAMPLE(t, uv) textureLod(t, (uv), 0.0)\n"
    "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
    "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
    "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
    "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
    "layout(set = 0, binding = 3) uniform sampler2D t3;\n"
    "layout(location = 0) out vec4 o;\n";
static const struct { const char *name, *main; } glsl_mains[] = {
    {"fg_luma.frag", "void main() { o = fg_luma(t0, ivec2(gl_FragCoord.xy), pc.a.xy); }\n"},
    {"fg_down.frag", "void main() { o = fg_down(t0, ivec2(gl_FragCoord.xy), pc.a.xy); }\n"},
    {"fg_motion.frag", "void main() { o = fg_motion(t0, t1, t2, t3, ivec2(gl_FragCoord.xy), pc.a, pc.b); }\n"},
    {"fg_median.frag", "void main() { o = fg_median(t0, t1, t2, ivec2(gl_FragCoord.xy), pc.a.xy); }\n"},
    {"fg_cost_rows.frag", "void main() { o = fg_cost_rows(t0, int(gl_FragCoord.y), int(pc.a.x)); }\n"},
    {"fg_cost_total.frag", "void main() { o = fg_cost_total(t0, int(pc.a.x)); }\n"},
    {"fg_warp.frag", "void main() { o = fg_warp(t0, t1, t2, t3, gl_FragCoord.xy, pc.a.x, pc.a.yz); }\n"},
};

const char *framegen_glsl(const char *pass) {
    enum { N = sizeof glsl_mains / sizeof glsl_mains[0] };
    static char *src[N];
    for (int i = 0; i < N; i++) {
        if (strcmp(pass, glsl_mains[i].name)) continue;
        if (!src[i]) {
            size_t n = sizeof glsl_head + sizeof framegen_src + strlen(glsl_mains[i].main) + 1;
            src[i] = malloc(n);
            snprintf(src[i], n, "%s%s%s", glsl_head, framegen_src, glsl_mains[i].main);
        }
        return src[i];
    }
    return NULL;
}
