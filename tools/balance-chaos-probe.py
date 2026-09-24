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
6 s, each with a 1.5 s bound. Per boot it prints

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
    while (rounds < 40 && clock_now_ns() - start < 6000000000ull) {
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

'''
ANCHOR_CALL = '    bool r = sched_balance_pull_pinned(reason);\n'
PROBE_CALL = '    (void)sched_balance_pull_pinned;\n    (void)reason;\n    bool r = bprobe_pinned();   /* BPROBE */\n'

EDITS = [(ANCHOR_FN, PROBE_FN + ANCHOR_FN), (ANCHOR_CALL, PROBE_CALL)]


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def apply():
    if os.path.exists(STAMP):
        sys.exit('already applied')
    if subprocess.run(['git', 'status', '--porcelain', '--', TARGET], capture_output=True, text=True).stdout.strip():
        sys.exit(f'{TARGET} has uncommitted changes')
    s = open(TARGET).read()
    for a, _ in EDITS:
        if s.count(a) != 1:
            sys.exit(f'{TARGET}: anchor not found exactly once: {a[:50]!r}')
    shutil.copyfile(TARGET, TARGET + BACKUP)
    try:
        for a, b in EDITS:
            s = s.replace(a, b)
        open(TARGET, 'w').write(s)
        open(STAMP, 'w').write(sha(TARGET) + '\n')
    except BaseException:
        shutil.move(TARGET + BACKUP, TARGET)
        os.utime(TARGET, None)
        if os.path.exists(STAMP):
            os.remove(STAMP)
        raise
    print('applied')


def revert():
    if not os.path.exists(STAMP):
        sys.exit('not applied')
    if sha(TARGET) != open(STAMP).read().strip():
        sys.exit(f'{TARGET} changed since apply; restore by hand from {TARGET + BACKUP}')
    shutil.move(TARGET + BACKUP, TARGET)
    os.utime(TARGET, None)
    os.remove(STAMP)
    print('reverted')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '', lambda: sys.exit(__doc__))()
