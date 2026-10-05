/*
 * online_sb.c - The onlining store-buffering shape on real hardware
 * (docs/kernel/quiesce/testing.md, "Memory ordering").
 *
 *   waiter:  unlink (release store P = 1); read online (acquire)
 *   new CPU: mark online (release store);  read P (acquire)
 *
 * The protocol's bad outcome is online == 0 (the waiter does not wait
 * for the CPU) with P == 0 (the CPU's reader missed the unlink). FENCE=0
 * is the order before the W1b/Q0 fences; FENCE=1 puts a seq_cst fence
 * between each side's store and load, as quiesce_core_after_begin and
 * quiesce_core_after_online do. A count is evidence that the pattern is
 * real on this machine, never proof of its absence: the verdicts are the
 * litmus tests in tests/litmus/quiesce/ (`make litmus`).
 *
 *   cc -O2 -pthread tests/litmus/hw/online_sb.c -o /tmp/sb0
 *   cc -O2 -pthread -DFENCE=1 tests/litmus/hw/online_sb.c -o /tmp/sb1
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#ifndef FENCE
#define FENCE 0
#endif
#define N 2000000
static _Atomic int P, online;
static _Atomic unsigned phase_w, phase_a;   /* round each thread has finished */
static _Atomic unsigned start;
static int r_online[N], r_p[N];

static void *ap(void *a)
{
    (void)a;
    for (unsigned i = 0; i < N; i++) {
        while (atomic_load_explicit(&start, memory_order_acquire) != i + 1)
            ;
        atomic_store_explicit(&online, 1, memory_order_release);
        if (FENCE)
            atomic_thread_fence(memory_order_seq_cst);
        r_p[i] = atomic_load_explicit(&P, memory_order_acquire);
        atomic_store_explicit(&phase_a, i + 1, memory_order_release);
    }
    return NULL;
}

int main(void)
{
    pthread_t t;
    pthread_create(&t, NULL, ap, NULL);
    for (unsigned i = 0; i < N; i++) {
        atomic_store(&P, 0);
        atomic_store(&online, 0);
        atomic_store_explicit(&start, i + 1, memory_order_release);   /* go */
        atomic_store_explicit(&P, 1, memory_order_release);           /* unlink */
        if (FENCE)
            atomic_thread_fence(memory_order_seq_cst);
        r_online[i] = atomic_load_explicit(&online, memory_order_acquire);
        while (atomic_load_explicit(&phase_a, memory_order_acquire) != i + 1)
            ;
    }
    pthread_join(t, NULL);
    long bad = 0;
    for (unsigned i = 0; i < N; i++)
        bad += r_online[i] == 0 && r_p[i] == 0;
    printf("FENCE=%d forbidden outcome (online=0, P=0): %ld of %d\n", FENCE, bad, N);
    return 0;
}
