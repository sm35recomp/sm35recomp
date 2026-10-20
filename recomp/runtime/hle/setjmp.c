// setjmp / longjmp across recompiled code.
//
// The guest's setjmp runs recompiled: it saves x19-x30, sp and d8-d15 into the guest jmp_buf and returns 0. Its longjmp
// would load them back and `br x30` to the instruction after the setjmp call, which is the middle of a recompiled
// function: there is no host function there to jump to. So every call to setjmp in the generated code also takes a host
// setjmp (tools/gen.py, setjmp call sites), into a host jmp_buf kept here for the guest jmp_buf's address, and the
// guest's longjmp is host code: it restores the guest registers from the guest jmp_buf, sets the return value, and
// host-longjmps back into the recompiled function that called setjmp, which carries on after the call. That function
// is still on the host stack (C requires it), and the host frames between are only recompiled code and the dispatcher.
//
// Lua (the game's scripts, Bezel Engine) raises its errors this way: luaD_throw longjmps to luaD_rawrunprotected.
#include <setjmp.h>
#include <string.h>

#include "../hle.h"

// Per thread: the guest jmp_bufs it has called setjmp with, newest last. Lua nests them (one per protected call, up to
// its C-stack limit of about 200); the table is a ring, so the oldest entry gives way when it is full.
#define JB_SLOTS 512
typedef struct { uint64_t key; jmp_buf jb; } JbSlot;
static __thread JbSlot *jb_slots;
static __thread unsigned jb_next;

static JbSlot *jb_find(uint64_t key) {
    if (!jb_slots) return NULL;
    for (unsigned i = 0; i < JB_SLOTS; i++) {
        JbSlot *s = &jb_slots[(jb_next - 1 - i) % JB_SLOTS];  // (newest first)
        if (s->key == key) return s;
    }
    return NULL;
}

jmp_buf *rt_jb_slot(Cpu *c, uint64_t guest_buf) {
    (void)c;
    if (!jb_slots) jb_slots = calloc(JB_SLOTS, sizeof *jb_slots);
    JbSlot *s = jb_find(guest_buf);
    if (!s) {
        s = &jb_slots[jb_next++ % JB_SLOTS];
        s->key = guest_buf;
    }
    return &s->jb;
}

static void guest_longjmp(Cpu *c) {
    uint64_t buf = c->x[0], val = c->x[1];
    JbSlot *s = jb_find(buf);
    if (!s) FATAL("longjmp to %llx: no setjmp took that jmp_buf on this thread\n", (unsigned long long)buf);
    const uint64_t *g = (const uint64_t *)(uintptr_t)buf;  // (the guest setjmp's layout: x19..x30, -, sp, d8..d15)
    for (int r = 19; r <= 30; r++) c->x[r] = g[r - 19];
    c->sp = g[13];
    for (int d = 8; d <= 15; d++) {
        uint64_t lo = g[14 + d - 8];
        memset(&c->v[d], 0, sizeof c->v[d]);
        memcpy(&c->v[d], &lo, 8);
    }
    c->x[0] = val ? val : 1;
    longjmp(s->jb, 1);
}
HLE_FUNC("longjmp", hle_guest_longjmp) { guest_longjmp(c); }
HLE_FUNC("_longjmp", hle_guest__longjmp) { guest_longjmp(c); }
