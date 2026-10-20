// Unit tests for runtime/nvn/buildsync.c (shader prefetch: build states, waits with steal, pending table).
// usage: make test-buildsync
#include <pthread.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/nvn/buildsync.h"

static int failures;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); failures++; return; } } while (0)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

static void test_mode(void) {
    CHECK("mode", shader_mode_from(NULL, NULL) == SHADERS_PREFETCH);
    CHECK("mode", shader_mode_from("1", NULL) == SHADERS_SYNC);
    CHECK("mode", shader_mode_from(NULL, "1") == SHADERS_SKIP);
    CHECK("mode", shader_mode_from("1", "1") == SHADERS_SKIP);
    printf("ok mode\n");
}

typedef struct { _Atomic int *state; int result, delay; } Finisher;
static void *finisher(void *a) {
    Finisher *f = a;
    sleep_ms(f->delay);
    build_finish(f->state, f->result);
    return NULL;
}
static pthread_t finish_later(Finisher *f) { pthread_t t; pthread_create(&t, NULL, finisher, f); return t; }

static void test_wait_after_finish(void) {
    memset(&g_prefetch, 0, sizeof g_prefetch);
    _Atomic int s = BUILD_PENDING;
    build_begin();
    Finisher f = {&s, BUILD_READY, 50};
    pthread_t t = finish_later(&f);
    int r = build_wait(&s, "test");
    pthread_join(t, NULL);
    CHECK("wait_after_finish", r == BUILD_READY);
    CHECK("wait_after_finish", g_prefetch.waits == 1);
    CHECK("wait_after_finish", g_prefetch.wait_ms >= 40);
    CHECK("wait_after_finish", g_prefetch.worst_ms >= 40);
    printf("ok wait_after_finish\n");
}

static void test_wait_failed_returns(void) {
    _Atomic int s = BUILD_PENDING;
    build_begin();
    Finisher f = {&s, BUILD_FAILED, 10};
    pthread_t t = finish_later(&f);
    int r = build_wait(&s, "test");
    pthread_join(t, NULL);
    CHECK("wait_failed_returns", r == BUILD_FAILED);
    printf("ok wait_failed_returns\n");
}

static void test_wait_ready_no_count(void) {
    memset(&g_prefetch, 0, sizeof g_prefetch);
    _Atomic int s = BUILD_READY;
    CHECK("wait_ready_no_count", build_wait(&s, "test") == BUILD_READY);
    CHECK("wait_ready_no_count", g_prefetch.waits == 0);
    printf("ok wait_ready_no_count\n");
}

static sem_t *gate;
static void gate_job(void *a) { (void)a; sem_wait(gate); }
static void record_thread(void *a) { *(pthread_t *)a = pthread_self(); }

static void test_steal_queued(void) {
    JobQueue *q = jobq_create(1);
    BuildJob g = {0}, b = {0};
    pthread_t ran = 0;
    jobq_submit(q, &g, gate_job, NULL);
    jobq_submit(q, &b, record_thread, &ran);
    job_run_or_wait(q, &b);
    CHECK("steal_queued", pthread_equal(ran, pthread_self()));
    CHECK("steal_queued", b.state == JOB_DONE);
    sem_post(gate);
    job_run_or_wait(q, &g);
    CHECK("steal_queued", g.state == JOB_DONE);
    printf("ok steal_queued\n");
}

typedef struct { pthread_t ran; _Atomic int started; } Slow;
static void slow_job(void *a) { Slow *s = a; s->started = 1; sleep_ms(50); s->ran = pthread_self(); }

static void test_wait_running(void) {
    memset(&g_prefetch, 0, sizeof g_prefetch);
    JobQueue *q = jobq_create(1);
    BuildJob j = {0};
    Slow s = {0};
    jobq_submit(q, &j, slow_job, &s);
    while (!s.started) sleep_ms(1);
    double t0 = now_ms();
    job_run_or_wait(q, &j);
    CHECK("wait_running", j.state == JOB_DONE);
    CHECK("wait_running", now_ms() - t0 >= 30);
    CHECK("wait_running", !pthread_equal(s.ran, pthread_self()));
    CHECK("wait_running", g_prefetch.waits == 1);  // (a wait on a job counts as one, as build_wait's do)
    printf("ok wait_running\n");
}

static int order[10], norder;
static void order_job(void *a) { order[norder++] = (int)(intptr_t)a; }

static void test_fifo(void) {
    JobQueue *q = jobq_create(1);
    BuildJob g = {0}, j[10];
    memset(j, 0, sizeof j);
    jobq_submit(q, &g, gate_job, NULL);  // (hold the worker so all ten are queued before any runs)
    for (int i = 0; i < 10; i++) jobq_submit(q, &j[i], order_job, (void *)(intptr_t)i);
    sem_post(gate);
    while (j[9].state != JOB_DONE) sleep_ms(1);
    CHECK("fifo", norder == 10);
    for (int i = 0; i < 10; i++) CHECK("fifo", order[i] == i);
    printf("ok fifo\n");
}

static void test_drain(void) {
    _Atomic int s[3] = {BUILD_PENDING, BUILD_PENDING, BUILD_PENDING};
    Finisher f[3];
    pthread_t t[3];
    for (int i = 0; i < 3; i++) {
        build_begin();
        f[i] = (Finisher){&s[i], BUILD_READY, 20 * (i + 1)};
        t[i] = finish_later(&f[i]);
    }
    double t0 = now_ms();
    build_drain();
    double dt = now_ms() - t0;
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
    CHECK("drain", dt >= 50);
    CHECK("drain", s[2] == BUILD_READY);
    printf("ok drain\n");
}

// (a job whose function publishes its object's state and is then still running: build_drain must wait for the job
// to be done with the object too - a reset frees it right after the drain)
static _Atomic int tail_state;
static void finish_then_linger(void *a) { (void)a; build_finish(&tail_state, BUILD_READY); sleep_ms(50); }
static void test_drain_waits_job_tail(void) {
    JobQueue *q = jobq_create(1);
    BuildJob j = {0};
    tail_state = BUILD_PENDING;
    build_begin();
    jobq_submit(q, &j, finish_then_linger, NULL);
    build_drain();
    CHECK("drain_waits_job_tail", j.state == JOB_DONE);
    printf("ok drain_waits_job_tail\n");
}

#define PEND_THREADS 8
#define PEND_PER 400
static int pend_vals[PEND_THREADS][PEND_PER];
static void *pend_worker(void *a) {
    int id = (int)(intptr_t)a;
    for (int i = 0; i < PEND_PER; i++) {
        uint64_t h = 1000 + (uint64_t)(i * PEND_THREADS + id);
        void *got = pend_get_or_put(h, &pend_vals[id][i]);
        if (got != &pend_vals[id][i]) { printf("FAIL pend: hash %llu got another entry\n", (unsigned long long)h); failures++; }
    }
    return NULL;
}
static void test_pend(void) {
    int a, b;
    CHECK("pend", pend_get_or_put(7, &a) == &a);
    CHECK("pend", pend_get_or_put(7, &b) == &a);
    CHECK("pend", pend_take(7) == &a);
    CHECK("pend", pend_take(7) == NULL);
    pthread_t t[PEND_THREADS];
    for (int i = 0; i < PEND_THREADS; i++) pthread_create(&t[i], NULL, pend_worker, (void *)(intptr_t)i);
    for (int i = 0; i < PEND_THREADS; i++) pthread_join(t[i], NULL);
    for (int id = 0; id < PEND_THREADS; id++)
        for (int i = 0; i < PEND_PER; i++)
            CHECK("pend", pend_take(1000 + (uint64_t)(i * PEND_THREADS + id)) == &pend_vals[id][i]);
    CHECK("pend", pend_take(1000) == NULL);
    printf("ok pend\n");
}

int main(void) {
    gate = sem_open("/test_buildsync_gate", O_CREAT, 0600, 0);
    sem_unlink("/test_buildsync_gate");
    test_mode();
    test_wait_after_finish();
    test_wait_failed_returns();
    test_wait_ready_no_count();
    test_steal_queued();
    test_wait_running();
    test_fifo();
    test_drain();
    test_drain_waits_job_tail();
    test_pend();
    printf(failures ? "test_buildsync: %d failed\n" : "test_buildsync: all passed\n", failures);
    return failures ? 1 : 0;
}
