// Shader prefetch's shared parts (buildsync.h).
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef LOG
#include "rt.h"
#endif
#include "buildsync.h"

PrefetchStats g_prefetch;

int shader_mode_from(const char *no_prefetch, const char *async) {
    if (async) return SHADERS_SKIP;
    if (no_prefetch) return SHADERS_SYNC;
    return SHADERS_PREFETCH;
}
int shader_mode(void) {
    static int v = -1;
    if (v < 0) v = shader_mode_from(getenv("LARECOMP_NO_SHADER_PREFETCH"), getenv("LARECOMP_ASYNC_SHADERS"));
    return v;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// ---- build states: one lock and condition for every completion (waits are rare: first uses only)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done = PTHREAD_COND_INITIALIZER;
static int inflight;

void build_begin(void) {
    pthread_mutex_lock(&lock);
    inflight++;
    pthread_mutex_unlock(&lock);
}
void build_finish(_Atomic int *state, int result) {
    pthread_mutex_lock(&lock);
    atomic_store(state, result);
    inflight--;
    pthread_cond_broadcast(&done);
    pthread_mutex_unlock(&lock);
}

static void count_wait(double ms, const void *what_addr, const char *what) {  // (under lock)
    g_prefetch.waits++;
    g_prefetch.wait_ms += ms;
    if (ms > g_prefetch.worst_ms) g_prefetch.worst_ms = ms;
    if (ms > 2000) {  // (once per object)
        static const void *logged[64];
        static int nlogged;
        for (int i = 0; i < nlogged; i++) if (logged[i] == what_addr) return;
        if (nlogged < 64) logged[nlogged++] = what_addr;
        LOG("[shader] waited %.0f ms for %s\n", ms, what);
    }
}

int build_wait(_Atomic int *state, const char *what) {
    int s = atomic_load(state);
    if (s != BUILD_PENDING) return s;
    double t0 = now_ms();
    pthread_mutex_lock(&lock);
    while ((s = atomic_load(state)) == BUILD_PENDING) pthread_cond_wait(&done, &lock);
    count_wait(now_ms() - t0, state, what);
    pthread_mutex_unlock(&lock);
    return s;
}
void build_drain(void) {
    pthread_mutex_lock(&lock);
    while (inflight > 0) pthread_cond_wait(&done, &lock);
    pthread_mutex_unlock(&lock);
}

// ---- pending pipelines: open addressing, linear probing, deletion by backward shift
#define PEND_TAB 4096
static pthread_mutex_t pend_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t key; void *val; } pend[PEND_TAB];
static int npend;
static unsigned pend_slot(uint64_t h) { return (unsigned)(h ^ (h >> 29)) & (PEND_TAB - 1); }

void *pend_get_or_put(uint64_t hash, void *fresh) {
    pthread_mutex_lock(&pend_lock);
    unsigned s = pend_slot(hash);
    while (pend[s].key && pend[s].key != hash) s = (s + 1) & (PEND_TAB - 1);
    void *r;
    if (pend[s].key) r = pend[s].val;
    else if (npend >= PEND_TAB - 1) r = fresh;  // (full: not kept; the draw builds it on demand)
    else { pend[s].key = hash; pend[s].val = fresh; npend++; r = fresh; }
    pthread_mutex_unlock(&pend_lock);
    return r;
}
void *pend_take(uint64_t hash) {
    pthread_mutex_lock(&pend_lock);
    unsigned s = pend_slot(hash);
    while (pend[s].key && pend[s].key != hash) s = (s + 1) & (PEND_TAB - 1);
    void *r = NULL;
    if (pend[s].key) {
        r = pend[s].val;
        npend--;
        // (backward shift: entries after the hole that probed past it move into it)
        unsigned hole = s, i = s;
        for (;;) {
            i = (i + 1) & (PEND_TAB - 1);
            if (!pend[i].key) break;
            unsigned home = pend_slot(pend[i].key);
            if (((i - home) & (PEND_TAB - 1)) >= ((i - hole) & (PEND_TAB - 1))) { pend[hole] = pend[i]; hole = i; }
        }
        pend[hole].key = 0;
        pend[hole].val = NULL;
    }
    pthread_mutex_unlock(&pend_lock);
    return r;
}
void pend_clear(void (*release)(void *)) {
    pthread_mutex_lock(&pend_lock);
    for (int i = 0; i < PEND_TAB; i++)
        if (pend[i].key) { if (release) release(pend[i].val); pend[i].key = 0; pend[i].val = NULL; }
    npend = 0;
    pthread_mutex_unlock(&pend_lock);
}

// ---- job queues
struct JobQueue { pthread_mutex_t lock; pthread_cond_t cond; BuildJob *head, *tail; };

static void run_job(BuildJob *j) {  // (the caller won QUEUED -> RUNNING)
    build_begin();  // (in flight until it's done with j too: a reset frees the object j is in once build_drain returns)
    j->fn(j->arg);
    pthread_mutex_lock(&lock);
    atomic_store(&j->state, JOB_DONE);
    inflight--;
    pthread_cond_broadcast(&done);
    pthread_mutex_unlock(&lock);
}
static void unlink_job(JobQueue *q, BuildJob *j) {  // (under q->lock)
    BuildJob *prev = NULL;
    for (BuildJob *p = q->head; p; prev = p, p = p->next) {
        if (p != j) continue;
        if (prev) prev->next = p->next; else q->head = p->next;
        if (q->tail == p) q->tail = prev;
        p->next = NULL;
        return;
    }
}
static void *worker(void *a) {
    JobQueue *q = a;
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->head) pthread_cond_wait(&q->cond, &q->lock);
        BuildJob *j = q->head;
        unlink_job(q, j);
        pthread_mutex_unlock(&q->lock);
        int want = JOB_QUEUED;
        if (atomic_compare_exchange_strong(&j->state, &want, JOB_RUNNING)) run_job(j);  // (else a waiter took it)
    }
    return NULL;
}
JobQueue *jobq_create(int nthreads) {
    JobQueue *q = calloc(1, sizeof *q);
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    for (int i = 0; i < nthreads; i++) {
        pthread_t t;
        pthread_create(&t, NULL, worker, q);
        pthread_detach(t);
    }
    return q;
}
void jobq_submit(JobQueue *q, BuildJob *j, void (*fn)(void *), void *arg) {
    j->fn = fn;
    j->arg = arg;
    j->next = NULL;
    atomic_store(&j->state, JOB_QUEUED);
    pthread_mutex_lock(&q->lock);
    if (q->tail) q->tail->next = j; else q->head = j;
    q->tail = j;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}
void job_run_or_wait(JobQueue *q, BuildJob *j) {
    if (atomic_load(&j->state) == JOB_DONE) return;
    double t0 = now_ms();
    int want = JOB_QUEUED;
    if (atomic_compare_exchange_strong(&j->state, &want, JOB_RUNNING)) {
        pthread_mutex_lock(&q->lock);
        unlink_job(q, j);
        pthread_mutex_unlock(&q->lock);
        run_job(j);
        pthread_mutex_lock(&lock);
    } else {
        pthread_mutex_lock(&lock);
        while (atomic_load(&j->state) != JOB_DONE) pthread_cond_wait(&done, &lock);
    }
    count_wait(now_ms() - t0, j, "job");
    pthread_mutex_unlock(&lock);
}

void build_test_delay(void) {
    static int ms = -1;
    if (ms < 0) { const char *e = getenv("LARECOMP_DEBUG_CONT_DELAY_MS"); ms = e ? atoi(e) : 0; }
    if (ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&t, NULL); }
}

// ---- stats
void prefetch_stats_log(void) {
    PrefetchStats *s = &g_prefetch;
    LOG("[shader] prefetch: scanned %llu draws / %llu submits (%.1f ms), started %llu programs + %llu pipelines ahead, "
        "waits %llu (%.0f ms total, worst %.0f ms), unforeseen %llu\n",
        (unsigned long long)s->draws, (unsigned long long)s->submits, s->scan_ns / 1e6, (unsigned long long)s->progs,
        (unsigned long long)s->pipes, (unsigned long long)s->waits, s->wait_ms, s->worst_ms,
        (unsigned long long)s->unforeseen);
}
void prefetch_stats_tick(uint64_t presented) {
    static bool reg;
    if (!reg) { reg = true; atexit(prefetch_stats_log); }
    if (!presented || presented % 600) return;
    prefetch_stats_log();
    pthread_mutex_lock(&lock);
    memset(&g_prefetch, 0, sizeof g_prefetch);
    pthread_mutex_unlock(&lock);
}
