/*
 * sync_sched.h - The deterministic one-CPU scheduler of tests/host/test_sync.c.
 */

#ifndef TESTS_HOST_SYNC_SCHED_H
#define TESTS_HOST_SYNC_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#define SYNC_MAX_THREADS 8

/* Runs `n` fake threads with priorities `prio` (lower is more urgent), each
 * calling body(id), interleaved by `seed`. False on a deadlock. */
bool sync_run(uint64_t seed, int n, const int prio[], void (*body)(int id));

/* In a body: the running thread's id, a switch to another ready thread, and
 * a number in [0, n) from the seed. */
int sync_self(void);
void sync_yield(void);
uint64_t sync_rand(unsigned n);
int sync_priority(int id);

/* One line of the trace (FNV-1a over every line). */
void sync_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint64_t sync_trace_digest(void);
uint64_t sync_trace_lines(void);
void sync_trace_dump(bool on);

#endif
