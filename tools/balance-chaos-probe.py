#!/usr/bin/env python3
"""
balance-chaos-probe.py -- why does sched-balance-pull fail under the chaos migrator?

`sched-balance-pull` (kernel/scheduler/smptest.c) creates two workers per
CPU, releases every other one to spin, and requires that within 3 s the
released workers are running on as many CPUs as there are of them. It
passes in every plain boot and failed five times in two days in CI's
chaos boot (`make test-chaos`, `docs/testing/flakes.md`).

The question is which of two things the chaos boot does to it: slows the
spread (the test's bound is too tight for a machine being shuffled), or
makes it impossible (something leaves two spinners on one CPU where no
migrator may separate them -- S26 forbids moving a PREEMPTED thread, and
two compute-bound threads sharing a CPU are always preempted).

Three measurements, each in place of one balance test's body so that
each has that test's own watchdog (8 s) to itself, and each returning
success so the rest of the boot runs:

1. BPROBE, in `sched-balance-pull`: the test's scenario repeated for up
   to 40 rounds or 3 s, each with a 1.5 s bound; on a miss, a dump of
   every released worker and the balancer's and migrator's counters.
   A round whose workers could not be created is counted as such, not
   as a spread.

       BPROBE round R spread in M ms
       BPROBE round R MISS after 1500 ms: cpus used U of N
       BPROBE   worker I: last cpu C, state S, preempted P, queue cpu Q
       BPROBE   chaos migrated +X, balancer scans +S, pulls +Y, refused not-ready +Z, gap +G
       BPROBE summary: rounds R, spread K, missed J, setup failed F, slowest M ms (chaos=0|1)

2. UPROBE, in `sched-balance-hysteresis`: `init --probe spin:N` (added
   to init for the probe) runs N native threads spinning in user mode.
   Once all N exist and each is placed on a CPU, it samples every 10 ms
   for 3 s whether some CPU holds two of them, and whether a thread
   queued on *that* CPU is PREEMPTED. If the workload never forms, it
   says so and measures nothing.

       UPROBE summary: N user spinners, S samples, shared in K; sharings P
         (a preempted thread queued on the shared CPU at onset: Q), longest M ms

3. PPROBE, in `sched-balance-affinity`: the pair, made on purpose. Two
   workers pinned to one CPU; the probe waits (up to 1 s) until the pair
   has settled -- both have run and, for a spinning pair, the queued one
   is PREEMPTED and neither is queued unmarked -- then widens both to every CPU and waits up to 1 s for them to
   run on two CPUs. A pair whose premise was never seen is reported
   inconclusive rather than measured.

       PPROBE spinning pair on cpu C: preempted at widen 1; separated NO
         after 1000 ms; balancer pulls +0, refused not-ready +R

The same probe in a plain debug image is the control: the two builds
differ only in SCHED_CHAOS.

Usage:

    python3 tools/balance-chaos-probe.py apply
    gmake ARCH=x86_64 test-chaos; grep 'BPROBE\\|UPROBE\\|PPROBE' out/x86_64-debug-chaos/boot-test-chaos.log
    gmake ARCH=x86_64 test;       grep 'BPROBE\\|UPROBE\\|PPROBE' out/x86_64-debug/boot-test.log
    python3 tools/balance-chaos-probe.py revert

`apply` refuses a file with uncommitted changes and edits nothing if an
anchor is missing, restoring every file on a failure part-way. `revert`
checks every file's hash first, then restores all of them from copies and
removes the backups only once every file is restored, so a failure
part-way leaves every backup in place for a retry.
"""

import hashlib
import os
import shutil
import subprocess
import sys

TARGET = 'kernel/scheduler/smptest.c'
INIT = 'userland/init/init.c'
BACKUP = '.balance-chaos-probe.orig'
STAMP = '.balance-chaos-probe.applied'

ANCHOR_INC = '#include <kernel/acpi.h>\n'
PROBE_INC = ANCHOR_INC + '#include <kernel/bootarchive.h>   /* BPROBE */\n#include <kernel/process.h>\n#include <kernel/signal.h>\n'

ANCHOR_FN = 'bool selftest_sched_balance_pull(const char **reason)\n{\n'
PROBE_FN = r'''/* --- BPROBE / UPROBE / PPROBE (tools/balance-chaos-probe.py; not for merge) --- */
enum bprobe_outcome { BP_SPREAD, BP_MISS, BP_SETUP };

static enum bprobe_outcome bprobe_round(unsigned round, uint64_t bound_ms, uint64_t *took_ms)
{
    unsigned n = cpu_count();
    unsigned count = n * 2, runners = n, made = 0;
    static struct bal_worker w[CONFIG_MAX_CPUS * 2];
    static struct thread *t[CONFIG_MAX_CPUS * 2];
    const char *why = NULL;
    *took_ms = 0;
    if (!bal_create_blocked(w, t, count, &made, &why)) {
        kinfo("BPROBE round %u setup failed: %s", round, why);
        bal_stop_all(w, t, made);
        return BP_SETUP;   /* unmeasured: neither a spread nor a miss */
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
    return ok ? BP_SPREAD : BP_MISS;
}

static void bprobe(void)
{
    if (cpu_count() < 2)
        return;
    uint64_t start = clock_now_ns(), slowest = 0;
    unsigned rounds = 0, spread = 0, missed = 0, setup = 0;
    while (rounds < 40 && clock_now_ns() - start < 3000000000ull) {
        uint64_t took = 0;
        enum bprobe_outcome o = bprobe_round(rounds, 1500, &took);
        if (o == BP_SPREAD)
            spread++;
        else if (o == BP_MISS)
            missed++;
        else
            setup++;
        if (o != BP_SETUP && took > slowest)
            slowest = took;
        rounds++;
    }
    kinfo("BPROBE summary: rounds %u, spread %u, missed %u, setup failed %u, slowest %llu ms (chaos=%d)", rounds,
          spread, missed, setup, (unsigned long long)slowest, CONFIG_SCHED_CHAOS ? 1 : 0);
}

/* USER threads: `init --probe spin:N`. Sharing is read per CPU -- some CPU
 * holding two of the spinners -- and a preemption is attributed only when
 * the preempted thread is queued on that same CPU. */
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
    /* The workload must exist before its samples mean anything: all N
     * threads, each placed on a CPU. */
    bool formed = false;
    uint64_t t0 = clock_now_ns();
    while (!formed && clock_now_ns() - t0 < 2000000000ull) {
        unsigned placed = 0;
        arch_irq_state_t s = spin_lock_irqsave(&p->lock);
        struct thread *t;
        list_for_each_entry(t, &p->threads, proc_link) {
            int c = __atomic_load_n(&t->cpu, __ATOMIC_RELAXED);
            enum thread_state st = __atomic_load_n(&t->state, __ATOMIC_RELAXED);
            if (c >= 0 && (unsigned)c < n && (st == THREAD_RUNNING || st == THREAD_READY))
                placed++;
        }
        spin_unlock_irqrestore(&p->lock, s);
        formed = placed >= n;
        if (!formed)
            thread_sleep_ms(5);
    }
    if (!formed) {
        kinfo("UPROBE workload never formed: fewer than %u spinners placed after 2 s; nothing measured", n);
    } else {
        unsigned pairs = 0, samples = 0, shared_samples = 0, preempted_pairs = 0;
        uint64_t run_start = 0, longest = 0;
        bool in_run = false;
        uint64_t start = clock_now_ns();
        while (clock_now_ns() - start < 3000000000ull) {
            unsigned per_cpu[CONFIG_MAX_CPUS] = { 0 };
            cpumask_t preempted_on = 0;   /* CPUs with a preempted spinner queued */
            arch_irq_state_t s = spin_lock_irqsave(&p->lock);
            struct thread *t;
            list_for_each_entry(t, &p->threads, proc_link) {
                int c = __atomic_load_n(&t->cpu, __ATOMIC_RELAXED);
                if (c < 0 || (unsigned)c >= n)
                    continue;
                per_cpu[c]++;
                if (__atomic_load_n(&t->state, __ATOMIC_RELAXED) == THREAD_READY &&
                    (__atomic_load_n(&t->flags, __ATOMIC_RELAXED) & THREAD_FLAG_PREEMPTED))
                    preempted_on |= CPUMASK_OF((unsigned)c);
            }
            spin_unlock_irqrestore(&p->lock, s);
            bool shared = false, shared_preempted = false;
            for (unsigned c = 0; c < n; c++)
                if (per_cpu[c] >= 2) {
                    shared = true;
                    if (preempted_on & CPUMASK_OF(c))
                        shared_preempted = true;
                }
            samples++;
            uint64_t now = clock_now_ns();
            if (shared) {
                shared_samples++;
                if (!in_run) {
                    in_run = true;
                    run_start = now;
                    pairs++;
                    if (shared_preempted)
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
        kinfo("UPROBE summary: %u user spinners, %u samples, shared in %u; sharings %u "
              "(a preempted thread queued on the shared CPU at onset: %u), longest %llu ms%s (chaos=%d)",
              n, samples, shared_samples, pairs, preempted_pairs, (unsigned long long)(longest / 1000000ull),
              in_run ? ", still sharing at the end" : "", CONFIG_SCHED_CHAOS ? 1 : 0);
    }
    signal_send(p, SIGKILL, NULL);
    (void)process_wait_exit(p);
    process_put(p);
}

/* The pair, made on purpose. Every exit stops and joins what it made. */
static void pprobe_one(unsigned yielding)
{
    unsigned n = cpu_count(), self = arch_cpu_id();
    unsigned c = (self + 1) % n;
    static struct bal_worker w[2];
    struct thread *t[2] = { NULL, NULL };
    unsigned made = 0;
    for (unsigned i = 0; i < 2; i++) {
        memset(&w[i], 0, sizeof(w[i]));
        completion_init(&w[i].started, "pp-start");
        completion_init(&w[i].release, "pp-rel");
        w[i].runs = 1;
        w[i].yielding = yielding;
        t[i] = thread_create_on(bal_worker_main, &w[i], "pp", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        if (t[i] == NULL)
            break;
        made++;
        wait_for_completion(&w[i].started);
    }
    for (unsigned i = 0; i < made; i++)
        complete(&w[i].release);
    unsigned preempted = 0;
    if (made < 2) {
        kinfo("PPROBE %s pair: a worker could not be created; nothing measured", yielding ? "yielding" : "spinning");
        goto out;
    }
    /* The premise, observed rather than assumed: the pair has settled (below). */
    bool premise = false;
    uint64_t t0 = clock_now_ns();
    while (!premise && clock_now_ns() - t0 < 1000000000ull) {
        preempted = 0;
        for (unsigned i = 0; i < 2; i++)
            if (__atomic_load_n(&t[i]->state, __ATOMIC_RELAXED) == THREAD_READY &&
                (__atomic_load_n(&t[i]->flags, __ATOMIC_RELAXED) & THREAD_FLAG_PREEMPTED))
                preempted++;
        bool both_ran = __atomic_load_n(&t[0]->switches, __ATOMIC_RELAXED) > 1 &&
                        __atomic_load_n(&t[1]->switches, __ATOMIC_RELAXED) > 1;
        /* A spinning pair must have SETTLED: both ran, neither queued
         * unmarked -- a worker that has never run is movable (the
         * balance-movable unit's build found "one preempted" too weak). */
        bool queued_unmarked = false;
        for (unsigned i = 0; i < 2; i++)
            if (__atomic_load_n(&t[i]->state, __ATOMIC_RELAXED) == THREAD_READY &&
                !(__atomic_load_n(&t[i]->flags, __ATOMIC_RELAXED) & THREAD_FLAG_PREEMPTED))
                queued_unmarked = true;
        premise = yielding ? both_ran : (both_ran && preempted > 0 && !queued_unmarked);
        if (!premise)
            thread_sleep_ms(2);
    }
    if (!premise) {
        kinfo("PPROBE %s pair on cpu %u: premise not seen in 1 s (preempted %u); inconclusive",
              yielding ? "yielding" : "spinning", c, preempted);
        goto out;
    }
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
    kinfo("PPROBE %s pair on cpu %u: preempted at widen %u; separated %s after %llu ms; "
          "balancer pulls +%llu, refused not-ready +%llu (chaos=%d)",
          yielding ? "yielding" : "spinning", c, preempted, apart ? "yes" : "NO", (unsigned long long)took,
          (unsigned long long)(b1.pulls - b0.pulls),
          (unsigned long long)(b1.refused[SCHED_MIGRATE_NOT_READY] - b0.refused[SCHED_MIGRATE_NOT_READY]),
          CONFIG_SCHED_CHAOS ? 1 : 0);
out:
    for (unsigned i = 0; i < made; i++)
        __atomic_store_n(&w[i].stop, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < made; i++)
        thread_join(t[i]);
}

static void pprobe(void)
{
    if (cpu_count() < 2)
        return;
    pprobe_one(0);
    pprobe_one(1);
}

'''
# Each measurement replaces one balance test's call, so each has that
# test's watchdog to itself.
CALLS = [
    ('    bool r = sched_balance_pull_pinned(reason);\n',
     '    (void)sched_balance_pull_pinned;\n    (void)reason;\n    bprobe();   /* BPROBE */\n    bool r = true;\n'),
    ('    bool r = sched_balance_hysteresis_pinned(reason);\n',
     '    (void)sched_balance_hysteresis_pinned;\n    (void)reason;\n    uprobe();   /* UPROBE */\n    bool r = true;\n'),
    ('    bool r = sched_balance_affinity_pinned(reason);\n',
     '    (void)sched_balance_affinity_pinned;\n    (void)reason;\n    pprobe();   /* PPROBE */\n    bool r = true;\n'),
]

INIT_FN_ANCHOR = 'static int filter_case(const char *kind)\n'
INIT_FN = '''static void *uprobe_spin(void *arg)   /* UPROBE (tools/balance-chaos-probe.py; not for merge) */
{
    for (volatile unsigned long k = 0;; k++)
        ;
    return arg;
}

'''
INIT_ANCHOR = '    if (strncmp(kind, "cwd-is:", 7) == 0) {\n'
INIT_PROBE = '''    if (strncmp(kind, "spin:", 5) == 0) {   /* UPROBE */
        unsigned n = (unsigned)strtoul(kind + 5, NULL, 10);
        static cosmo_thread_t st[64];
        for (unsigned i = 1; i < n && i < 64; i++)
            if (cosmo_thread_start(&st[i], uprobe_spin, NULL, 16 * 1024) != 0)
                return 90;
        uprobe_spin(NULL);
        return 0;
    }
'''

FILES = [
    (TARGET, [(ANCHOR_INC, PROBE_INC), (ANCHOR_FN, PROBE_FN + ANCHOR_FN)] + CALLS),
    (INIT, [(INIT_FN_ANCHOR, INIT_FN + INIT_FN_ANCHOR), (INIT_ANCHOR, INIT_PROBE + INIT_ANCHOR)]),
]


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
            # Already restored by an earlier revert that failed part-way:
            # the file matches its backup, and a retry finishes the job.
            restored = os.path.exists(path + BACKUP) and sha(path) == sha(path + BACKUP)
            if sha(path) != digest and not restored:
                sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
    # Restore every file from a copy first; drop the backups only once all
    # are restored, so a failure part-way leaves every backup for a retry.
    for path, _ in FILES:
        shutil.copyfile(path + BACKUP, path)
        os.utime(path, None)
    for path, _ in FILES:
        os.remove(path + BACKUP)
    os.remove(STAMP)
    print('reverted')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '', lambda: sys.exit(__doc__))()
