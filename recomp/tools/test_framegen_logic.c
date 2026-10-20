// Frame generation's per-frame decision (runtime/nvn/framegen.c): when an in-between frame is made. `make test-framegen`.
#include <stdio.h>
#include <stdlib.h>

#include "nvn/framegen.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const uint64_t SLOT = 16666667;  // one 60 Hz frame

static FgFrame base(uint64_t now) {
    FgFrame f = {.w = 1920, .h = 1080, .fmt = 80, .crop = {0, 0, 1, 1}, .now_ns = now, .display_hz = 120, .enabled = true};
    return f;
}

static void test_first_frame_real(void) {
    FgState s = {0};
    FgFrame f = base(SLOT);
    CHECK(!framegen_step(&s, &f));
    f.now_ns += SLOT;
    CHECK(framegen_step(&s, &f));
}

static void test_enable_resets(void) {
    FgState s = {0};
    FgFrame f = base(SLOT);
    CHECK(!framegen_step(&s, &f));
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
    f.now_ns += SLOT; f.enabled = false; CHECK(!framegen_step(&s, &f));
    f.now_ns += SLOT; f.enabled = true; CHECK(!framegen_step(&s, &f));
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
}

static void test_size_change_resets(void) {
    FgState s = {0};
    FgFrame f = base(SLOT);
    framegen_step(&s, &f);
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
    f.now_ns += SLOT; f.w = 1280; CHECK(!framegen_step(&s, &f));
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
    f.now_ns += SLOT; f.fmt = 81; CHECK(!framegen_step(&s, &f));
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
    f.now_ns += SLOT; f.crop[2] = 0.75f; CHECK(!framegen_step(&s, &f));
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
}

static void test_gap_bounds(void) {
    FgState s = {0};
    FgFrame f = base(SLOT);
    framegen_step(&s, &f);
    f.now_ns += 11900000; CHECK(!framegen_step(&s, &f));
    f.now_ns += 22100000; CHECK(!framegen_step(&s, &f));
    f.now_ns += 12000000; CHECK(framegen_step(&s, &f));
    f.now_ns += 22000000; CHECK(framegen_step(&s, &f));
    f.now_ns += 40000000; CHECK(!framegen_step(&s, &f));  // a hitch
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
}

static void test_held_resets(void) {
    FgState s = {0};
    FgFrame f = base(SLOT);
    framegen_step(&s, &f);
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
    f.now_ns += SLOT; f.held = true; CHECK(!framegen_step(&s, &f));
    f.now_ns += SLOT; f.held = false; CHECK(!framegen_step(&s, &f));
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
}

static void test_low_refresh_skips(void) {
    FgState s = {0};
    FgFrame f = base(SLOT);
    f.display_hz = 60;
    for (int i = 0; i < 4; i++) { CHECK(!framegen_step(&s, &f)); f.now_ns += SLOT; }
    f.display_hz = 0;
    for (int i = 0; i < 4; i++) { CHECK(!framegen_step(&s, &f)); f.now_ns += SLOT; }
    f.display_hz = 119.88f;
    framegen_step(&s, &f);  // (it was stored all along: only the refresh kept it from generating)
    f.now_ns += SLOT; CHECK(framegen_step(&s, &f));
}

static void test_any_pace(void) {  // (LARECOMP_FRAMEGEN_ANY_PACE: slow software renderers in tests)
    FgState s = {0};
    FgFrame f = base(SLOT);
    f.any_pace = true;
    framegen_step(&s, &f);
    f.now_ns += 140000000; CHECK(framegen_step(&s, &f));
    f.now_ns += 140000000; f.w = 1280; CHECK(!framegen_step(&s, &f));  // (still not across a size change)
}

static void test_real_present_after_gen_ready(void) {  // (review I2) the real frame waits half a slot after the
    // in-between image is ready on the GPU, not after its present call returned on the CPU
    uint64_t t_gen_cpu = 1000000000ull;
    CHECK(framegen_real_present_at(t_gen_cpu + FG_HALF_SLOT_NS, t_gen_cpu + 1000000) == t_gen_cpu + 1000000 + FG_HALF_SLOT_NS);
    CHECK(framegen_real_present_at(t_gen_cpu + FG_HALF_SLOT_NS, t_gen_cpu - 5000000) == t_gen_cpu + FG_HALF_SLOT_NS);
}

static void test_counters(void) {
    g_fg_real = g_fg_generated = 0;
    FgState s = {0};
    FgFrame f = base(SLOT);
    framegen_step(&s, &f);
    f.now_ns += SLOT; framegen_step(&s, &f);
    f.now_ns += SLOT; framegen_step(&s, &f);
    CHECK(g_fg_real == 3);
    CHECK(g_fg_generated == 2);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } t[] = {
        {"test_first_frame_real", test_first_frame_real}, {"test_enable_resets", test_enable_resets},
        {"test_size_change_resets", test_size_change_resets}, {"test_gap_bounds", test_gap_bounds},
        {"test_held_resets", test_held_resets}, {"test_low_refresh_skips", test_low_refresh_skips},
        {"test_any_pace", test_any_pace}, {"test_real_present_after_gen_ready", test_real_present_after_gen_ready},
        {"test_counters", test_counters},
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int before = fails;
        t[i].fn();
        printf("%s %s\n", fails == before ? "PASS" : "FAIL", t[i].name);
        bad += fails != before;
    }
    return bad ? 1 : 0;
}
