// Tests for runtime/kernel.c's mutexes and condition variables (Horizon semantics), through rt_svc: the guest
// threads are host functions started by svcCreateThread / svcStartThread, and lock / unlock / wait below do what the
// game's SDK does around svcArbitrateLock, svcArbitrateUnlock and svcWaitProcessWideKeyAtomic. Run by
// tools/test_kernel_condvar.sh. A case that doesn't finish within 1 s fails (the run stops: its threads can't be
// killed).
// Usage: test_kernel_condvar [case...]   (no case: all of them)
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../runtime/kernel.h"

#define W 0x40000000u  // the mutex word's waiters bit
#define R_TIMEOUT 0xEA01u

static _Atomic int failures;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) { printf("  %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; }     \
    } while (0)

// ---- what kernel.c needs from the rest of the runtime
static _Atomic int n_self_lock_logs;  // "[kernel] guest locks a mutex it already owns" lines
void rt_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, "guest locks a mutex it already owns")) n_self_lock_logs++;
    if (getenv("TEST_KERNEL_LOG")) fputs(line, stderr);
}
void rt_fatal_trace(void) { abort(); }
void crash_handler_install(void) {}
void guest_call(Cpu *c, uint64_t addr) { ((void (*)(Cpu *))(uintptr_t)addr)(c); }  // (entries are host functions)
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void plat_print_backtrace(void) {}
const char *plat_symbol(uint64_t pc, uint64_t *offset) { (void)pc; (void)offset; return NULL; }
int ipc_handle_request(Cpu *c, uint32_t handle) { (void)c; (void)handle; return 0; }
uint32_t ipc_connect_named_port(const char *name) { (void)name; return 0; }
uint32_t vi_vsync_handle(void) { return 0; }
Module g_modules[4];
int g_nmodules;

// ---- the guest side
static uint64_t svc(Cpu *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    rt_svc(c, imm, 0);
    return c->x[0];
}

static uint32_t self(Cpu *c) { return ((KThread *)c->thread)->handle; }
static uint32_t word(_Atomic uint32_t *m) { return *m; }
static uint32_t owner(_Atomic uint32_t *m) { return *m & ~W; }

// nn::os / libnx mutex lock: take a free mutex, else mark it contended and ArbitrateLock on its owner. No "owner is
// me" check, as in the SDK: a thread that locks a mutex it already holds asks the kernel to wait on itself.
static void lock(Cpu *c, _Atomic uint32_t *m) {
    uint32_t tag = self(c);
    for (;;) {
        uint32_t v = 0;
        if (__atomic_compare_exchange_n((uint32_t *)m, &v, tag, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
        if (!(v & W) && !__atomic_compare_exchange_n((uint32_t *)m, &v, v | W, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            continue;
        svc(c, 0x1A, v & ~W, (uint64_t)(uintptr_t)m, tag, 0);
        if (owner(m) == tag) return;
    }
}

static void unlock(Cpu *c, _Atomic uint32_t *m) {
    uint32_t v = self(c);
    if (!__atomic_compare_exchange_n((uint32_t *)m, &v, 0, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        svc(c, 0x1B, (uint64_t)(uintptr_t)m, 0, 0, 0);
}

static uint32_t cv_wait(Cpu *c, _Atomic uint32_t *cv, _Atomic uint32_t *m, int64_t timeout_ns) {
    return (uint32_t)svc(c, 0x1C, (uint64_t)(uintptr_t)m, (uint64_t)(uintptr_t)cv, self(c), (uint64_t)timeout_ns);
}

static void signal_cv(Cpu *c, _Atomic uint32_t *cv, int count) { svc(c, 0x1D, (uint64_t)(uintptr_t)cv, (uint64_t)count, 0, 0); }
static void sleep_ns(Cpu *c, int64_t ns) { svc(c, 0x0B, (uint64_t)ns, 0, 0, 0); }

// ---- threads and the watchdog
static Cpu g_host;  // svc calls from the test's own (non-guest) thread
static _Atomic int g_done;

static uint32_t start(void (*fn)(Cpu *), int prio, int core) {
    g_host.x[4] = (uint64_t)prio;
    g_host.x[5] = (uint64_t)(int64_t)core;
    CHECK(svc(&g_host, 0x08, 0, (uint64_t)(uintptr_t)fn, 0, 0) == 0, "CreateThread");
    uint32_t h = (uint32_t)g_host.x[1];
    CHECK(svc(&g_host, 0x09, h, 0, 0, 0) == 0, "StartThread");
    return h;
}

// Wait until `n` threads have finished (g_done), at most 1 s; a deadlocked case can't be recovered from.
static void await(const char *name, int n) {
    for (uint64_t end = plat_now_ns() + 1000000000ull; g_done < n;) {
        if (plat_now_ns() > end) {
            printf("  %s: still running after 1 s (deadlocked)\nFAIL\n", name);
            fflush(stdout);
            _exit(1);
        }
        usleep(1000);
    }
}

static _Atomic uint32_t g_m, g_cv;  // the case's mutex word and condition variable key
static _Atomic int g_ready, g_released;
static _Atomic uint32_t g_b_handle;
static void reset(void) { g_m = 0; g_cv = 0; g_ready = 0; g_released = 0; g_done = 0; g_b_handle = 0; }

// A timed-out wait returns 0xEA01 without the mutex; the SDK then locks it again, which must not block.
static void timeout_thread(Cpu *c) {
    lock(c, &g_m);
    CHECK(owner(&g_m) == self(c), "locked: word %x, self %x", word(&g_m), self(c));
    uint32_t r = cv_wait(c, &g_cv, &g_m, 1000000);
    CHECK(r == R_TIMEOUT, "timed-out wait returned %x, expected ea01", r);
    CHECK(owner(&g_m) != self(c), "timed-out wait returned holding the mutex (word %x)", word(&g_m));
    lock(c, &g_m);  // (nn::os TimedWait after 0xEA01)
    CHECK(owner(&g_m) == self(c), "relocked: word %x", word(&g_m));
    unlock(c, &g_m);
    CHECK(word(&g_m) == 0, "unlocked: word %x", word(&g_m));
    g_done++;
}

static void timeout_returns_without_mutex(void) {
    start(timeout_thread, 44, 0);
    await("timeout_returns_without_mutex", 1);
}

// A signalled wait returns 0 holding the mutex.
static void signalled_waiter(Cpu *c) {
    lock(c, &g_m);
    g_ready = 1;
    uint32_t r = cv_wait(c, &g_cv, &g_m, 500000000);
    CHECK(r == 0, "signalled wait returned %x", r);
    CHECK(owner(&g_m) == self(c), "signalled wait returned without the mutex (word %x)", word(&g_m));
    unlock(c, &g_m);
    g_done++;
}

static void signaller(Cpu *c) {
    while (!g_ready) sleep_ns(c, 100000);
    lock(c, &g_m);  // (free once the waiter is in its wait)
    signal_cv(c, &g_cv, 1);
    unlock(c, &g_m);
    g_done++;
}

static void signal_returns_with_mutex(void) {
    start(signalled_waiter, 44, 0);
    start(signaller, 44, 1);
    await("signal_returns_with_mutex", 2);
    CHECK(word(&g_m) == 0, "word %x after both unlocked", word(&g_m));
}

// Signalled while another thread holds the mutex (moved to the mutex's waiters), then the deadline passes: 0xEA01,
// and the mutex is still the other thread's; the waiter's own lock then waits for its unlock.
static void late_waiter(Cpu *c) {
    lock(c, &g_m);
    g_ready = 1;
    uint32_t r = cv_wait(c, &g_cv, &g_m, 50000000);
    CHECK(r == R_TIMEOUT, "wait returned %x, expected ea01", r);
    CHECK(owner(&g_m) == g_b_handle, "after the timeout: word %x, holder %x", word(&g_m), g_b_handle);
    CHECK(!g_released, "the holder had already unlocked");
    lock(c, &g_m);
    CHECK(g_released, "locked before the holder unlocked");
    CHECK(owner(&g_m) == self(c), "relocked: word %x", word(&g_m));
    unlock(c, &g_m);
    g_done++;
}

static void holder(Cpu *c) {
    g_b_handle = self(c);
    while (!g_ready) sleep_ns(c, 100000);
    lock(c, &g_m);
    signal_cv(c, &g_cv, 1);
    sleep_ns(c, 200000000);  // (the waiter's 50 ms pass meanwhile)
    g_released = 1;
    unlock(c, &g_m);  // (a stale waiters bit: through ArbitrateUnlock)
    g_done++;
}

static void signal_then_timeout(void) {
    start(late_waiter, 44, 0);
    start(holder, 44, 1);
    await("signal_then_timeout", 2);
    CHECK(word(&g_m) == 0, "word %x after both unlocked", word(&g_m));
}

// Signals racing short deadlines (the wake that lands as the deadline passes): a wait that returns 0 holds the
// mutex, one that returns 0xEA01 doesn't, every time.
static _Atomic int n_ok, n_timeout;
static void racing_waiter(Cpu *c) {
    lock(c, &g_m);
    g_ready = 1;
    for (uint64_t end = plat_now_ns() + 300000000ull; plat_now_ns() < end;) {
        uint32_t r = cv_wait(c, &g_cv, &g_m, 20000);
        if (r == 0) {
            n_ok++;
            if (owner(&g_m) != self(c)) { CHECK(0, "0 without the mutex (word %x)", word(&g_m)); break; }
        } else {
            n_timeout++;
            if (r != R_TIMEOUT) { CHECK(0, "wait returned %x", r); break; }
            if (owner(&g_m) == self(c)) { CHECK(0, "ea01 holding the mutex (word %x)", word(&g_m)); break; }
            lock(c, &g_m);
        }
    }
    g_ready = 2;
    unlock(c, &g_m);
    g_done++;
}

static void racing_signaller(Cpu *c) {
    while (!g_ready) sleep_ns(c, 100000);
    while (g_ready == 1) {
        lock(c, &g_m);
        signal_cv(c, &g_cv, 1);
        unlock(c, &g_m);
    }
    g_done++;
}

static void wake_at_deadline(void) {
    n_ok = n_timeout = 0;
    start(racing_waiter, 44, 0);
    start(racing_signaller, 44, 1);
    await("wake_at_deadline", 2);
    printf("  (%d signalled, %d timed out)\n", n_ok, n_timeout);
    CHECK(n_ok > 0 && n_timeout > 0, "both outcomes expected");
}

// ArbitrateLock on a mutex the caller owns: logged once per address, and it still waits (as Horizon does), until
// CancelSynchronization ends it.
static _Atomic int g_self_returns;
static void self_locker(Cpu *c) {
    g_b_handle = self(c);
    lock(c, &g_m);
    for (int i = 0; i < 2; i++) {
        g_m = self(c) | W;
        svc(c, 0x1A, self(c), (uint64_t)(uintptr_t)&g_m, self(c), 0);
        g_self_returns++;
    }
    g_m = 0;
    g_done++;
}

static void self_lock_logged(void) {
    n_self_lock_logs = 0;
    g_self_returns = 0;
    start(self_locker, 44, 0);
    for (int i = 0; i < 2; i++) {
        usleep(50000);
        CHECK(g_self_returns == i, "the self-lock returned without a cancel (%d)", g_self_returns);
        svc(&g_host, 0x19, g_b_handle, 0, 0, 0);
    }
    await("self_lock_logged", 1);
    CHECK(n_self_lock_logs == 1, "%d 'already owns' log lines, expected 1", n_self_lock_logs);
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    {"timeout_returns_without_mutex", timeout_returns_without_mutex},
    {"signal_returns_with_mutex", signal_returns_with_mutex},
    {"signal_then_timeout", signal_then_timeout},
    {"wake_at_deadline", wake_at_deadline},
    {"self_lock_logged", self_lock_logged},
};

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        bool run = argc < 2;
        for (int a = 1; a < argc; a++) run |= !strcmp(argv[a], cases[i].name);
        if (!run) continue;
        int before = failures;
        printf("%s\n", cases[i].name);
        reset();
        cases[i].fn();
        if (failures != before) printf("  FAILED\n");
    }
    printf(failures ? "FAIL (%d)\n" : "ok\n", failures);
    return failures != 0;
}
