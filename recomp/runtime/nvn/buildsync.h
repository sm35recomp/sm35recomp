#pragma once
// Shader prefetch (docs/superpowers/specs/2026-10-20-shader-prefetch-design.md), shared by both backends: the mode,
// build states and the waits on them, the in-flight count a reset drains, the pending-pipeline table, a job queue
// whose waiter runs a job itself while it is still queued, and the prefetch stats.
#include <stdatomic.h>
#include <stdint.h>

// prefetch (default): builds start ahead and in the background, a draw waits for its own; sync
// (LARECOMP_NO_SHADER_PREFETCH=1): built inline on first use; skip (LARECOMP_ASYNC_SHADERS=1, LAS's way): built in
// the background, draws skipped meanwhile
enum { SHADERS_PREFETCH, SHADERS_SYNC, SHADERS_SKIP };
int shader_mode_from(const char *no_prefetch, const char *async);  // (the two variables' values, NULL when unset)
int shader_mode(void);                                              // (read once)

enum { BUILD_PENDING = 0, BUILD_READY = 1, BUILD_FAILED = 2 };
void build_begin(void);                             // one more build in flight
void build_finish(_Atomic int *state, int result);  // the build's result: one fewer in flight, waiters woken
int build_wait(_Atomic int *state, const char *what);  // until it isn't PENDING (counted in g_prefetch when it waits)
void build_drain(void);                             // until no build is in flight (before a reset drops them)

// Pipelines requested ahead, by pipeline hash (non-zero), until a draw takes them into its own table
void *pend_get_or_put(uint64_t hash, void *fresh);  // the entry there, or fresh (now there)
void *pend_take(uint64_t hash);                     // removed, or NULL
void pend_clear(void (*release)(void *));

typedef struct JobQueue JobQueue;
typedef struct BuildJob { void (*fn)(void *); void *arg; struct BuildJob *next; _Atomic int state; } BuildJob;
enum { JOB_IDLE, JOB_QUEUED, JOB_RUNNING, JOB_DONE };
JobQueue *jobq_create(int nthreads);
void jobq_submit(JobQueue *q, BuildJob *j, void (*fn)(void *), void *arg);  // (j: the caller's, e.g. in the object)
void job_run_or_wait(JobQueue *q, BuildJob *j);  // still queued: run here, now; else wait until it's done

typedef struct { uint64_t draws, submits, scan_ns, progs, pipes, waits, unforeseen; double wait_ms, worst_ms; } PrefetchStats;
extern PrefetchStats g_prefetch;
void prefetch_stats_tick(uint64_t presented);  // the stats line every 600 presented frames (then reset)
void prefetch_stats_log(void);                 // the stats line now (also at exit)
void build_test_delay(void);  // (test) LARECOMP_DEBUG_CONT_DELAY_MS: a pause where a program's pipeline requests go out
