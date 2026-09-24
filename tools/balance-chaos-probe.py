#!/usr/bin/env python3
"""
balance-chaos-probe.py -- why does sched-balance-pull fail under the chaos migrator?

`sched-balance-pull` (kernel/scheduler/smptest.c) creates two workers per
CPU, releases every other one to spin, and requires that within 3 s the
released workers are running on as many CPUs as there are of them. It
passes in every plain boot and has failed five times in two days in CI's
chaos boot (`make test-chaos`, `docs/testing/flakes.md`).

The question is which of two things the chaos boot does to it: slows the
spread (the test's bound is too tight for a machine being shuffled), or
makes it impossible (something leaves two spinners on one CPU where no
migrator may separate them -- S26 forbids moving a PREEMPTED thread, and
two compute-bound threads sharing a CPU are always preempted).

This replaces the test's body, for the probe only, with repeated rounds
of the same scenario inside the self-test watchdog: up to 40 rounds or
3 s, each with a 1.5 s bound. It then asks the same of USER threads:
`init --probe spin:N` (added to init for the probe) runs N native
threads spinning in user mode, and the kernel side samples every 10 ms
for 4 s whether two share a CPU and for how long:

    UPROBE summary: N user spinners on N CPUs, S samples, shared in K;
      sharings P (queued one preempted at onset: Q), longest M ms

Last, the pair made on purpose (PPROBE): two spinners pinned to one CPU
until the queued one has been preempted, then widened to every CPU --
once spinning, once yielding:

    PPROBE spinning pair on cpu C: queued one preempted at widen 1;
      separated NO after 1000 ms; balancer pulls +0, refused not-ready +R Per boot it prints

    BPROBE round R spread in M ms
    BPROBE round R MISS after 1500 ms: cpus used U of N
    BPROBE   worker I: last cpu C, state S, preempted P, queue cpu Q
    BPROBE   chaos migrated +X, balancer scans +S, pulls +Y, refused not-ready +Z, gap +G
    BPROBE summary: rounds R, spread K, missed J, slowest M ms (chaos=0|1)

and returns success, so the rest of the boot runs. The same probe in a
plain debug image is the control: the two builds differ only in
SCHED_CHAOS.

Usage:

    python3 tools/balance-chaos-probe.py apply
    gmake ARCH=x86_64 test-chaos; grep BPROBE out/x86_64-debug-chaos/boot-test-chaos.log
    gmake ARCH=x86_64 test;       grep BPROBE out/x86_64-debug/boot-test.log
    python3 tools/balance-chaos-probe.py revert

`apply` refuses a file with uncommitted changes and edits nothing if an
anchor is missing (restoring on any failure part-way); `revert` restores
its snapshot, refuses if the file changed since the apply, and touches it
so make rebuilds.
"""

import hashlib
import os
import shutil
import subprocess
import sys

TARGET = 'kernel/scheduler/smptest.c'
BACKUP = '.balance-chaos-probe.orig'
STAMP = '.balance-chaos-probe.applied'

# The probe goes in front of the test's wrapper; the wrapper calls it.
ANCHOR_FN = 'bool selftest_sched_balance_pull(const char **reason)\n{\n'
PROBE_FN = r'''/* --- BPROBE (tools/balance-chaos-probe.py; not for merge) --- */
static bool bprobe_round(unsigned round, uint64_t bound_ms, uint64_t *took_ms)
{
    unsigned n = cpu_count();
    unsigned count = n * 2, runners = n, made = 0;
    static struct bal_worker w[CONFIG_MAX_CPUS * 2];
    static struct thread *t[CONFIG_MAX_CPUS * 2];
    const char *why = NULL;
    if (!bal_create_blocked(w, t, count, &made, &why)) {
        kinfo("BPROBE round %u setup failed: %s", round, why);
        bal_stop_all(w, t, made);
        return true;
    }
    struct sched_balance_stats b0, b1;
    sched_balance_stats(&b0);
    uint64_t c0 = 0, c1 = 0, r0 = 0, r1 = 0;
#if CONFIG_SCHED_CHAOS
    sched_chaos_stats(&c0, &r0);
#endif
    uint64_t start = clock_now_ns();
    for (unsigned i = 0; i < count; i += 2) {
        w[i].runs = 1;
        complete(&w[i].release);
    }
    uint64_t deadline = clock_deadline_ns(bound_ms * 1000000ull);
    unsigned used = 0;
    while (!clock_deadline_passed(deadline)) {
        used = bal_cpus_used(w, count, 2);
        if (used >= runners)
            break;
        thread_sleep_ms(5);
    }
    used = bal_cpus_used(w, count, 2);
    *took_ms = (clock_now_ns() - start) / 1000000ull;
    bool ok = used >= runners;
    if (!ok) {
        kinfo("BPROBE round %u MISS after %llu ms: cpus used %u of %u", round, (unsigned long long)*took_ms, used, n);
        for (unsigned i = 0; i < count; i += 2)
            kinfo("BPROBE   worker %u: last cpu %u, state %u, preempted %u, queue cpu %d", i,
                  __atomic_load_n(&w[i].cpu, __ATOMIC_RELAXED), (unsigned)__atomic_load_n(&t[i]->state, __ATOMIC_RELAXED),
                  (__atomic_load_n(&t[i]->flags, __ATOMIC_RELAXED) & THREAD_FLAG_PREEMPTED) ? 1u : 0u,
                  __atomic_load_n(&t[i]->cpu, __ATOMIC_RELAXED));
        sched_balance_stats(&b1);
#if CONFIG_SCHED_CHAOS
        sched_chaos_stats(&c1, &r1);
#endif
        /* A queue whose only spare thread is PREEMPTED offers nothing:
         * pick_migratable returns NULL and the pull reports NOT_READY. */
        kinfo("BPROBE   chaos migrated +%llu, balancer scans +%llu, pulls +%llu, refused not-ready +%llu, gap +%llu",
              (unsigned long long)(c1 - c0), (unsigned long long)(b1.scans - b0.scans),
              (unsigned long long)(b1.pulls - b0.pulls),
              (unsigned long long)(b1.refused[SCHED_MIGRATE_NOT_READY] - b0.refused[SCHED_MIGRATE_NOT_READY]),
              (unsigned long long)(b1.refused[SCHED_MIGRATE_GAP] - b0.refused[SCHED_MIGRATE_GAP]));
    } else {
        kinfo("BPROBE round %u spread in %llu ms", round, (unsigned long long)*took_ms);
    }
    (void)r0;
    (void)r1;
    bal_stop_all(w, t, count);
    return ok;
}

static bool bprobe_pinned(void)
{
    if (cpu_count() < 2)
        return true;
    uint64_t start = clock_now_ns(), slowest = 0;
    unsigned rounds = 0, spread = 0, missed = 0;
    while (rounds < 40 && clock_now_ns() - start < 3000000000ull) {
        uint64_t took = 0;
        if (bprobe_round(rounds, 1500, &took))
            spread++;
        else
            missed++;
        if (took > slowest)
            slowest = took;
        rounds++;
    }
    kinfo("BPROBE summary: rounds %u, spread %u, missed %u, slowest %llu ms (chaos=%d)", rounds, spread, missed,
          (unsigned long long)slowest, CONFIG_SCHED_CHAOS ? 1 : 0);
    return true;
}

/* The pair, made on purpose: two spinners pinned to one CPU until the
 * queued one has been preempted, then widened to every CPU. If S26 holds
 * the queued one, no pull can separate them however long an idle CPU
 * looks; a yielding pair is never PREEMPTED and should be separated. */
static void pprobe(unsigned yielding)
{
    unsigned n = cpu_count(), self = arch_cpu_id();
    unsigned c = (self + 1) % n;
    static struct bal_worker w[2];
    struct thread *t[2] = { NULL, NULL };
    for (unsigned i = 0; i < 2; i++) {
        memset(&w[i], 0, sizeof(w[i]));
        completion_init(&w[i].started, "pp-start");
        completion_init(&w[i].release, "pp-rel");
        w[i].runs = 1;
        w[i].yielding = yielding;
        t[i] = thread_create_on(bal_worker_main, &w[i], "pp", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        if (t[i] == NULL)
            return;
        wait_for_completion(&w[i].started);
    }
    for (unsigned i = 0; i < 2; i++)
        complete(&w[i].release);
    thread_sleep_ms(50);   /* they share CPU c: each has been preempted by the other by now */
    unsigned preempted = 0;
    for (unsigned i = 0; i < 2; i++)
        if (__atomic_load_n(&t[i]->state, __ATOMIC_RELAXED) == THREAD_READY &&
            (__atomic_load_n(&t[i]->flags, __ATOMIC_RELAXED) & THREAD_FLAG_PREEMPTED))
            preempted++;
    cpumask_t all = 0;
    for (unsigned k = 0; k < n; k++)
        all |= CPUMASK_OF(k);
    for (unsigned i = 0; i < 2; i++)
        thread_set_affinity(t[i], all);
    struct sched_balance_stats b0, b1;
    sched_balance_stats(&b0);
    uint64_t start = clock_now_ns();
    bool apart = false;
    while (clock_now_ns() - start < 1000000000ull) {
        if (__atomic_load_n(&w[0].cpu, __ATOMIC_RELAXED) != __atomic_load_n(&w[1].cpu, __ATOMIC_RELAXED)) {
            apart = true;
            break;
        }
        thread_sleep_ms(2);
    }
    uint64_t took = (clock_now_ns() - start) / 1000000ull;
    sched_balance_stats(&b1);
    kinfo("PPROBE %s pair on cpu %u: queued one preempted at widen %u; separated %s after %llu ms; "
          "balancer pulls +%llu, refused not-ready +%llu (chaos=%d)",
          yielding ? "yielding" : "spinning", c, preempted, apart ? "yes" : "NO", (unsigned long long)took,
          (unsigned long long)(b1.pulls - b0.pulls),
          (unsigned long long)(b1.refused[SCHED_MIGRATE_NOT_READY] - b0.refused[SCHED_MIGRATE_NOT_READY]),
          CONFIG_SCHED_CHAOS ? 1 : 0);
    for (unsigned i = 0; i < 2; i++)
        __atomic_store_n(&w[i].stop, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 2; i++)
        thread_join(t[i]);
}

/* The same question for USER threads: `init --probe spin:N` runs N native
 * threads that spin in user mode forever. Sampled every 10 ms for 4 s:
 * how often two of them share a CPU, how long each sharing lasts, and
 * whether the queued one of a pair is PREEMPTED. */
static void uprobe(void)
{
    unsigned n = cpu_count();
    const void *image;
    size_t size;
    if (n < 2 || !bootarchive_find("init", &image, &size))
        return;
    char kind[16];
    ksnprintf(kind, sizeof(kind), "spin:%u", n);
    const char *argv[] = { "init", "--probe", kind, NULL };
    struct process *p = NULL;
    if (process_create_from_elf(image, size, argv[0], argv, NULL, NULL, &p) != 0) {
        kinfo("UPROBE spawn failed");
        return;
    }
    uint64_t t0 = clock_now_ns();
    while (__atomic_load_n(&p->nr_threads, __ATOMIC_RELAXED) < n && clock_now_ns() - t0 < 2000000000ull)
        thread_sleep_ms(5);
    unsigned pairs = 0, samples = 0, shared_samples = 0, preempted_pairs = 0;
    uint64_t run_start = 0, longest = 0;
    bool in_run = false;
    uint64_t start = clock_now_ns();
    while (clock_now_ns() - start < 4000000000ull) {
        cpumask_t seen = 0;
        unsigned threads = 0;
        bool preempted_queued = false;
        unsigned per_cpu[CONFIG_MAX_CPUS] = { 0 };
        arch_irq_state_t s = spin_lock_irqsave(&p->lock);
        struct thread *t;
        list_for_each_entry(t, &p->threads, proc_link) {
            int c = __atomic_load_n(&t->cpu, __ATOMIC_RELAXED);
            if (c >= 0 && (unsigned)c < n) {
                seen |= CPUMASK_OF((unsigned)c);
                per_cpu[c]++;
            }
            if (__atomic_load_n(&t->state, __ATOMIC_RELAXED) == THREAD_READY &&
                (__atomic_load_n(&t->flags, __ATOMIC_RELAXED) & THREAD_FLAG_PREEMPTED))
                preempted_queued = true;
            threads++;
        }
        spin_unlock_irqrestore(&p->lock, s);
        unsigned distinct = 0;
        for (unsigned c = 0; c < n; c++)
            if (seen & CPUMASK_OF(c))
                distinct++;
        samples++;
        bool shared = threads >= n && distinct < n;
        uint64_t now = clock_now_ns();
        if (shared) {
            shared_samples++;
            if (!in_run) {
                in_run = true;
                run_start = now;
                pairs++;
                if (preempted_queued)
                    preempted_pairs++;
            }
        } else if (in_run) {
            in_run = false;
            if (now - run_start > longest)
                longest = now - run_start;
        }
        thread_sleep_ms(10);
    }
    if (in_run && clock_now_ns() - run_start > longest)
        longest = clock_now_ns() - run_start;
    kinfo("UPROBE summary: %u user spinners on %u CPUs, %u samples, shared in %u; sharings %u (queued one preempted at onset: %u), "
          "longest %llu ms%s (chaos=%d)", n, n, samples, shared_samples, pairs, preempted_pairs,
          (unsigned long long)(longest / 1000000ull), in_run ? ", still sharing at the end" : "", CONFIG_SCHED_CHAOS ? 1 : 0);
    signal_send(p, SIGKILL, NULL);
    (void)process_wait_exit(p);
    process_put(p);
}

'''
ANCHOR_CALL = '    bool r = sched_balance_pull_pinned(reason);\n'
PROBE_CALL = '    (void)sched_balance_pull_pinned;\n    (void)reason;\n    bool r = bprobe_pinned();   /* BPROBE */\n    uprobe();\n    pprobe(0);\n    pprobe(1);\n'

ANCHOR_INC = '#include <kernel/acpi.h>\n'
PROBE_INC = '#include <kernel/acpi.h>\n#include <kernel/bootarchive.h>   /* BPROBE */\n#include <kernel/process.h>\n#include <kernel/signal.h>\n'

INIT = 'userland/init/init.c'
INIT_ANCHOR = '    if (strncmp(kind, "cwd-is:", 7) == 0) {\n'
INIT_PROBE = '''    if (strncmp(kind, "spin:", 5) == 0) {   /* UPROBE (tools/balance-chaos-probe.py; not for merge) */
        unsigned n = (unsigned)strtoul(kind + 5, NULL, 10);
        static cosmo_thread_t st[64];
        for (unsigned i = 1; i < n && i < 64; i++)
            if (cosmo_thread_start(&st[i], uprobe_spin, NULL, 16 * 1024) != 0)
                return 90;
        uprobe_spin(NULL);
        return 0;
    }
'''
INIT_FN_ANCHOR = 'static int filter_case(const char *kind)\n'
INIT_FN = '''static void *uprobe_spin(void *arg)   /* UPROBE */
{
    for (volatile unsigned long k = 0;; k++)
        ;
    return arg;
}

'''

EDITS = [(ANCHOR_INC, PROBE_INC), (ANCHOR_FN, PROBE_FN + ANCHOR_FN), (ANCHOR_CALL, PROBE_CALL)]
INIT_EDITS = [(INIT_FN_ANCHOR, INIT_FN + INIT_FN_ANCHOR), (INIT_ANCHOR, INIT_PROBE + INIT_ANCHOR)]
FILES = [(TARGET, EDITS), (INIT, INIT_EDITS)]


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def apply():
    if os.path.exists(STAMP):
        sys.exit('already applied')
    for path, edits in FILES:
        if subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True).stdout.strip():
            sys.exit(f'{path} has uncommitted changes')
        s = open(path).read()
        for a, _ in edits:
            if s.count(a) != 1:
                sys.exit(f'{path}: anchor not found exactly once: {a[:50]!r}')
    done, stamp = [], []
    try:
        for path, edits in FILES:
            shutil.copyfile(path, path + BACKUP)
            done.append(path)
            s = open(path).read()
            for a, b in edits:
                s = s.replace(a, b)
            open(path, 'w').write(s)
            stamp.append(f'{path} {sha(path)}')
        open(STAMP, 'w').write('\n'.join(stamp) + '\n')
    except BaseException:
        for path in done:
            shutil.move(path + BACKUP, path)
            os.utime(path, None)
        if os.path.exists(STAMP):
            os.remove(STAMP)
        raise
    print('applied')


def revert():
    if not os.path.exists(STAMP):
        sys.exit('not applied')
    for line in open(STAMP).read().split('\n'):
        if line:
            path, digest = line.split()
            if sha(path) != digest:
                sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
    for path, _ in FILES:
        shutil.move(path + BACKUP, path)
        os.utime(path, None)
    os.remove(STAMP)
    print('reverted')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '', lambda: sys.exit(__doc__))()
